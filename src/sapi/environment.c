/* Environment sandbox adapted from FrankenPHP (MIT, see ../php/LICENSE.frankenphp).
 * Process environment is snapshotted before PHP execution threads start. PHP
 * putenv writes only thread-local Zend arrays; libc environ is never mutated. */
#include "environment.h"
#include <php_variables.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;
static char **process_env;
static size_t process_env_count;
typedef struct {
    zend_string *name, *value;
} hp_process_env_strings_t;
typedef struct {
    size_t index;
    bool valid_seen;
} hp_process_env_slot_t;
static hp_process_env_strings_t *process_env_strings;
static size_t process_env_import_count;
static _Thread_local HashTable *sandbox, *prepared, *worker;

int hp_env_init(void)
{
    while (environ[process_env_count]) ++process_env_count;
    process_env = calloc(process_env_count + 1, sizeof(*process_env));
    if (!process_env) return -1;
    for (size_t i = 0; i < process_env_count; ++i) {
        if (!(process_env[i] = strdup(environ[i]))) {
            hp_env_shutdown();
            return -1;
        }
    }
    return 0;
}

/* PHP's persistent fpemalloc ignores its fallible argument in this ABI.
 * Mirror zend_string_alloc's malloc-backed layout so startup can cleanly fail.
 * Hash before publishing: imported immutable strings never mutate across ZTS. */
static zend_string *process_string(const char *value, size_t len)
{
    if (len > SIZE_MAX - (_ZSTR_STRUCT_SIZE(0) + ZEND_MM_ALIGNMENT - 1)) return NULL;
    zend_string *s = malloc(ZEND_MM_ALIGNED_SIZE(_ZSTR_STRUCT_SIZE(len)));
    if (!s) return NULL;
    GC_SET_REFCOUNT(s, 1);
    GC_TYPE_INFO(s) = GC_STRING | (IS_STR_PERSISTENT << GC_FLAGS_SHIFT);
    ZSTR_H(s) = 0;
    ZSTR_LEN(s) = len;
    memcpy(ZSTR_VAL(s), value, len);
    ZSTR_VAL(s)[len] = '\0';
    zend_string_hash_val(s);
    GC_ADD_FLAGS(s, IS_STR_INTERNED);
    return s;
}

int hp_env_startup(void)
{
    if (process_env_strings) return 0;
    if (process_env_count > UINT32_MAX ||
        process_env_count > SIZE_MAX / sizeof(hp_process_env_strings_t) - 1) return -1;
    size_t slot_count = 2;
    while (slot_count / 2 < process_env_count) {
        if (slot_count > SIZE_MAX / (2 * sizeof(hp_process_env_slot_t))) return -1;
        slot_count *= 2;
    }
    hp_process_env_strings_t *entries = calloc(process_env_count + 1, sizeof(*entries));
    if (!entries) return -1;
    hp_process_env_slot_t *slots = calloc(slot_count, sizeof(*slots));
    if (!slots) { free(entries); return -1; }
    size_t count = 0;
    for (size_t i = 0; i < process_env_count; ++i) {
        const char *raw = process_env[i];
        if (!*raw) continue; /* Go os.Environ filters only empty raw entries. */
        const char *eq = strchr(raw, '=');
        bool valid = eq != NULL;
        zend_string *name = process_string(raw, valid ? (size_t)(eq - raw) : strlen(raw));
        if (!name) goto Failed;
        size_t slot = (size_t)ZSTR_H(name) & (slot_count - 1);
        while (slots[slot].index) {
            zend_string *prior = entries[slots[slot].index - 1].name;
            if (ZSTR_H(prior) == ZSTR_H(name) && ZSTR_LEN(prior) == ZSTR_LEN(name) &&
                !memcmp(ZSTR_VAL(prior), ZSTR_VAL(name), ZSTR_LEN(name))) break;
            slot = (slot + 1) & (slot_count - 1);
        }
        /* Go removes later NAME=VALUE duplicates, but retains every nonempty
         * malformed entry. The later raw imports then update the same key. */
        if (valid && slots[slot].valid_seen) { free(name); continue; }
        zend_string *value = process_string(valid ? eq + 1 : "", valid ? strlen(eq + 1) : 0);
        if (!value) { free(name); goto Failed; }
        if (slots[slot].index) {
            hp_process_env_strings_t *prior = entries + slots[slot].index - 1;
            free(name);
            free(prior->value);
            prior->value = value;
        } else {
            entries[count].name = name;
            entries[count].value = value;
            slots[slot].index = ++count;
        }
        if (valid) slots[slot].valid_seen = true;
    }
    free(slots);
    process_env_import_count = count;
    process_env_strings = entries;
    return 0;
Failed:
    for (size_t i = 0; i < count; ++i) {
        free(entries[i].name);
        free(entries[i].value);
    }
    free(slots);
    free(entries);
    return -1;
}

