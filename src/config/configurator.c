#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../h2o/php_handler.h"
#include "h2o/configurator.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

typedef struct {
    h2o_configurator_t super;
    hp_route_config_t *vars;
    hp_route_config_t stack[H2O_CONFIGURATOR_NUM_LEVELS + 2];
} php_configurator_t;
static int scalar(h2o_configurator_command_t *cmd, yoml_t *node)
{
    if (node->type == YOML_TYPE_SCALAR) return 0;
    h2o_configurator_errprintf(cmd, node, "value must be a scalar"); return -1;
}
static int number(h2o_configurator_command_t *cmd, yoml_t *node, uint64_t *out, int duration)
{
    if (scalar(cmd, node)) return -1;
    char *end; errno = 0;
    if (*node->data.scalar == '-' || !*node->data.scalar) goto Invalid;
    unsigned long long value = strtoull(node->data.scalar, &end, 10);
    uint64_t factor = 1;
    if (duration && *end) {
        if (!strcmp(end, "ms")) factor = 1;
        else if (!strcmp(end, "s")) factor = 1000;
        else if (!strcmp(end, "m")) factor = 60000;
        else goto Invalid;
    } else if (*end) goto Invalid;
    if (errno || value > UINT64_MAX / factor) goto Invalid;
    *out = value * factor; return 0;
Invalid:
    h2o_configurator_errprintf(cmd, node, duration ? "expected nonnegative duration (ms, s, m)" : "expected nonnegative integer");
    return -1;
}
static int set_runtime_value(h2o_configurator_command_t *cmd, const char *key, yoml_t *node)
{
    uint64_t value;
    int duration = strstr(key, "time") != NULL;
    if (number(cmd, node, &value, duration)) return -1;
    if (!strcmp(key, "threads") || !strcmp(key, "max-threads") || !strcmp(key, "queue-limit") || !strcmp(key, "max-requests")) {
        if (value > UINT_MAX || ((!strcmp(key, "threads") || !strcmp(key, "max-threads") || !strcmp(key, "queue-limit")) && !value)) goto Range;
        if (!strcmp(key, "threads")) hp_config.threads = (unsigned)value;
        else if (!strcmp(key, "max-threads")) hp_config.max_threads = (unsigned)value;
        else if (!strcmp(key, "queue-limit")) hp_config.queue_limit = (unsigned)value;
        else hp_config.max_requests = (unsigned)value;
    } else if (!strcmp(key, "max-wait-time")) hp_config.max_wait_ms = value;
    else if (!strcmp(key, "request-timeout")) hp_config.request_timeout_ms = value;
    else if (!strcmp(key, "shutdown-timeout")) hp_config.shutdown_timeout_ms = value;
    else if (!strcmp(key, "max-buffered-body") || !strcmp(key, "max-buffered-response") || !strcmp(key, "max-buffered-global")) {
        if (value < 4096 || value > SIZE_MAX) goto Range;
        if (!strcmp(key, "max-buffered-body")) hp_config.max_buffered_body = (size_t)value;
        else if (!strcmp(key, "max-buffered-response")) hp_config.max_buffered_response = (size_t)value;
        else hp_config.max_buffered_global = (size_t)value;
    } else {
        h2o_configurator_errprintf(cmd, node, "unknown PHP runtime option: %s", key); return -1;
    }
    return 0;
Range:
    h2o_configurator_errprintf(cmd, node, "PHP runtime option out of range: %s", key); return -1;
}
static int on_runtime(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx;
    for (size_t i = 0; i < node->data.mapping.size; ++i) {
        yoml_mapping_element_t *e = node->data.mapping.elements + i;
        if (scalar(cmd, e->key) || set_runtime_value(cmd, e->key->data.scalar, e->value)) return -1;
    }
    return 0;
}
static int on_runtime_scalar(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx; return set_runtime_value(cmd, cmd->name + 4, node);
}
static int on_root(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx;
    php_configurator_t *c = (void *)cmd->configurator;
    char path[PATH_MAX];
    if (!realpath(node->data.scalar, path)) {
        h2o_configurator_errprintf(cmd, node, "cannot resolve PHP root: %s", node->data.scalar); return -1;
    }
    struct stat st;
    if (stat(path, &st) || !S_ISDIR(st.st_mode)) {
        h2o_configurator_errprintf(cmd, node, "PHP root must be a directory"); return -1;
    }
    free(c->vars->root); c->vars->root = strdup(path); return 0;
}
static int on_front(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx; php_configurator_t *c = (void *)cmd->configurator;
    if (node->data.scalar[0] == '/' || strstr(node->data.scalar, "..")) {
        h2o_configurator_errprintf(cmd, node, "front controller must be a relative PHP path within php.root"); return -1;
    }
    free(c->vars->front_controller); c->vars->front_controller = strdup(node->data.scalar); return 0;
}
static int on_timeout(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx; php_configurator_t *c = (void *)cmd->configurator;
    return number(cmd, node, &c->vars->timeout_ms, 1);
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
    for (size_t i = 0; i < node->data.mapping.size; ++i) {
        yoml_mapping_element_t *e = node->data.mapping.elements + i;
        if (scalar(cmd, e->key) || scalar(cmd, e->value)) return -1;
        if (strpbrk(e->key->data.scalar, "\r\n=\"") || strpbrk(e->value->data.scalar, "\r\n\"")) {
            h2o_configurator_errprintf(cmd, e->key, "INI keys and values must not contain newlines or quotes"); return -1;
        }
        char *new_entries;
        if (asprintf(&new_entries, "%s%s=\"%s\"\n", hp_config.ini_entries ? hp_config.ini_entries : "", e->key->data.scalar, e->value->data.scalar) < 0) return -1;
        free(hp_config.ini_entries); hp_config.ini_entries = new_entries;
    }
    return 0;
}
static int on_classic(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    php_configurator_t *c = (void *)cmd->configurator;
    ssize_t enabled = h2o_configurator_get_one_of(cmd, node, "OFF,ON");
    if (enabled < 0) return -1;
    if (enabled) {
        if (!c->vars->root) { h2o_configurator_errprintf(cmd, node, "php.classic requires php.root at this level or an inherited level"); return -1; }
        hp_h2o_register(ctx->pathconf, c->vars);
    }
    return 0;
}
static int on_metrics(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    ssize_t enabled = h2o_configurator_get_one_of(cmd, node, "OFF,ON");
    if (enabled < 0) return -1;
    if (enabled) { hp_route_config_t route = {.metrics = 1}; hp_h2o_register(ctx->pathconf, &route); }
    return 0;
}
static int on_restart(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    ssize_t enabled = h2o_configurator_get_one_of(cmd, node, "OFF,ON");
    if (enabled < 0) return -1;
    if (enabled) { hp_route_config_t route = {.restart = 1}; hp_h2o_register(ctx->pathconf, &route); }
    return 0;
}
static int on_split_path(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx; php_configurator_t *c = (void *)cmd->configurator;
    ssize_t enabled = h2o_configurator_get_one_of(cmd, node, "OFF,ON");
    if (enabled < 0) return -1;
    c->vars->split_path = enabled; return 0;
}
static int on_symlink(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx; php_configurator_t *c = (void *)cmd->configurator;
    ssize_t enabled = h2o_configurator_get_one_of(cmd, node, "OFF,ON");
    if (enabled < 0) return -1;
    if (!enabled) { h2o_configurator_errprintf(cmd, node, "php.resolve-root-symlink must be ON; unresolved roots are not supported"); return -1; }
    c->vars->resolve_root_symlink = 1; return 0;
}
static int on_worker(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    php_configurator_t *c = (void *)cmd->configurator;
    hp_worker_config_t worker = {.num = 1, .max_requests = hp_config.max_requests, .max_consecutive_failures = 10, .root = c->vars->root};
    worker.env = hp_headers_clone(c->vars->env);
    int rc = -1;
    for (size_t i = 0; i < node->data.mapping.size; ++i) {
        yoml_mapping_element_t *e = node->data.mapping.elements + i;
        if (scalar(cmd, e->key)) goto Done;
        const char *key = e->key->data.scalar;
        if (!strcmp(key, "env")) { if (parse_env(cmd, e->value, &worker.env)) goto Done; }
        else if (!strcmp(key, "watch")) {
            if (e->value->type != YOML_TYPE_SEQUENCE) { h2o_configurator_errprintf(cmd, e->value, "watch must be a sequence"); goto Done; }
            worker.num_watch = e->value->data.sequence.size;
            worker.watch = calloc(worker.num_watch, sizeof(char *));
            for (size_t j = 0; j < worker.num_watch; ++j) {
                yoml_t *entry = e->value->data.sequence.elements[j];
                char path[PATH_MAX];
                if (scalar(cmd, entry) || !realpath(entry->data.scalar, path)) {
                    h2o_configurator_errprintf(cmd, entry, "watch path does not exist"); goto Done;
                }
                worker.watch[j] = strdup(path);
            }
        } else if (!strcmp(key, "file") || !strcmp(key, "name")) {
            if (scalar(cmd, e->value)) goto Done;
            if (!strcmp(key, "file")) worker.file = e->value->data.scalar; else worker.name = e->value->data.scalar;
        } else if (!strcmp(key, "num") || !strcmp(key, "max-requests") || !strcmp(key, "max-consecutive-failures")) {
            uint64_t n;
            if (number(cmd, e->value, &n, 0)) goto Done;
            if (n > UINT_MAX || ((!strcmp(key, "num") || !strcmp(key, "max-consecutive-failures")) && !n)) {
                h2o_configurator_errprintf(cmd, e->value, "worker count/limit out of range"); goto Done;
            }
            if (!strcmp(key, "num")) worker.num = (unsigned)n;
            else if (!strcmp(key, "max-requests")) worker.max_requests = (unsigned)n;
            else worker.max_consecutive_failures = (unsigned)n;
        } else { h2o_configurator_errprintf(cmd, e->key, "unknown PHP worker option: %s", key); goto Done; }
    }
    char file[PATH_MAX];
    struct stat st;
    if (!worker.file || !realpath(worker.file, file) || stat(file, &st) || !S_ISREG(st.st_mode)) {
        h2o_configurator_errprintf(cmd, node, "worker file must resolve to a regular PHP script"); goto Done;
    }
    worker.file = file;
    hp_route_config_t route = *c->vars;
    route.group = hp_worker_group_create(&worker);
    if (!route.group) { h2o_configurator_errprintf(cmd, node, "could not allocate PHP worker group"); goto Done; }
    hp_h2o_register(ctx->pathconf, &route); rc = 0;
Done:
    hp_headers_free(worker.env);
    for (size_t i = 0; i < worker.num_watch; ++i) free(worker.watch[i]);
    free(worker.watch); return rc;
}
static int enter_config(h2o_configurator_t *self, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx; (void)node; php_configurator_t *c = (void *)self;
    c->vars[1] = *c->vars; ++c->vars;
    c->vars->root = c->vars->root ? strdup(c->vars->root) : NULL;
    c->vars->front_controller = c->vars->front_controller ? strdup(c->vars->front_controller) : NULL;
    c->vars->env = hp_headers_clone(c->vars->env); return 0;
}
static int exit_config(h2o_configurator_t *self, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)ctx; (void)node; php_configurator_t *c = (void *)self;
    free(c->vars->root); free(c->vars->front_controller); hp_headers_free(c->vars->env);
    --c->vars; return 0;
}
void h2o_php_register_configurator(void *globalconf)
{
    hp_runtime_config_defaults();
    php_configurator_t *c = (void *)h2o_configurator_create(globalconf, sizeof(*c));
    c->vars = c->stack; c->vars->resolve_root_symlink = 1; c->vars->split_path = 1;
    c->super.enter = enter_config; c->super.exit = exit_config;
    const int scalar_global = H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR;
    const int scalar_all = H2O_CONFIGURATOR_FLAG_ALL_LEVELS | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR;
    const int route = H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_EXTENSION | H2O_CONFIGURATOR_FLAG_DEFERRED;
    h2o_configurator_define_command(&c->super, "php.runtime", H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_EXPECT_MAPPING, on_runtime);
    const char *keys[] = {"php.threads", "php.max-threads", "php.max-wait-time", "php.max-requests", "php.queue-limit", "php.max-buffered-body", "php.max-buffered-response", "php.max-buffered-global", "php.shutdown-timeout"};
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i) h2o_configurator_define_command(&c->super, keys[i], scalar_global, on_runtime_scalar);
    h2o_configurator_define_command(&c->super, "php.ini", H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_EXPECT_MAPPING, on_ini);
    h2o_configurator_define_command(&c->super, "php.root", scalar_all, on_root);
    h2o_configurator_define_command(&c->super, "php.front-controller", scalar_all, on_front);
    h2o_configurator_define_command(&c->super, "php.resolve-root-symlink", scalar_all, on_symlink);
    h2o_configurator_define_command(&c->super, "php.split-path", scalar_all, on_split_path);
    h2o_configurator_define_command(&c->super, "php.request-timeout", scalar_all, on_timeout);
    h2o_configurator_define_command(&c->super, "php.env", H2O_CONFIGURATOR_FLAG_ALL_LEVELS | H2O_CONFIGURATOR_FLAG_EXPECT_MAPPING, on_env);
    h2o_configurator_define_command(&c->super, "php.classic", route | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR, on_classic);
    h2o_configurator_define_command(&c->super, "php.worker", route | H2O_CONFIGURATOR_FLAG_EXPECT_MAPPING, on_worker);
    h2o_configurator_define_command(&c->super, "php.restart", H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_DEFERRED | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR, on_restart);
    h2o_configurator_define_command(&c->super, "php.metrics", H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_DEFERRED | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR, on_metrics);
}
