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

void hp_env_shutdown(void)
{
    if (process_env) {
        for (size_t i = 0; i < process_env_count; ++i) free(process_env[i]);
        free(process_env);
    }
    process_env = NULL;
    process_env_count = 0;
}

static const char *lookup_process(const char *name, size_t len)
{
    for (size_t i = 0; i < process_env_count; ++i) {
        const char *eq = strchr(process_env[i], '=');
        if (eq && (size_t)(eq - process_env[i]) == len && !memcmp(name, process_env[i], len))
            return eq + 1;
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
    for (size_t i = 0; i < process_env_count; ++i) {
        const char *eq = strchr(process_env[i], '=');
        if (!eq) continue;
        size_t len = (size_t)(eq - process_env[i]);
        char *name = estrndup(process_env[i], len);
        php_register_variable_safe(name, eq + 1, strlen(eq + 1), target);
        efree(name);
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