void hp_env_shutdown(void)
{
    /* No interpreter remains: these privately owned, interned-flagged strings
     * were never inserted into Zend's intern table and require explicit free. */
    if (process_env_strings) {
        for (size_t i = 0; i < process_env_import_count; ++i) {
            free(process_env_strings[i].name);
            free(process_env_strings[i].value);
        }
        free(process_env_strings);
        process_env_strings = NULL;
        process_env_import_count = 0;
    }
    if (process_env) {
        for (size_t i = 0; i < process_env_count; ++i) free(process_env[i]);
        free(process_env);
    }
    process_env = NULL;
    process_env_count = 0;
}

static const char *lookup_process(const char *name, size_t len)
{
    /* Public/SAPI getenv runs after hp_env_startup publishes this cache.
     * Scalar and full imports must see the same final malformed-entry updates. */
    for (size_t i = 0; i < process_env_import_count; ++i) {
        const hp_process_env_strings_t *entry = process_env_strings + i;
        if (ZSTR_LEN(entry->name) == len && !memcmp(name, ZSTR_VAL(entry->name), len))
            return ZSTR_VAL(entry->value);
    }
    return NULL;
}

static zval *lookup_overlay(const char *name, size_t len)
{
    HashTable *layers[] = {sandbox, prepared, worker};
    for (size_t i = 0; i < sizeof(layers) / sizeof(layers[0]); ++i) {
        if (layers[i]) {
            zval *entry = zend_hash_str_find(layers[i], name, len);
            if (entry) return entry;
        }
    }
    return NULL;
}

char *hp_env_get(const char *name, size_t len)
{
    zval *entry = lookup_overlay(name, len);
    if (entry) return Z_TYPE_P(entry) == IS_STRING ? Z_STRVAL_P(entry) : NULL;
    return (char *)lookup_process(name, len);
}

void hp_env_set(const char *name, const char *value)
{
    if (!prepared) prepared = zend_new_array(8);
    zval val;
    ZVAL_STRING(&val, value);
    zend_hash_str_update(prepared, name, strlen(name), &val);
}

void hp_env_bootstrapped(void)
{
    worker = prepared;
    prepared = NULL;
}

void hp_env_request_clear(void)
{
    if (prepared) zend_hash_release(prepared);
    prepared = NULL;
}

void hp_env_script_reset(void)
{
    hp_env_request_clear();
    if (sandbox) zend_hash_release(sandbox);
    if (worker) zend_hash_release(worker);
    sandbox = worker = NULL;
}

void hp_env_after_shutdown(void)
{
    /* Shutdown hooks can allocate a new sandbox, whose request heap was freed. */
    sandbox = prepared = worker = NULL;
}

static void import_process(zval *target)
{
    zend_hash_extend(Z_ARRVAL_P(target), (uint32_t)process_env_import_count, 0);
    for (size_t i = 0; i < process_env_import_count; ++i) {
        const hp_process_env_strings_t *entry = process_env_strings + i;
        zval value;
        ZVAL_STR(&value, entry->value);
        zend_hash_update(Z_ARRVAL_P(target), entry->name, &value);
    }
}

void hp_env_import(zval *target)
{
    import_process(target);
    /* Upstream intentionally preserves $_ENV between worker callbacks. It
     * must capture the boot environment, never the first request's overrides. */
    HashTable *env = worker ? worker : prepared;
    if (env) zend_hash_copy(Z_ARRVAL_P(target), env, (copy_ctor_func_t)zval_add_ref);
}

