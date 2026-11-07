#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../h2o/php_handler.h"
#include "cpu_budget.h"
#include "h2o/configurator.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct worker_catalog {
    struct worker_catalog *next;
    hp_worker_group_t *group;
    bool global;
} worker_catalog_t;
typedef struct {
    h2o_configurator_t super;
    uint64_t runtime_seen;
    bool runtime_mapping_seen;
    /* Borrowed only during synchronous configuration application. */
    yoml_t *pending_global_workers;
    h2o_configurator_command_t *pending_global_workers_cmd;
    hp_header_t *ini_seen;
    worker_catalog_t *catalog;
    hp_route_config_t *vars;
    hp_route_config_t stack[H2O_CONFIGURATOR_NUM_LEVELS + 2];
} php_configurator_t;
/* Same accepted signed64 decimal grammar as Caddy Atoi on the amd64 target,
 * restricted to the worker option's legal -1..INT64_MAX range. */
static bool worker_failure_limit(const char *text, int64_t *value)
{
    const unsigned char *p = (const unsigned char *)text;
    bool negative = false;
    if (*p == '+' || *p == '-') { negative = *p == '-'; ++p; }
    if (*p < '0' || *p > '9') return false;
    uint64_t magnitude = 0, limit = negative ? 1 : (uint64_t)INT64_MAX;
    do {
        unsigned digit = *p++ - '0';
        if (digit > limit || magnitude > (limit - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    } while (*p >= '0' && *p <= '9');
    if (*p) return false;
    *value = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    return true;
}
static int scalar(h2o_configurator_command_t *cmd, yoml_t *node)
{
    if (node->type == YOML_TYPE_SCALAR) return 0;
    h2o_configurator_errprintf(cmd, node, "value must be a scalar"); return -1;
}
/* Match nonnegative Go duration components, with native millisecond timers.
 * Bare integers retain the existing YAML milliseconds spelling. */
static int duration_ms(const char *text, uint64_t *out)
{
    const char *p = text;
    if (*p == '+') ++p;
    if (!*p || *p == '-') return -1;
    const char *digits = p;
    uint64_t plain = 0;
    bool plain_overflow = false;
    while (*p >= '0' && *p <= '9') {
        unsigned n = (unsigned)(*p++ - '0');
        if (plain > (UINT64_MAX - n) / 10) plain_overflow = true;
        if (!plain_overflow) plain = plain * 10 + n;
    }
    if (!*p && p != digits) {
        if (plain_overflow) return -1;
        *out = plain; return 0;
    }
    p = digits;
    uint64_t total = 0;
    while (*p) {
        long double value = 0, place = 0.1L;
        bool any = false;
        while (*p >= '0' && *p <= '9') {
            any = true; value = value * 10 + (unsigned)(*p++ - '0');
            if (value > INT64_MAX) return -1;
        }
        if (*p == '.') {
            ++p;
            while (*p >= '0' && *p <= '9') {
                any = true; value += (unsigned)(*p++ - '0') * place; place *= 0.1L;
            }
        }
        if (!any) return -1;
        static const struct { const char *name; uint64_t ns; } units[] = {
            {"ns", 1}, {"us", 1000}, {"\xC2\xB5s", 1000}, {"\xCE\xBCs", 1000},
            {"ms", 1000000}, {"s", 1000000000}, {"m", UINT64_C(60000000000)},
            {"h", UINT64_C(3600000000000)}
        };
        size_t unit;
        for (unit = 0; unit < sizeof(units) / sizeof(units[0]); ++unit) {
            size_t length = strlen(units[unit].name);
            if (!strncmp(p, units[unit].name, length)) { p += length; break; }
        }
        if (unit == sizeof(units) / sizeof(units[0])) return -1;
        long double ns = value * units[unit].ns;
        if (ns > (long double)(INT64_MAX - total)) return -1;
        total += (uint64_t)ns;
    }
    /* A positive timeout must not become the zero/disabled sentinel. */
    *out = total / 1000000 + (total % 1000000 != 0);
    return 0;
}
static int number(h2o_configurator_command_t *cmd, yoml_t *node, uint64_t *out, int duration)
{
    if (scalar(cmd, node)) return -1;
    if (duration) {
        if (!duration_ms(node->data.scalar, out)) return 0;
        h2o_configurator_errprintf(cmd, node, "expected nonnegative duration (ns, us, ms, s, m, h; fractions and composites allowed)");
        return -1;
    }
    const unsigned char *digits = (const unsigned char *)node->data.scalar;
    if (*digits == '+') ++digits;
    if (!*digits) goto Invalid;
    for (; *digits; ++digits)
        if (*digits < '0' || *digits > '9') goto Invalid;
    char *end; errno = 0;
    unsigned long long value = strtoull(node->data.scalar, &end, 10);
    if (errno || *end) goto Invalid;
    *out = value; return 0;
Invalid:
    h2o_configurator_errprintf(cmd, node, "expected nonnegative integer");
    return -1;
}
static void option_key(const char *key, char *out, size_t cap)
{
    snprintf(out, cap, "%s", key);
    for (char *p = out; *p; ++p) if (*p == '_') *p = '-';
    if (!strcmp(out, "num-threads")) snprintf(out, cap, "threads");
}
static int boolean(h2o_configurator_command_t *cmd, yoml_t *node, int *out)
{
    if (scalar(cmd, node)) return -1;
    if (!strcasecmp(node->data.scalar, "ON") || !strcasecmp(node->data.scalar, "true") || !strcmp(node->data.scalar, "1")) *out = 1;
    else if (!strcasecmp(node->data.scalar, "OFF") || !strcasecmp(node->data.scalar, "false") || !strcmp(node->data.scalar, "0")) *out = 0;
    else { h2o_configurator_errprintf(cmd, node, "expected ON/OFF or true/false"); return -1; }
    return 0;
}
static int set_runtime_value(h2o_configurator_command_t *cmd, const char *original_key, yoml_t *node)
{
    php_configurator_t *c = (void *)cmd->configurator;
    char key[64]; option_key(original_key, key, sizeof(key));
    static const char *keys[] = {"threads", "max-threads", "queue-limit", "max-requests", "max-wait-time", "request-timeout", "shutdown-timeout", "startup-timeout", "max-idle-time", "max-buffered-body", "max-buffered-response", "max-buffered-global", "collect-metrics"};
    size_t which;
    for (which = 0; which < sizeof(keys)/sizeof(keys[0]); ++which) if (!strcmp(key, keys[which])) break;
    if (which == sizeof(keys)/sizeof(keys[0])) { h2o_configurator_errprintf(cmd, node, "unknown PHP runtime option: %s", original_key); return -1; }
    if (c->runtime_seen & (1ULL << which)) { h2o_configurator_errprintf(cmd, node, "duplicate or conflicting PHP runtime aliases for %s", keys[which]); return -1; }
    c->runtime_seen |= 1ULL << which;
    if (!strcmp(key, "collect-metrics")) {
        int enabled;
        if (boolean(cmd, node, &enabled)) return -1;
        hp_config.collect_metrics = enabled != 0; return 0;
    }
    if (!strcmp(key, "max-threads") && node->type == YOML_TYPE_SCALAR && !strcasecmp(node->data.scalar, "auto")) {
        hp_config.max_threads_auto = hp_config.max_threads_explicit = true; hp_config.max_threads = 0; return 0;
    }
    uint64_t value;
    if (number(cmd, node, &value, strstr(key, "time") != NULL)) return -1;
    if (!strcmp(key, "threads") || !strcmp(key, "max-threads") || !strcmp(key, "queue-limit") || !strcmp(key, "max-requests")) {
        if (value > INT_MAX - 2) goto Range;
        if (!strcmp(key, "threads")) { hp_config.threads = (unsigned)value; hp_config.threads_explicit = value != 0; }
        else if (!strcmp(key, "max-threads")) { hp_config.max_threads = (unsigned)value; hp_config.max_threads_explicit = value != 0; hp_config.max_threads_auto = false; }
        else if (!strcmp(key, "queue-limit")) hp_config.queue_limit = (unsigned)value;
        else hp_config.max_requests = (unsigned)value;
    } else if (!strcmp(key, "max-wait-time")) hp_config.max_wait_ms = value;
    else if (!strcmp(key, "request-timeout")) hp_config.request_timeout_ms = value;
    else if (!strcmp(key, "shutdown-timeout")) hp_config.shutdown_timeout_ms = value;
    else if (!strcmp(key, "startup-timeout")) { if (!value) goto Range; hp_config.startup_timeout_ms = value; }
    else if (!strcmp(key, "max-idle-time")) hp_config.max_idle_ms = value;
    else {
        /* Match the runtime's 16 KiB global minimum during validation.
         * Per-request body and response limits retain their 4 KiB minimum. */
        if (!strcmp(key, "max-buffered-global") && value < 16384) {
            h2o_configurator_errprintf(cmd, node, "max-buffered-global must be at least 16384 bytes (got %" PRIu64 ")", value);
            return -1;
        }
        if (value < 4096 || value > SIZE_MAX) goto Range;
        if (!strcmp(key, "max-buffered-body")) hp_config.max_buffered_body = (size_t)value;
        else if (!strcmp(key, "max-buffered-response")) hp_config.max_buffered_response = (size_t)value;
        else hp_config.max_buffered_global = (size_t)value;
    }
    return 0;
Range:
    h2o_configurator_errprintf(cmd, node, "PHP runtime option out of range: %s", original_key); return -1;
}
static int on_runtime_scalar(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx; return set_runtime_value(cmd, cmd->name + 4, node);
}
/* Preserve the logical absolute spelling without following root symlinks. */
static char *absolute_path(const char *value, const char *base)
{
    char cwd[PATH_MAX], *joined = NULL;
    if (value[0] == '/') joined = strdup(value);
    else {
        if (!base) { if (!getcwd(cwd, sizeof(cwd))) return NULL; base = cwd; }
        if (asprintf(&joined, "%s/%s", base, value) < 0) return NULL;
    }
    if (!joined) return NULL;
    char *out = calloc(strlen(joined) + 2, 1), *save = NULL;
    if (!out) { free(joined); return NULL; }
    size_t length = 1; out[0] = '/';
    for (char *part = strtok_r(joined, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) {
            if (length > 1) { while (length > 1 && out[length - 1] != '/') --length; if (length > 1) --length; out[length] = 0; }
            continue;
        }
        if (length > 1) out[length++] = '/';
        size_t n = strlen(part); memcpy(out + length, part, n + 1); length += n;
    }
    free(joined); return out;
}
static int set_root(h2o_configurator_command_t *cmd, yoml_t *node, hp_route_config_t *route)
{
    if (scalar(cmd, node)) return -1;
    char *logical = absolute_path(node->data.scalar, NULL), canonical[PATH_MAX];
    struct stat st;
    if (!logical || !realpath(logical, canonical) || stat(canonical, &st) || !S_ISDIR(st.st_mode)) {
        h2o_configurator_errprintf(cmd, node, "PHP root must resolve to an existing directory: %s", node->data.scalar); free(logical); return -1;
    }
    free(route->root); route->root = logical; return 0;
}
static int claim_route(h2o_configurator_command_t *cmd, yoml_t *node, hp_route_config_t *route, unsigned bit)
{
    if (route->specified_options & bit) { h2o_configurator_errprintf(cmd, node, "duplicate or conflicting aliases for %s", cmd->name); return -1; }
    route->specified_options |= bit; return 0;
}
static int on_root(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx; php_configurator_t *c = (void *)cmd->configurator; return claim_route(cmd, node, c->vars, 1U) || set_root(cmd, node, c->vars) ? -1 : 0;
}
static int set_index(h2o_configurator_command_t *cmd, yoml_t *node, hp_route_config_t *route)
{
    if (scalar(cmd, node)) return -1;
    const char *value = node->data.scalar;
    if (!strcasecmp(value, "OFF") || !strcasecmp(value, "false")) { free(route->index_file); route->index_file = NULL; return 0; }
    size_t len = strlen(value);
    if (value[0] == '/' || strstr(value, "..") || !len) { h2o_configurator_errprintf(cmd, node, "index must be a relative script path within root, or OFF"); return -1; }
    free(route->index_file); route->index_file = strdup(value); return route->index_file ? 0 : -1;
}
static int on_index(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx; php_configurator_t *c = (void *)cmd->configurator; return claim_route(cmd, node, c->vars, 2U) || set_index(cmd, node, c->vars) ? -1 : 0;
}
static int on_timeout(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx; php_configurator_t *c = (void *)cmd->configurator;
    const char *key = cmd->name + 4;
    if (strstr(key, "body")) return claim_route(cmd, node, c->vars, 4U) || number(cmd, node, &c->vars->body_timeout_ms, 1) ? -1 : 0;
    if (strstr(key, "response")) return claim_route(cmd, node, c->vars, 8U) || number(cmd, node, &c->vars->response_timeout_ms, 1) ? -1 : 0;
    if (claim_route(cmd, node, c->vars, 16U)) return -1;
    c->vars->timeout_set = true; return number(cmd, node, &c->vars->timeout_ms, 1);
}
static void replace_env(hp_header_t **list, const char *key, const char *value)
{
    hp_header_t **h = list;
    while (*h) {
        if (!strcmp((*h)->name, key)) {
            hp_header_t *old = *h; *h = old->next; old->next = NULL; hp_headers_free(old);
        } else h = &(*h)->next;
    }
    hp_header_add(list, key, value);
}
static int parse_env(h2o_configurator_command_t *cmd, yoml_t *node, hp_header_t **list)
{
    if (node->type != YOML_TYPE_MAPPING) { h2o_configurator_errprintf(cmd, node, "environment must be a mapping"); return -1; }
    for (size_t i = 0; i < node->data.mapping.size; ++i) {
        yoml_mapping_element_t *e = node->data.mapping.elements + i;
        if (scalar(cmd, e->key) || scalar(cmd, e->value)) return -1;
        if (!*e->key->data.scalar || strchr(e->key->data.scalar, '=')) { h2o_configurator_errprintf(cmd, e->key, "invalid environment name"); return -1; }
        replace_env(list, e->key->data.scalar, e->value->data.scalar);
    }
    return 0;
}
static int on_env(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx; php_configurator_t *c = (void *)cmd->configurator;
    return parse_env(cmd, node, &c->vars->env);
}
static int on_ini(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx;
    php_configurator_t *c = (void *)cmd->configurator;
    for (size_t i = 0; i < node->data.mapping.size; ++i) {
        yoml_mapping_element_t *e = node->data.mapping.elements + i;
        if (scalar(cmd, e->key)) return -1;
        const char *key = e->key->data.scalar;
        if (!*key || strpbrk(key, "\r\n=\"")) {
            h2o_configurator_errprintf(cmd, e->key, "invalid PHP INI key"); return -1;
        }
        bool sequence = e->value->type == YOML_TYPE_SEQUENCE;
        if (sequence && strcmp(key, "extension") && strcmp(key, "zend_extension")) {
            h2o_configurator_errprintf(cmd, e->value, "only extension and zend_extension accept lists"); return -1;
        }
        size_t count = sequence ? e->value->data.sequence.size : 1;
        if (!count) { h2o_configurator_errprintf(cmd, e->value, "INI extension list must not be empty"); return -1; }
        char *fragment = strdup("");
        if (!fragment) return -1;
        for (size_t j = 0; j < count; ++j) {
            yoml_t *value = sequence ? e->value->data.sequence.elements[j] : e->value;
            if (scalar(cmd, value)) { free(fragment); return -1; }
            if (strpbrk(value->data.scalar, "\r\n\"")) {
                h2o_configurator_errprintf(cmd, value, "INI values must not contain newlines or quotes"); free(fragment); return -1;
            }
            char *next;
            if (asprintf(&next, "%s%s=\"%s\"\n", fragment, key, value->data.scalar) < 0) { free(fragment); return -1; }
            free(fragment); fragment = next;
        }
        const char *old = hp_header_get(c->ini_seen, key);
        if (old && strcmp(old, fragment)) {
            h2o_configurator_errprintf(cmd, e->key, "conflicting PHP INI aliases for %s", key); free(fragment); return -1;
        }
        if (old) { free(fragment); continue; }
        if (hp_header_add(&c->ini_seen, key, fragment)) { free(fragment); return -1; }
        char *new_entries;
        if (asprintf(&new_entries, "%s%s", hp_config.ini_entries ? hp_config.ini_entries : "", fragment) < 0) { free(fragment); return -1; }
        free(fragment); free(hp_config.ini_entries); hp_config.ini_entries = new_entries;
    }
    return 0;
}
static int on_log(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx;
    if (!*node->data.scalar) { h2o_configurator_errprintf(cmd, node, "php.log requires a file path or stderr"); return -1; }
    free(hp_config.log_path);
    hp_config.log_path = strdup(node->data.scalar);
    return hp_config.log_path ? 0 : -1;
}
static int on_metrics(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    int format = HP_METRICS_JSON;
    if (node->type == YOML_TYPE_SCALAR) {
        ssize_t enabled = h2o_configurator_get_one_of(cmd, node, "OFF,ON");
        if (enabled < 0) return -1;
        if (!enabled) return 0;
    } else {
        if (node->type != YOML_TYPE_MAPPING || node->data.mapping.size != 1) {
            h2o_configurator_errprintf(cmd, node, "php.metrics requires ON, OFF or exactly one format option"); return -1;
        }
        yoml_mapping_element_t *e = node->data.mapping.elements;
        if (scalar(cmd, e->key) || scalar(cmd, e->value)) return -1;
        if (strcmp(e->key->data.scalar, "format")) {
            h2o_configurator_errprintf(cmd, e->key, "unknown php.metrics option: %s", e->key->data.scalar); return -1;
        }
        ssize_t value = h2o_configurator_get_one_of(cmd, e->value, "json,prometheus");
        if (value < 0) return -1;
        format = value ? HP_METRICS_PROMETHEUS : HP_METRICS_JSON;
    }
    hp_route_config_t route = {.metrics = format}; hp_h2o_register(ctx->pathconf, &route);
    return 0;
}
static int on_restart(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    ssize_t enabled = h2o_configurator_get_one_of(cmd, node, "OFF,ON");
    if (enabled < 0) return -1;
    if (enabled) { hp_route_config_t route = {.restart = 1}; hp_h2o_register(ctx->pathconf, &route); }
    return 0;
}
static int set_split_path(h2o_configurator_command_t *cmd, yoml_t *node, hp_route_config_t *route)
{
    if (node->type == YOML_TYPE_SCALAR && (!strcasecmp(node->data.scalar, "ON") || !strcasecmp(node->data.scalar, "OFF") || !strcasecmp(node->data.scalar, "true") || !strcasecmp(node->data.scalar, "false")))
        return boolean(cmd, node, &route->split_path);
    size_t n = node->type == YOML_TYPE_SEQUENCE ? node->data.sequence.size : 1;
    if (!n) { h2o_configurator_errprintf(cmd, node, "split_path must contain at least one delimiter"); return -1; }
    char **parts = calloc(n, sizeof(char *));
    if (!parts) return -1;
    for (size_t i = 0; i < n; ++i) {
        yoml_t *part = node->type == YOML_TYPE_SEQUENCE ? node->data.sequence.elements[i] : node;
        if (scalar(cmd, part) || !*part->data.scalar || strchr(part->data.scalar, '/') || strchr(part->data.scalar, '\\')) {
            h2o_configurator_errprintf(cmd, part, "split_path delimiters must be nonempty and contain no slash");
            for (size_t j = 0; j < i; ++j) free(parts[j]);
            free(parts); return -1;
        }
        for (const unsigned char *ch = (const unsigned char *)part->data.scalar; *ch; ++ch) {
            if (*ch >= 128) {
                h2o_configurator_errprintf(cmd, part, "split path contains non-ASCII characters");
                for (size_t j = 0; j < i; ++j) free(parts[j]);
                free(parts); return -1;
            }
        }
        parts[i] = strdup(part->data.scalar);
        if (!parts[i]) {
            for (size_t j = 0; j < i; ++j) free(parts[j]);
            free(parts); return -1;
        }
        for (char *ch = parts[i]; *ch; ++ch)
            if (*ch >= 'A' && *ch <= 'Z') *ch += 'a' - 'A';
    }
    for (size_t i = 0; i < route->num_split_paths; ++i) free(route->split_paths[i]);
    free(route->split_paths); route->split_paths = parts; route->num_split_paths = n; route->split_path = 1; return 0;
}
static int on_split_path(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx; php_configurator_t *c = (void *)cmd->configurator; return claim_route(cmd, node, c->vars, 32U) || set_split_path(cmd, node, c->vars) ? -1 : 0;
}
static int on_symlink(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx; php_configurator_t *c = (void *)cmd->configurator; return claim_route(cmd, node, c->vars, 64U) || boolean(cmd, node, &c->vars->resolve_root_symlink) ? -1 : 0;
}
static int compile_match(h2o_configurator_command_t *cmd, yoml_t *node, regex_t *expression)
{
    if (scalar(cmd, node)) return -1;
    int rc = regcomp(expression, node->data.scalar, REG_EXTENDED | REG_NOSUB);
    if (!rc) return 0;
    char message[256]; regerror(rc, expression, message, sizeof(message));
    h2o_configurator_errprintf(cmd, node, "invalid worker match expression: %s", message); return -1;
}
static int token_name(const char *value)
{
    if (!*value) return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("!#$%&'*+-.^_`|~", *p))) return 0;
    return 1;
}
static int parse_worker_match(h2o_configurator_command_t *cmd, yoml_t *node, hp_route_match_t **out)
{
    if (node->type != YOML_TYPE_MAPPING) { h2o_configurator_errprintf(cmd, node, "worker match must be a mapping"); return -1; }
    hp_route_match_t *match = calloc(1, sizeof(*match));
    if (!match) return -1;
    atomic_init(&match->refs, 1);
    bool seen_methods = false, seen_headers = false;
    for (size_t i = 0; i < node->data.mapping.size; ++i) {
        yoml_mapping_element_t *e = node->data.mapping.elements + i;
        if (scalar(cmd, e->key)) goto Error;
        const char *key = e->key->data.scalar;
        if (!strcmp(key, "path")) {
            if (match->has_path) goto Duplicate;
            if (compile_match(cmd, e->value, &match->path)) goto Error;
            match->has_path = true;
        } else if (!strcmp(key, "methods")) {
            if (seen_methods) goto Duplicate;
            seen_methods = true;
            size_t n = e->value->type == YOML_TYPE_SEQUENCE ? e->value->data.sequence.size : 1;
            if (!n) { h2o_configurator_errprintf(cmd, e->value, "worker methods must not be empty"); goto Error; }
            for (size_t j = 0; j < n; ++j) {
                yoml_t *entry = e->value->type == YOML_TYPE_SEQUENCE ? e->value->data.sequence.elements[j] : e->value;
                if (scalar(cmd, entry)) goto Error;
                if (!token_name(entry->data.scalar)) { h2o_configurator_errprintf(cmd, entry, "invalid HTTP method"); goto Error; }
                h2o_vector_reserve(NULL, &match->methods, match->methods.size + 1);
                match->methods.entries[match->methods.size++] = h2o_strdup(NULL, entry->data.scalar, SIZE_MAX).base;
            }
        } else if (!strcmp(key, "headers")) {
            if (seen_headers) goto Duplicate;
            seen_headers = true;
            if (e->value->type != YOML_TYPE_MAPPING) { h2o_configurator_errprintf(cmd, e->value, "worker match headers must be a mapping"); goto Error; }
            for (size_t j = 0; j < e->value->data.mapping.size; ++j) {
                yoml_mapping_element_t *header = e->value->data.mapping.elements + j;
                if (scalar(cmd, header->key)) goto Error;
                if (!token_name(header->key->data.scalar)) { h2o_configurator_errprintf(cmd, header->key, "invalid HTTP header name"); goto Error; }
                h2o_vector_reserve(NULL, &match->headers, match->headers.size + 1);
                if (compile_match(cmd, header->value, &match->headers.entries[match->headers.size].value)) goto Error;
                match->headers.entries[match->headers.size].name = h2o_strdup(NULL, header->key->data.scalar, SIZE_MAX).base;
                ++match->headers.size;
            }
        } else { h2o_configurator_errprintf(cmd, e->key, "unknown worker match option: %s", key); goto Error; }
        continue;
Duplicate:
        h2o_configurator_errprintf(cmd, e->key, "duplicate worker match option: %s", key); goto Error;
    }
    *out = match; return 0;
Error:
    hp_route_match_release(match); return -1;
}
static int parse_path_match(h2o_configurator_command_t *cmd, yoml_t *node, bool regexp, hp_route_match_t **out)
{
    if (node->type == YOML_TYPE_MAPPING) return parse_worker_match(cmd, node, out);
    hp_route_match_t *match = calloc(1, sizeof(*match));
    if (!match) return -1;
    atomic_init(&match->refs, 1);
    if (regexp) {
        if (compile_match(cmd, node, &match->path)) { hp_route_match_release(match); return -1; }
        match->has_path = true;
    } else {
        size_t count = node->type == YOML_TYPE_SEQUENCE ? node->data.sequence.size : 1;
        if (!count) { h2o_configurator_errprintf(cmd, node, "worker match must not be empty"); hp_route_match_release(match); return -1; }
        for (size_t i = 0; i < count; ++i) {
            yoml_t *entry = node->type == YOML_TYPE_SEQUENCE ? node->data.sequence.elements[i] : node;
            if (scalar(cmd, entry) || !*entry->data.scalar || entry->data.scalar[0] != '/') { h2o_configurator_errprintf(cmd, entry, "worker match must be an absolute URL path glob"); hp_route_match_release(match); return -1; }
            h2o_vector_reserve(NULL, &match->globs, match->globs.size + 1);
            match->globs.entries[match->globs.size++] = strdup(entry->data.scalar);
        }
    }
    *out = match; return 0;
}
static int add_watch(h2o_configurator_command_t *cmd, yoml_t *node, hp_worker_config_t *worker, const char *base)
{
    if (node->type == YOML_TYPE_SCALAR && (!strcasecmp(node->data.scalar, "OFF") || !strcasecmp(node->data.scalar, "false"))) return 0;
    size_t n = node->type == YOML_TYPE_SEQUENCE ? node->data.sequence.size : 1;
    bool default_pattern = !n || (node->type == YOML_TYPE_SCALAR && (!*node->data.scalar || !strcasecmp(node->data.scalar, "ON") || !strcasecmp(node->data.scalar, "true")));
    if (default_pattern) n = 1;
    worker->watch = calloc(n, sizeof(char *)); worker->num_watch = n;
    if (!worker->watch) return -1;
    for (size_t i = 0; i < n; ++i) {
        yoml_t *entry = node->type == YOML_TYPE_SEQUENCE && !default_pattern ? node->data.sequence.elements[i] : node;
        if (!default_pattern && scalar(cmd, entry)) return -1;
        worker->watch[i] = absolute_path(default_pattern ? "**/*.{env,php,twig,yaml,yml}" : entry->data.scalar, base);
        if (!worker->watch[i]) return -1;
    }
    return 0;
}
static int parse_one_worker(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node,
                            hp_route_config_t *route, bool allow_match, bool global)
{
    (void)ctx;
    php_configurator_t *c = (void *)cmd->configurator;
    hp_worker_config_t worker = {.max_requests = hp_config.max_requests, .max_consecutive_failures = 6, .root = route->root};
    worker.env = hp_headers_clone(route->env);
    hp_route_match_t *match = NULL;
    hp_header_t *seen = NULL;
    int rc = -1;
    char *file = NULL, *unique_name = NULL, *scoped_name = NULL, *metric_name = NULL;
    if (node->type == YOML_TYPE_SCALAR) worker.file = node->data.scalar;
    else if (node->type != YOML_TYPE_MAPPING) { h2o_configurator_errprintf(cmd, node, "worker must be a script or mapping"); goto Done; }
    for (size_t i = 0; node->type == YOML_TYPE_MAPPING && i < node->data.mapping.size; ++i) {
        yoml_mapping_element_t *e = node->data.mapping.elements + i;
        if (scalar(cmd, e->key)) goto Done;
        char key[64]; option_key(e->key->data.scalar, key, sizeof(key));
        if (hp_header_get(seen, key)) { h2o_configurator_errprintf(cmd, e->key, "duplicate worker option or alias: %s", key); goto Done; }
        hp_header_add(&seen, key, "1");
        if (!strcmp(key, "match") || !strcmp(key, "match-regexp")) {
            if (!allow_match || match) { h2o_configurator_errprintf(cmd, e->key, "worker match requires php_server or a native worker route, once only"); goto Done; }
            if (!strcmp(key, "match") && route->mode == HP_ROUTE_SERVER && e->value->type == YOML_TYPE_MAPPING) {
                h2o_configurator_errprintf(cmd, e->value, "php_server worker match uses path globs; use match_regexp for native predicates"); goto Done;
            }
            if (parse_path_match(cmd, e->value, !strcmp(key, "match-regexp"), &match)) goto Done;
        } else if (!strcmp(key, "startup-timeout")) {
            if (number(cmd, e->value, &worker.startup_timeout_ms, 1)) goto Done;
            if (!worker.startup_timeout_ms) { h2o_configurator_errprintf(cmd, e->value, "startup_timeout must be positive"); goto Done; }
        } else if (!strcmp(key, "env")) { if (parse_env(cmd, e->value, &worker.env)) goto Done; }
        else if (!strcmp(key, "watch")) { if (add_watch(cmd, e->value, &worker, NULL)) goto Done; }
        else if (!strcmp(key, "file") || !strcmp(key, "name")) {
            if (scalar(cmd, e->value)) goto Done;
            if (!strcmp(key, "file")) worker.file = e->value->data.scalar; else worker.name = e->value->data.scalar;
        } else if (!strcmp(key, "max-consecutive-failures")) {
            if (scalar(cmd, e->value)) goto Done;
            int64_t n;
            if (!worker_failure_limit(e->value->data.scalar, &n)) { h2o_configurator_errprintf(cmd, e->value, "max_consecutive_failures must be -1 or a signed64 nonnegative integer"); goto Done; }
            worker.max_consecutive_failures = n;
        } else if (!strcmp(key, "num") || !strcmp(key, "max-requests") || !strcmp(key, "max-threads")) {
            uint64_t n; if (number(cmd, e->value, &n, 0)) goto Done;
            if (n > INT_MAX - 2) { h2o_configurator_errprintf(cmd, e->value, "worker count/limit out of range"); goto Done; }
            if (!strcmp(key, "num")) worker.num = (unsigned)n;
            else if (!strcmp(key, "max-threads")) worker.max_threads = (unsigned)n;
            else worker.max_requests = (unsigned)n;
        } else { h2o_configurator_errprintf(cmd, e->key, "unknown worker option: %s", e->key->data.scalar); goto Done; }
    }
    if (!worker.file || !*worker.file || !(file = absolute_path(worker.file, route->root))) { h2o_configurator_errprintf(cmd, node, "worker file is required"); goto Done; }
    char canonical[PATH_MAX]; struct stat st;
    if (!realpath(file, canonical) || stat(canonical, &st) || !S_ISREG(st.st_mode)) { h2o_configurator_errprintf(cmd, node, "worker file must resolve to a regular script: %s", file); goto Done; }
    worker.file = canonical;
    char canonical_root[PATH_MAX];
    if (worker.root && route->resolve_root_symlink && realpath(worker.root, canonical_root)) worker.root = canonical_root;
    if (global) for (worker_catalog_t *entry = c->catalog; entry; entry = entry->next)
        if (entry->global && !strcmp(hp_worker_group_config(entry->group)->file, canonical)) {
            h2o_configurator_errprintf(cmd, node, "duplicate global worker script: %s", canonical); goto Done;
        }
    if (!route->workers) { route->workers = calloc(1, sizeof(*route->workers)); if (!route->workers) goto Done; atomic_init(&route->workers->refs, 1); }
    for (size_t i = 0; i < route->workers->entries.size; ++i)
        if (!strcmp(hp_worker_group_config(route->workers->entries.entries[i].group)->file, canonical)) { h2o_configurator_errprintf(cmd, node, "duplicate worker script in this server: %s", canonical); goto Done; }
    if (route->name && !global && asprintf(&scoped_name, "%s:%s", route->name, worker.name ? worker.name : canonical) < 0) goto Done;
    const char *base_name = scoped_name ? scoped_name : (worker.name ? worker.name : canonical);
    unique_name = strdup(base_name);
    for (unsigned suffix = 2;; ++suffix) {
        bool used = false;
        for (worker_catalog_t *entry = c->catalog; entry; entry = entry->next)
            if (!strcmp(hp_worker_group_config(entry->group)->name, unique_name)) { used = true; break; }
        if (!used) break;
        free(unique_name); if (asprintf(&unique_name, "%s#%u", base_name, suffix) < 0) { unique_name = NULL; goto Done; }
    }
    /* Metrics use the original logical path/name and upstream's separate
     * collision policy. Existing scoped native names and logs do not change.
     * An omitted native server name still differs from Caddy's srvN grouping;
     * collision-qualified defaults retain that documented mapping gap. */
    const char *metric_base = worker.name && *worker.name ? worker.name : file;
    metric_name = strdup(metric_base);
    bool qualify = !global && route->name && *route->name;
    unsigned metric_suffix = 0;
    if (!metric_name) goto Done;
    for (;;) {
        bool used = false;
        for (worker_catalog_t *entry = c->catalog; entry; entry = entry->next) {
            const hp_worker_config_t *old = hp_worker_group_config(entry->group);
            if (!strcmp(old->metrics_name, metric_name)) { used = true; break; }
        }
        if (!used) break;
        free(metric_name); metric_name = NULL;
        if (qualify) {
            if (asprintf(&metric_name, "%s:%s", route->name, metric_base) < 0) metric_name = NULL;
            qualify = false;
        } else {
            if (metric_suffix == UINT_MAX) goto Done;
            if (asprintf(&metric_name, "%s_%u", metric_base, ++metric_suffix) < 0) metric_name = NULL;
        }
        if (!metric_name) goto Done;
    }
    worker.metrics_name = metric_name;
    worker.name = unique_name;
    hp_worker_group_t *group = hp_worker_group_create(&worker);
    if (!group) { h2o_configurator_errprintf(cmd, node, "could not allocate worker group"); goto Done; }
    worker_catalog_t *catalog = calloc(1, sizeof(*catalog));
    if (!catalog) goto Done;
    catalog->group = group; catalog->global = global; catalog->next = c->catalog; c->catalog = catalog;
    h2o_vector_reserve(NULL, &route->workers->entries, route->workers->entries.size + 1);
    route->workers->entries.entries[route->workers->entries.size].group = group;
    route->workers->entries.entries[route->workers->entries.size].match = match;
    ++route->workers->entries.size; match = NULL; rc = 0;
Done:
    hp_route_match_release(match); hp_headers_free(worker.env); hp_headers_free(seen); free(file); free(unique_name); free(scoped_name); free(metric_name);
    for (size_t i = 0; i < worker.num_watch; ++i) free(worker.watch[i]);
    free(worker.watch); return rc;
}
static int parse_workers(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node,
                         hp_route_config_t *route, bool allow_match, bool global)
{
    size_t n = node->type == YOML_TYPE_SEQUENCE ? node->data.sequence.size : 1;
    for (size_t i = 0; i < n; ++i)
        if (parse_one_worker(cmd, ctx, node->type == YOML_TYPE_SEQUENCE ? node->data.sequence.elements[i] : node, route, allow_match, global)) return -1;
    return 0;
}
static void add_global_workers(php_configurator_t *c, hp_route_config_t *route)
{
    for (worker_catalog_t *entry = c->catalog; entry; entry = entry->next) {
        if (!entry->global) continue;
        bool exists = false;
        if (route->workers) for (size_t i = 0; i < route->workers->entries.size; ++i)
            if (!strcmp(hp_worker_group_config(route->workers->entries.entries[i].group)->file, hp_worker_group_config(entry->group)->file)) exists = true;
        if (exists) continue;
        if (!route->workers) { route->workers = calloc(1, sizeof(*route->workers)); if (!route->workers) abort(); atomic_init(&route->workers->refs, 1); }
        h2o_vector_reserve(NULL, &route->workers->entries, route->workers->entries.size + 1);
        route->workers->entries.entries[route->workers->entries.size].group = entry->group;
        route->workers->entries.entries[route->workers->entries.size].match = NULL;
        ++route->workers->entries.size;
    }
}
static int on_worker(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    php_configurator_t *c = (void *)cmd->configurator;
    if (c->vars->declared) { h2o_configurator_errprintf(cmd, node, "one PHP handler per path is allowed; put multiple worker definitions in a sequence"); return -1; }
    c->vars->declared = 1;
    hp_route_config_t route; hp_route_config_clone(&route, c->vars); route.mode = HP_ROUTE_WORKER;
    int rc = parse_workers(cmd, ctx, node, &route, true, false);
    if (!rc) hp_h2o_register(ctx->pathconf, &route);
    hp_route_config_dispose(&route); return rc;
}
static int on_runtime(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx;
    php_configurator_t *c = (void *)cmd->configurator;
    if (c->runtime_mapping_seen) { h2o_configurator_errprintf(cmd, node, "duplicate php.runtime mapping"); return -1; }
    c->runtime_mapping_seen = true;
    yoml_t *workers = NULL;
    for (size_t i = 0; i < node->data.mapping.size; ++i) {
        yoml_mapping_element_t *e = node->data.mapping.elements + i;
        if (scalar(cmd, e->key)) return -1;
        if (!strcmp(e->key->data.scalar, "php_ini") || !strcmp(e->key->data.scalar, "php-ini")) {
            if (e->value->type != YOML_TYPE_MAPPING || on_ini(cmd, ctx, e->value)) return -1;
        } else if (!strcmp(e->key->data.scalar, "worker") || !strcmp(e->key->data.scalar, "workers")) {
            if (workers) { h2o_configurator_errprintf(cmd, e->key, "duplicate worker/workers runtime option"); return -1; }
            workers = e->value;
        } else if (set_runtime_value(cmd, e->key->data.scalar, e->value)) return -1;
    }
    c->pending_global_workers = workers;
    c->pending_global_workers_cmd = workers ? cmd : NULL;
    return 0;
}
static int finalize_global_workers(php_configurator_t *c, h2o_configurator_context_t *ctx)
{
    yoml_t *workers = c->pending_global_workers;
    if (!workers) return 0;
    h2o_configurator_command_t *cmd = c->pending_global_workers_cmd;
    c->pending_global_workers = NULL; c->pending_global_workers_cmd = NULL;
    /* Global defaults/root/env are final before any host or extension enters.
     * Keep runtime scalars immediate: file.custom-handler is semi-deferred. */
    hp_route_config_t route; hp_route_config_clone(&route, c->vars);
    int rc = parse_workers(cmd, ctx, workers, &route, false, true);
    hp_route_config_dispose(&route); return rc;
}
static int on_php(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    php_configurator_t *c = (void *)cmd->configurator;
    if (c->vars->declared) { h2o_configurator_errprintf(cmd, node, "php and php_server cannot be combined with another PHP handler on the same path"); return -1; }
    c->vars->declared = 1;
    hp_route_config_t route; hp_route_config_clone(&route, c->vars);
    route.mode = !strcmp(cmd->name, "php_server") ? HP_ROUTE_SERVER : HP_ROUTE_DIRECT;
    route.file_server = route.mode == HP_ROUTE_SERVER;
    int rc = -1;
    yoml_t *workers = NULL, *script = NULL;
    hp_header_t *seen = NULL;
    if (node->type == YOML_TYPE_SCALAR) {
        int enabled;
        if (!*node->data.scalar || !strcasecmp(node->data.scalar, "ON") || !strcasecmp(node->data.scalar, "OFF") || !strcasecmp(node->data.scalar, "true") || !strcasecmp(node->data.scalar, "false")) {
            enabled = 1; if (*node->data.scalar && boolean(cmd, node, &enabled)) goto Done;
            if (!enabled) { c->vars->disabled = 1; rc = 0; goto Done; }
        } else if (route.mode == HP_ROUTE_DIRECT) script = node;
        else { if (set_root(cmd, node, &route)) goto Done; }
    } else for (size_t i = 0; i < node->data.mapping.size; ++i) {
        yoml_mapping_element_t *e = node->data.mapping.elements + i;
        if (scalar(cmd, e->key)) goto Done;
        char key[64]; option_key(e->key->data.scalar, key, sizeof(key));
        if (!strcmp(key, "worker")) strcpy(key, "workers");
        if (!strcmp(key, "split")) strcpy(key, "split-path");
        if (!strcmp(key, "response-delivery-timeout")) strcpy(key, "response-timeout");
        if (hp_header_get(seen, key)) { h2o_configurator_errprintf(cmd, e->key, "duplicate php handler option or alias: %s", key); goto Done; }
        hp_header_add(&seen, key, "1");
        if (!strcmp(key, "root")) { if (set_root(cmd, e->value, &route)) goto Done; }
        else if (!strcmp(key, "index")) { if (set_index(cmd, e->value, &route)) goto Done; }
        else if (!strcmp(key, "split-path")) { if (set_split_path(cmd, e->value, &route)) goto Done; }
        else if (!strcmp(key, "resolve-root-symlink")) { if (boolean(cmd, e->value, &route.resolve_root_symlink)) goto Done; }
        else if (!strcmp(key, "file-server")) { if (route.mode != HP_ROUTE_SERVER) { h2o_configurator_errprintf(cmd, e->key, "file_server belongs to php_server; php is a direct handler"); goto Done; } if (boolean(cmd, e->value, &route.file_server)) goto Done; }
        else if (!strcmp(key, "env")) { if (parse_env(cmd, e->value, &route.env)) goto Done; }
        else if (!strcmp(key, "workers")) workers = e->value;
        else if (!strcmp(key, "script")) script = e->value;
        else if (!strcmp(key, "name")) { if (scalar(cmd, e->value)) goto Done; free(route.name); route.name = strdup(e->value->data.scalar); }
        else if (!strcmp(key, "request-body-timeout")) { if (number(cmd, e->value, &route.body_timeout_ms, 1)) goto Done; }
        else if (!strcmp(key, "response-timeout") || !strcmp(key, "response-delivery-timeout")) { if (number(cmd, e->value, &route.response_timeout_ms, 1)) goto Done; }
        else if (!strcmp(key, "request-timeout")) { route.timeout_set = true; if (number(cmd, e->value, &route.timeout_ms, 1)) goto Done; }
        else { h2o_configurator_errprintf(cmd, e->key, "unknown php handler option: %s", e->key->data.scalar); goto Done; }
    }
    if (script && route.mode != HP_ROUTE_DIRECT) { h2o_configurator_errprintf(cmd, script, "script belongs to direct php; use index or worker in php_server"); goto Done; }
    if (!route.root && script && script->type == YOML_TYPE_SCALAR && script->data.scalar[0] == '/') {
        route.root = absolute_path(script->data.scalar, NULL);
        char *slash = route.root ? strrchr(route.root, '/') : NULL;
        if (slash) { if (slash == route.root) slash[1] = 0; else *slash = 0; }
    }
    if (!route.root && !(route.root = absolute_path(".", NULL))) goto Done;
    if (!route.name && ctx->hostconf) route.name = strndup(ctx->hostconf->authority.host.base, ctx->hostconf->authority.host.len);
    if (workers && parse_workers(cmd, ctx, workers, &route, route.mode == HP_ROUTE_SERVER, false)) goto Done;
    if (route.mode == HP_ROUTE_DIRECT && route.workers && route.workers->entries.size) {
        if (route.workers->entries.size != 1 || script) { h2o_configurator_errprintf(cmd, node, "direct php accepts exactly one worker or one script"); goto Done; }
        route.group = route.workers->entries.entries[0].group;
    } else {
        add_global_workers(c, &route);
        if (route.mode == HP_ROUTE_DIRECT) {
            if (script && scalar(cmd, script)) goto Done;
            const char *value = script ? script->data.scalar : route.index_file;
            if (!value) { h2o_configurator_errprintf(cmd, node, "direct php requires script when index is OFF"); goto Done; }
            char *path = absolute_path(value, route.root), canonical[PATH_MAX], root[PATH_MAX]; struct stat st;
            if (!path || !realpath(route.root, root) || !realpath(path, canonical) || stat(canonical, &st) || !S_ISREG(st.st_mode) ||
                strncmp(root, canonical, strlen(root)) || (strlen(root) != 1 && canonical[strlen(root)] && canonical[strlen(root)] != '/')) {
                h2o_configurator_errprintf(cmd, node, "direct script must resolve to a regular file within root"); free(path); goto Done;
            }
            free(route.script); route.script = strdup(canonical);
            free(route.script_name);
            const char *relative = canonical + strlen(root); if (*relative == '/') ++relative;
            if (asprintf(&route.script_name, "/%s", relative) < 0) { free(path); goto Done; }
            free(path);
        }
    }
    hp_h2o_register(ctx->pathconf, &route);
    if (route.mode == HP_ROUTE_SERVER && route.file_server) hp_h2o_register_server_files(ctx->pathconf, &route, *ctx->mimemap);
    rc = 0;
Done:
    hp_headers_free(seen); hp_route_config_dispose(&route); return rc;
}
static bool ascii_equal(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return *a == *b;
}
static bool extension_matches(const hp_route_config_t *route, const char *extension)
{
    size_t n = route->num_split_paths ? route->num_split_paths : 1;
    for (size_t i = 0; i < n; ++i) {
        const char *part = route->num_split_paths ? route->split_paths[i] : ".php";
        if (ascii_equal(part, extension) || (part[0] == '.' && ascii_equal(part + 1, extension))) return true;
    }
    return false;
}
static bool extension_node_matches(const hp_route_config_t *route, const yoml_t *node)
{
    if (!node) return false;
    if (node->type == YOML_TYPE_SCALAR) return extension_matches(route, node->data.scalar);
    if (node->type != YOML_TYPE_SEQUENCE || !node->data.sequence.size) return false;
    for (size_t i = 0; i < node->data.sequence.size; ++i)
        if (!extension_node_matches(route, node->data.sequence.elements[i])) return false;
    return true;
}
static void register_automatic(php_configurator_t *c, h2o_pathconf_t *path)
{
    hp_route_config_t route; hp_route_config_clone(&route, c->vars); add_global_workers(c, &route);
    hp_h2o_register(path, &route); hp_route_config_dispose(&route);
}
static int enter_config(h2o_configurator_t *self, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)node; php_configurator_t *c = (void *)self;
    if (ctx->parent && finalize_global_workers(c, ctx)) return -1;
    hp_route_config_clone(c->vars + 1, c->vars); ++c->vars; c->vars->declared = 0; c->vars->specified_options = 0; return 0;
}
static int exit_config(h2o_configurator_t *self, h2o_configurator_context_t *ctx, yoml_t *node)
{
    php_configurator_t *c = (void *)self;
    int rc = 0;
    if (!ctx->parent && finalize_global_workers(c, ctx)) { rc = -1; goto Done; }
    if (ctx->pathconf && !c->vars->disabled) {
        size_t num_dirs = 0;
        if (node) {
            for (size_t i = 0; i < node->data.mapping.size; ++i)
                if (!strcmp(node->data.mapping.elements[i].key->data.scalar, "file.dir")) ++num_dirs;
        }
        size_t canonical_count = 0, canonical_index = 0;
        const hp_route_config_t *canonical = NULL;
        for (size_t i = 0; i < ctx->pathconf->handlers.size; ++i) {
            const hp_route_config_t *route = hp_h2o_handler_config(ctx->pathconf->handlers.entries[i]);
            if (route && (route->mode == HP_ROUTE_SERVER || route->mode == HP_ROUTE_DIRECT)) {
                ++canonical_count; canonical_index = i; canonical = route;
            }
        }
        if (canonical_count) {
            size_t expected = 1 + (canonical->mode == HP_ROUTE_SERVER && canonical->file_server ? 1 : 0);
            if (canonical_count != 1 || num_dirs || ctx->pathconf->handlers.size != expected) {
                h2o_configurator_errprintf(NULL, node, "php_server/php owns its path; put file.dir, proxies and administration on separate paths");
                rc = -1; goto Done;
            }
            h2o_handler_t **handlers = ctx->pathconf->handlers.entries;
            h2o_handler_t *php = handlers[canonical_index];
            memmove(handlers + 1, handlers, canonical_index * sizeof(*handlers)); handlers[0] = php;
        }
        if (h2o_configurator_at_extension_level(ctx)) {
            yoml_t *extension = node ? yoml_get(node, "extension") : NULL;
            if (c->vars->root && extension_node_matches(c->vars, extension) &&
                !ctx->pathconf->handlers.size)
                register_automatic(c, ctx->pathconf);
        } else {
            if (num_dirs) {
                h2o_handler_t **handlers = ctx->pathconf->handlers.entries;
                size_t num_workers = 0, worker_index = 0;
                for (size_t i = 0; i < ctx->pathconf->handlers.size; ++i) {
                    if (hp_h2o_is_worker(handlers[i])) {
                        ++num_workers;
                        worker_index = i;
                    }
                }
                if (num_workers) {
                    if (num_workers != 1 || ctx->pathconf->handlers.size != num_dirs + 1) {
                        h2o_configurator_errprintf(NULL, node,
                            "php.worker with file.dir requires exactly one PHP worker and only file.dir handlers "
                            "at the same path; move other handlers to separate paths");
                        rc = -1; goto Done;
                    }
                    h2o_handler_t *worker = handlers[worker_index];
                    for (size_t i = 0; i < node->data.mapping.size; ++i) {
                        yoml_mapping_element_t *e = node->data.mapping.elements + i;
                        if (!strcmp(e->key->data.scalar, "file.dir") && e->value->type == YOML_TYPE_SCALAR)
                            hp_h2o_worker_add_static_root(worker, e->value->data.scalar);
                    }
                    memmove(handlers + 1, handlers, worker_index * sizeof(*handlers));
                    handlers[0] = worker;
                }
            }
            h2o_mimemap_type_t *php_type = h2o_mimemap_get_type_by_extension(*ctx->mimemap, h2o_iovec_init(H2O_STRLIT("php")));
            /* Explicit handlers keep their route; directory routes run PHP before serving assets. */
            if (c->vars->root && php_type->type != H2O_MIMEMAP_TYPE_DYNAMIC && ctx->pathconf->handlers.size == num_dirs) {
                register_automatic(c, ctx->pathconf);
                h2o_handler_t **handlers = ctx->pathconf->handlers.entries;
                h2o_handler_t *php = handlers[num_dirs];
                memmove(handlers + 1, handlers, num_dirs * sizeof(*handlers));
                handlers[0] = php;
                if (!num_dirs)
                    h2o_file_register(ctx->pathconf, c->vars->root, NULL, *ctx->mimemap, 0);
            }
        }
    }
Done:
    hp_route_config_dispose(c->vars);
    --c->vars; return rc;
}
static int dispose_config(h2o_configurator_t *self)
{
    php_configurator_t *c = (void *)self;
    hp_headers_free(c->ini_seen);
    while (c->catalog) { worker_catalog_t *next = c->catalog->next; free(c->catalog); c->catalog = next; }
    return 0;
}
void h2o_php_register_configurator(void *globalconf)
{
    hp_cpu_budget_capture_startup();
    hp_runtime_config_defaults();
    php_configurator_t *c = (void *)h2o_configurator_create(globalconf, sizeof(*c));
    c->vars = c->stack; c->vars->resolve_root_symlink = 1; c->vars->split_path = 1; c->vars->file_server = 1; c->vars->body_timeout_ms = 60000;
    c->vars->index_file = "index.php";
    c->super.enter = enter_config; c->super.exit = exit_config; c->super.dispose = dispose_config;
    const int scalar_global = H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR;
    const int scalar_all = H2O_CONFIGURATOR_FLAG_ALL_LEVELS | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR;
    const int route = H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_EXTENSION | H2O_CONFIGURATOR_FLAG_DEFERRED;
    h2o_configurator_define_command(&c->super, "php.runtime", H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_EXPECT_MAPPING, on_runtime);
    const char *keys[] = {"php.threads", "php.max-threads", "php.max-wait-time", "php.max-requests", "php.queue-limit", "php.max-buffered-body", "php.max-buffered-response", "php.max-buffered-global", "php.shutdown-timeout", "php.startup-timeout", "php.max-idle-time", "php.collect-metrics", "php.num_threads", "php.max_threads", "php.max_wait_time", "php.max_idle_time", "php.max_requests", "php.startup_timeout", "php.shutdown_timeout", "php.collect_metrics"};
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i) h2o_configurator_define_command(&c->super, keys[i], scalar_global, on_runtime_scalar);
    h2o_configurator_define_command(&c->super, "php.ini", H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_EXPECT_MAPPING, on_ini);
    h2o_configurator_define_command(&c->super, "php_ini", H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_EXPECT_MAPPING, on_ini);
    h2o_configurator_define_command(&c->super, "php.php_ini", H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_EXPECT_MAPPING, on_ini);
    h2o_configurator_define_command(&c->super, "php.log", scalar_global, on_log);
    h2o_configurator_define_command(&c->super, "php.root", scalar_all, on_root);
    h2o_configurator_define_command(&c->super, "php.index", scalar_all, on_index);
    h2o_configurator_define_command(&c->super, "php.resolve-root-symlink", scalar_all, on_symlink);
    h2o_configurator_define_command(&c->super, "php.resolve_root_symlink", scalar_all, on_symlink);
    h2o_configurator_define_command(&c->super, "php.split-path", H2O_CONFIGURATOR_FLAG_ALL_LEVELS | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR | H2O_CONFIGURATOR_FLAG_EXPECT_SEQUENCE, on_split_path);
    h2o_configurator_define_command(&c->super, "php.split_path", H2O_CONFIGURATOR_FLAG_ALL_LEVELS | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR | H2O_CONFIGURATOR_FLAG_EXPECT_SEQUENCE, on_split_path);
    h2o_configurator_define_command(&c->super, "php.request-timeout", scalar_all, on_timeout);
    h2o_configurator_define_command(&c->super, "php.request_timeout", scalar_all, on_timeout);
    h2o_configurator_define_command(&c->super, "php.request_body_timeout", scalar_all, on_timeout);
    h2o_configurator_define_command(&c->super, "php.response_timeout", scalar_all, on_timeout);
    h2o_configurator_define_command(&c->super, "php.request-body-timeout", scalar_all, on_timeout);
    h2o_configurator_define_command(&c->super, "php.response-timeout", scalar_all, on_timeout);
    h2o_configurator_define_command(&c->super, "php.env", H2O_CONFIGURATOR_FLAG_ALL_LEVELS | H2O_CONFIGURATOR_FLAG_EXPECT_MAPPING, on_env);
    h2o_configurator_define_command(&c->super, "php.worker", route | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR | H2O_CONFIGURATOR_FLAG_EXPECT_MAPPING | H2O_CONFIGURATOR_FLAG_EXPECT_SEQUENCE, on_worker);
    h2o_configurator_define_command(&c->super, "php_server", H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_DEFERRED | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR | H2O_CONFIGURATOR_FLAG_EXPECT_MAPPING, on_php);
    h2o_configurator_define_command(&c->super, "php", H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_DEFERRED | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR | H2O_CONFIGURATOR_FLAG_EXPECT_MAPPING, on_php);
    h2o_configurator_define_command(&c->super, "php.restart", H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_DEFERRED | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR, on_restart);
    h2o_configurator_define_command(&c->super, "php.metrics", H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_DEFERRED | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR | H2O_CONFIGURATOR_FLAG_EXPECT_MAPPING, on_metrics);
}