void hp_env_server_import(zval *target)
{
    import_process(target);
    if (worker) zend_hash_copy(Z_ARRVAL_P(target), worker, (copy_ctor_func_t)zval_add_ref);
    if (prepared) zend_hash_copy(Z_ARRVAL_P(target), prepared, (copy_ctor_func_t)zval_add_ref);
}

PHP_FUNCTION(hp_putenv)
{
    char *setting;
    size_t len;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(setting, len)
    ZEND_PARSE_PARAMETERS_END();
    if (!len || setting[0] == '=') {
        zend_argument_value_error(1, "must have a valid syntax");
        RETURN_THROWS();
    }
    if (!sandbox) sandbox = zend_new_array(0);
    char *nul = memchr(setting, '\0', len);
    if (nul) len = (size_t)(nul - setting);
    char *eq = memchr(setting, '=', len);
    zval val;
    if (eq) {
        size_t name_len = (size_t)(eq - setting);
        ZVAL_STRINGL(&val, eq + 1, len - name_len - 1);
        zend_hash_str_update(sandbox, setting, name_len, &val);
    } else {
        ZVAL_NULL(&val);
        zend_hash_str_update(sandbox, setting, len, &val);
    }
    RETURN_TRUE;
}

PHP_FUNCTION(hp_getenv)
{
    zend_string *name = NULL;
    bool local_only = false;
    ZEND_PARSE_PARAMETERS_START(0, 2)
        Z_PARAM_OPTIONAL
        Z_PARAM_STR_OR_NULL(name)
        Z_PARAM_BOOL(local_only)
    ZEND_PARSE_PARAMETERS_END();
    (void)local_only;
    if (name) {
        char *value = hp_env_get(ZSTR_VAL(name), ZSTR_LEN(name));
        if (value) RETURN_STRING(value);
        RETURN_FALSE;
    }
    array_init(return_value);
    hp_env_server_import(return_value);
    if (sandbox) {
        zend_string *key;
        zval *value;
        ZEND_HASH_FOREACH_STR_KEY_VAL(sandbox, key, value) {
            if (Z_TYPE_P(value) == IS_NULL) zend_hash_del(Z_ARRVAL_P(return_value), key);
            else {
                Z_TRY_ADDREF_P(value);
                zend_hash_update(Z_ARRVAL_P(return_value), key, value);
            }
        } ZEND_HASH_FOREACH_END();
    }
}

void hp_env_override_functions(void)
{
    zend_function *fn = zend_hash_str_find_ptr(CG(function_table), "putenv", sizeof("putenv") - 1);
    if (fn && fn->type == ZEND_INTERNAL_FUNCTION) fn->internal_function.handler = ZEND_FN(hp_putenv);
    fn = zend_hash_str_find_ptr(CG(function_table), "getenv", sizeof("getenv") - 1);
    if (fn && fn->type == ZEND_INTERNAL_FUNCTION) fn->internal_function.handler = ZEND_FN(hp_getenv);
}

hp_header_t *hp_env_snapshot(void)
{
    zval all;
    array_init(&all);
    hp_env_server_import(&all);
    if (sandbox) {
        zend_string *key; zval *value;
        ZEND_HASH_FOREACH_STR_KEY_VAL(sandbox, key, value) {
            if (!key) continue;
            if (Z_TYPE_P(value) == IS_NULL) zend_hash_del(Z_ARRVAL(all), key);
            else { Z_TRY_ADDREF_P(value); zend_hash_update(Z_ARRVAL(all), key, value); }
        } ZEND_HASH_FOREACH_END();
    }
    hp_header_t *snapshot = NULL;
    zend_string *key; zval *value;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL(all), key, value) {
        if (key && Z_TYPE_P(value) == IS_STRING && hp_header_add(&snapshot, ZSTR_VAL(key), Z_STRVAL_P(value))) {
            hp_headers_free(snapshot); snapshot = NULL; break;
        }
    } ZEND_HASH_FOREACH_END();
    zval_ptr_dtor(&all);
    return snapshot;
}

void hp_env_apply_snapshot(const hp_header_t *snapshot)
{
    hp_env_request_clear();
    for (const hp_header_t *entry = snapshot; entry; entry = entry->next)
        hp_env_set(entry->name, entry->value);
}
