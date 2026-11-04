#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "commands.h"
#include "../h2o/php_handler.h"
#include "cpu_budget.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sysexits.h>
#include <unistd.h>

typedef struct command_values {
    char *root, *listen, *index, *threads, *maximum;
    H2O_VECTOR(char *) workers;
    H2O_VECTOR(char *) watch;
    bool access_log, no_compress, no_file_server, dry_run, check;
} command_values_t;
static enum { COMMAND_NONE, COMMAND_EFFECTIVE, COMMAND_RENDER } command_mode;
static char *generated_yaml, *temporary_path;
static pid_t temporary_owner;

static void quote(FILE *out, const char *value)
{
    if (!value) { fputs("null", out); return; }
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') { fputc('\\', out); fputc(*p, out); }
        else if (*p < 0x20) fprintf(out, "\\u%04x", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}
static void remove_temporary(void)
{
    /* A PHP child must not remove the serving parent's reload input. */
    if (temporary_path && temporary_owner == getpid()) unlink(temporary_path);
}
static void free_values(command_values_t *v)
{
    free(v->root); free(v->listen); free(v->index); free(v->threads); free(v->maximum);
    for (size_t i = 0; i < v->workers.size; ++i) free(v->workers.entries[i]);
    for (size_t i = 0; i < v->watch.size; ++i) free(v->watch.entries[i]);
    free(v->workers.entries); free(v->watch.entries);
}
static int error(const char *text)
{
    fprintf(stderr, "h2o-php: %s\n", text); return EX_USAGE;
}
static void usage(FILE *out)
{
    fputs("Usage: h2o-php php-server [options]\n"
          "  --root, -r DIRECTORY       Document root (default: current directory)\n"
          "  --listen, -l HOST:PORT     HTTP listener (default: :80)\n"
          "  --worker, -w FILE[,NUM]    Persistent worker; may be repeated\n"
          "  --watch[=GLOB]             Watch first worker; repeat for more patterns\n"
          "  --index FILE              Front controller (default: index.php; OFF disables)\n"
          "  --num-threads N           Regular PHP thread minimum\n"
          "  --max-threads N|auto       Total PHP thread limit\n"
          "  --no-file-server          Disable H2O static-file fallback\n"
          "  --access-log, -a          Write native H2O access log to stdout\n"
          "  --no-compress             Disable native H2O response compression\n"
          "  --dry-run                 Validate through H2O, then print generated YAML\n"
          "  --check                   Validate through H2O without starting the server\n"
          "\nUsage: h2o-php effective-config -c FILE\n"
          "  Apply H2O configuration and print inherited PHP settings as JSON.\n", out);
}
static bool count_value(const char *value, bool allow_zero)
{
    if (!value || !*value) return false;
    uint64_t n = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p < '0' || *p > '9' || n > (INT_MAX - 2U - (*p - '0')) / 10U) return false;
        n = n * 10 + (*p - '0');
    }
    return allow_zero || n > 0;
}
static int replace(char **slot, const char *value)
{
    if (*slot) return error("a single-value command option was supplied more than once");
    if (!(*slot = strdup(value))) return EX_OSERR;
    return 0;
}
static int append_value(void *vector, const char *value)
{
    /* All command lists contain owned, NUL-terminated strings. */
    H2O_VECTOR(char *) *v = vector;
    h2o_vector_reserve(NULL, v, v->size + 1);
    if (!(v->entries[v->size] = strdup(value))) return EX_OSERR;
    ++v->size; return 0;
}
static int listen_address(const char *input, char **host, unsigned *port)
{
    const char *number = NULL, *end = NULL;
    if (input[0] == '[') {
        end = strchr(input + 1, ']');
        if (!end || end[1] != ':') return error("bracketed IPv6 listener requires a port");
        *host = strndup(input + 1, (size_t)(end - input - 1)); number = end + 2;
    } else {
        end = strrchr(input, ':');
        if (end) {
            if (memchr(input, ':', (size_t)(end - input))) return error("use [IPv6]:PORT for an IPv6 listener");
            *host = end == input ? strdup("0.0.0.0") : strndup(input, (size_t)(end - input)); number = end + 1;
        } else { *host = strdup("0.0.0.0"); number = input; }
    }
    if (!*host) return EX_OSERR;
    if (!count_value(number, true)) return error("listener port must be an integer between 0 and 65535");
    unsigned long n = strtoul(number, NULL, 10);
    if (n > 65535) return error("listener port must be an integer between 0 and 65535");
    *port = (unsigned)n; return 0;
}
static int render_server(command_values_t *v)
{
    char root[PATH_MAX], *host = NULL; unsigned port;
    struct stat st;
    if (!realpath(v->root ? v->root : ".", root) || stat(root, &st) || !S_ISDIR(st.st_mode))
        return error("--root must resolve to an existing directory");
    int status = listen_address(v->listen ? v->listen : ":80", &host, &port);
    if (status) { free(host); return status; }
    if (v->watch.size && !v->workers.size) { free(host); return error("--watch requires --worker"); }
    size_t yaml_size = 0;
    FILE *out = open_memstream(&generated_yaml, &yaml_size);
    if (!out) { free(host); return EX_OSERR; }
    fputs("listen:\n  host: ", out); quote(out, host); fprintf(out, "\n  port: %u\n", port); free(host);
    fputs("php.runtime:\n", out);
    if (v->threads) { fputs("  num_threads: ", out); quote(out, v->threads); fputc('\n', out); }
    if (v->maximum) { fputs("  max_threads: ", out); quote(out, v->maximum); fputc('\n', out); }
    if (!v->threads && !v->maximum) fputs("  num_threads: 0\n", out);
    fputs("hosts:\n  localhost:\n", out);
    if (v->access_log) fputs("    access-log: /dev/stdout\n", out);
    fprintf(out, "    compress: %s\n    paths:\n      /:\n        php_server:\n          root: ", v->no_compress ? "OFF" : "ON");
    quote(out, root); fputs("\n          index: ", out); quote(out, v->index ? v->index : "index.php");
    fprintf(out, "\n          file_server: %s\n", v->no_file_server ? "OFF" : "ON");
    if (v->workers.size) fputs("          worker:\n", out);
    for (size_t i = 0; i < v->workers.size; ++i) {
        char *worker = strdup(v->workers.entries[i]), *count = worker ? strchr(worker, ',') : NULL;
        if (!worker) { fclose(out); return EX_OSERR; }
        if (count) *count++ = 0;
        if (!*worker || (count && !count_value(count, true))) { free(worker); fclose(out); return error("--worker expects FILE or FILE,NUM with a nonnegative count"); }
        fputs("            - file: ", out); quote(out, worker); fputc('\n', out);
        if (count) { fputs("              num: ", out); quote(out, count); fputc('\n', out); }
        if (i == 0 && v->watch.size) {
            fputs("              watch:\n", out);
            for (size_t j = 0; j < v->watch.size; ++j) { fputs("                - ", out); quote(out, v->watch.entries[j]); fputc('\n', out); }
        }
        free(worker);
    }
    if (fclose(out)) return EX_OSERR;
    const char *tempdir = getenv("TMPDIR"); if (!tempdir || !*tempdir) tempdir = "/tmp";
    if (asprintf(&temporary_path, "%s/h2o-php-command-XXXXXX", tempdir) < 0) return EX_OSERR;
    int fd = mkstemp(temporary_path);
    if (fd < 0) { perror("h2o-php temporary configuration"); return EX_CANTCREAT; }
    temporary_owner = getpid(); atexit(remove_temporary);
    size_t offset = 0;
    while (offset < yaml_size) {
        ssize_t n = write(fd, generated_yaml + offset, yaml_size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return EX_IOERR; }
        offset += (size_t)n;
    }
    if (close(fd)) return EX_IOERR;
    return 0;
}
static int rewrite(int *argc, char ***argv, const char *config, bool check)
{
    char **next = calloc(check ? 5 : 4, sizeof(char *));
    if (!next) return EX_OSERR;
    size_t n = 0; next[n++] = (*argv)[0];
    if (check) next[n++] = "-t";
    next[n++] = "-c"; next[n++] = (char *)config;
    *argc = (int)n; *argv = next; return -1;
}
int hp_config_command_prepare(int *argc, char ***argv)
{
    if (*argc < 2) return -1;
    bool effective = !strcmp((*argv)[1], "effective-config");
    if (!effective && strcmp((*argv)[1], "php-server")) return -1;
    command_values_t v = {0}; char *config = NULL; int result = EX_USAGE;
    for (int i = 2; i < *argc; ++i) {
        const char *arg = (*argv)[i], *value = strchr(arg, '=');
        size_t keylen = value ? (size_t)(value - arg) : strlen(arg);
        char key[64];
        if (keylen >= sizeof(key)) { result = error("unknown command option"); goto Done; }
        memcpy(key, arg, keylen); key[keylen] = 0; if (value) ++value;
        if (!strcmp(key, "--help") || !strcmp(key, "-h")) { usage(stdout); result = 0; goto Done; }
        if (effective) {
            if (strcmp(key, "--config") && strcmp(key, "-c")) { result = error("effective-config accepts only --config/-c FILE"); goto Done; }
            if (!value && ++i < *argc) value = (*argv)[i];
            if (!value) { result = error("missing configuration filename"); goto Done; }
            if ((result = replace(&config, value))) goto Done;
            continue;
        }
        if (!strcmp(key, "--watch")) {
            /* Like FrankenPHP, the optional argument uses --watch=PATTERN. */
            if (!value) value = "**/*.{env,php,twig,yaml,yml}";
            if ((result = append_value(&v.watch, value))) goto Done;
            continue;
        }
        bool *toggle = NULL;
        if (!strcmp(key, "--access-log") || !strcmp(key, "-a")) toggle = &v.access_log;
        else if (!strcmp(key, "--no-compress")) toggle = &v.no_compress;
        else if (!strcmp(key, "--no-file-server")) toggle = &v.no_file_server;
        else if (!strcmp(key, "--dry-run") || !strcmp(key, "--print-config")) toggle = &v.dry_run;
        else if (!strcmp(key, "--check")) toggle = &v.check;
        if (toggle) {
            if (value) { result = error("boolean command flags do not take values"); goto Done; }
            *toggle = true; continue;
        }
        char **slot = NULL;
        if (!strcmp(key, "--root") || !strcmp(key, "-r")) slot = &v.root;
        else if (!strcmp(key, "--listen") || !strcmp(key, "-l")) slot = &v.listen;
        else if (!strcmp(key, "--index")) slot = &v.index;
        else if (!strcmp(key, "--num-threads") || !strcmp(key, "--num_threads")) slot = &v.threads;
        else if (!strcmp(key, "--max-threads") || !strcmp(key, "--max_threads")) slot = &v.maximum;
        else if (strcmp(key, "--worker") && strcmp(key, "-w")) {
            result = error("unsupported option; --help lists native php-server flags (configure TLS/domains in H2O YAML)"); goto Done;
        }
        if (!value && ++i < *argc) value = (*argv)[i];
        if (!value || !*value) { result = error("command option requires a nonempty value"); goto Done; }
        if (slot == &v.threads && !count_value(value, true)) { result = error("--num-threads requires a nonnegative integer"); goto Done; }
        if (slot == &v.maximum && strcmp(value, "auto") && !count_value(value, false)) { result = error("--max-threads requires auto or a positive integer"); goto Done; }
        if ((result = slot ? replace(slot, value) : append_value(&v.workers, value))) goto Done;
    }
    if (effective) {
        if (!config) { result = error("effective-config requires -c FILE"); goto Done; }
        command_mode = COMMAND_EFFECTIVE;
        /* argv must own the filename after temporary parse values are freed. */
        char *path = config; config = NULL; result = rewrite(argc, argv, path, true);
    } else {
        if ((result = render_server(&v))) goto Done;
        if (v.dry_run) command_mode = COMMAND_RENDER;
        result = rewrite(argc, argv, temporary_path, v.check || v.dry_run);
    }
Done:
    free(config); free_values(&v); return result;
}
bool hp_config_command_wants_dump(void) { return command_mode != COMMAND_NONE; }

static void environment(FILE *out, const hp_header_t *env)
{
    fputc('{', out); bool first = true;
    for (; env; env = env->next) {
        if (!first) fputc(',', out);
        first = false;
        quote(out, env->name); fputc(':', out); quote(out, env->value);
    }
    fputc('}', out);
}
static void worker_config(FILE *out, const hp_worker_config_t *w, const hp_route_match_t *match)
{
    fputs("{\"name\":", out); quote(out, w->name); fputs(",\"file\":", out); quote(out, w->file);
    fputs(",\"root\":", out); quote(out, w->root);
    fprintf(out, ",\"num\":%u,\"max_threads\":%u,\"max_requests\":%u,\"max_consecutive_failures\":%" PRId64 ",\"startup_timeout_ms\":%" PRIu64 ",\"env\":", w->num, w->max_threads, w->max_requests, w->max_consecutive_failures, w->startup_timeout_ms);
    environment(out, w->env); fputs(",\"watch\":[", out);
    for (size_t i = 0; i < w->num_watch; ++i) { if (i) fputc(',', out); quote(out, w->watch[i]); }
    fputs("],\"match\":", out);
    if (!match) fputs("null", out);
    else {
        fprintf(out, "{\"path_regexp\":%s,\"header_regexp_count\":%zu,\"paths\":[", match->has_path ? "true" : "false", match->headers.size);
        for (size_t i = 0; i < match->globs.size; ++i) { if (i) fputc(',', out); quote(out, match->globs.entries[i]); }
        fputs("],\"methods\":[", out);
        for (size_t i = 0; i < match->methods.size; ++i) { if (i) fputc(',', out); quote(out, match->methods.entries[i]); }
        fputs("]}", out);
    }
    fputc('}', out);
}
static void worker(FILE *out, hp_worker_group_t *group, const hp_route_match_t *match)
{
    worker_config(out, hp_worker_group_config(group), match);
}
typedef struct worker_snapshot {
    FILE *output;
    uint64_t minimum;
    bool first;
} worker_snapshot_t;
static void count_worker(const hp_worker_config_t *config, void *data)
{
    ((worker_snapshot_t *)data)->minimum += config->num;
}
static void dump_worker(const hp_worker_config_t *config, void *data)
{
    worker_snapshot_t *snapshot = data;
    if (!snapshot->first) fputc(',', snapshot->output);
    snapshot->first = false;
    worker_config(snapshot->output, config, NULL);
}
static void route(FILE *out, const hp_route_config_t *r)
{
    static const char *modes[] = {"automatic", "php_server", "php", "php.worker"};
    fputs("{\"mode\":", out); quote(out, r->metrics ? "metrics" : r->restart ? "worker_restart" : modes[r->mode]);
    fputs(",\"name\":", out); quote(out, r->name); fputs(",\"root\":", out); quote(out, r->root);
    fputs(",\"script\":", out); quote(out, r->script); fputs(",\"index\":", out); quote(out, r->index_file);
    fprintf(out, ",\"resolve_root_symlink\":%s,\"file_server\":%s,\"split_path_enabled\":%s,\"split_path\":[", r->resolve_root_symlink ? "true" : "false", r->file_server ? "true" : "false", r->split_path ? "true" : "false");
    size_t n = r->num_split_paths ? r->num_split_paths : 1;
    for (size_t i = 0; i < n; ++i) { if (i) fputc(',', out); quote(out, r->num_split_paths ? r->split_paths[i] : ".php"); }
    fprintf(out, "],\"request_timeout_ms\":%" PRIu64 ",\"request_body_timeout_ms\":%" PRIu64 ",\"response_delivery_timeout_ms\":%" PRIu64 ",\"env\":", r->timeout_set ? r->timeout_ms : hp_config.request_timeout_ms, r->body_timeout_ms, r->response_timeout_ms);
    environment(out, r->env); fputs(",\"workers\":[", out);
    if (r->workers) for (size_t i = 0; i < r->workers->entries.size; ++i) {
        if (i) fputc(',', out);
        worker(out, r->workers->entries.entries[i].group, r->workers->entries.entries[i].match);
    }
    else if (r->group) worker(out, r->group, r->match);
    fputs("]}", out);
}
int hp_config_command_dump(const h2o_globalconf_t *global, FILE *out)
{
    if (hp_runtime_config_prepare()) return EX_CONFIG;
    if (command_mode == COMMAND_RENDER) {
        if (!generated_yaml || fputs(generated_yaml, out) == EOF) return EX_IOERR;
        return fflush(out) ? EX_IOERR : 0;
    }
    char cpu_budget[HP_CPU_BUDGET_JSON_CAP];
    if (!hp_cpu_budget_json(cpu_budget, sizeof(cpu_budget))) return EX_SOFTWARE;
    worker_snapshot_t snapshot = {.output = out, .first = true};
    hp_runtime_each_worker(count_worker, &snapshot);
    fputs("{\"schema\":\"h2o-php-effective-v1\",\"scope\":\"inherited PHP configuration\",\"php_engine_started\":false,\"runtime\":{", out);
    fprintf(out, "\"num_threads\":%u,\"num_threads_explicit\":%s,\"max_threads\":", hp_config.threads, hp_config.threads_explicit ? "true" : "false");
    if (hp_config.max_threads_auto) quote(out, "auto"); else fprintf(out, "%u", hp_config.max_threads);
    fprintf(out, ",\"automatic_cap_pending_php_memory_limit\":%s,\"worker_minimum\":%" PRIu64 ",\"total_minimum\":%" PRIu64 ",\"max_wait_time_ms\":%" PRIu64 ",\"max_idle_time_ms\":%" PRIu64 ",\"max_requests\":%u,\"queue_limit\":%u,\"queue_limit_unlimited\":%s,\"max_buffered_body\":%zu,\"max_buffered_response\":%zu,\"max_buffered_global\":%zu,\"request_timeout_ms\":%" PRIu64 ",\"startup_timeout_ms\":%" PRIu64 ",\"shutdown_timeout_ms\":%" PRIu64 ",\"php_ini\":", hp_config.max_threads_auto ? "true" : "false", snapshot.minimum, snapshot.minimum + hp_config.threads, hp_config.max_wait_ms, hp_config.max_idle_ms, hp_config.max_requests, hp_config.queue_limit, hp_config.queue_limit == 0 ? "true" : "false", hp_config.max_buffered_body, hp_config.max_buffered_response, hp_config.max_buffered_global, hp_config.request_timeout_ms, hp_config.startup_timeout_ms, hp_config.shutdown_timeout_ms);
    quote(out, hp_config.ini_entries); fputs(",\"log\":", out); quote(out, hp_config.log_path);
    fputs(",\"cpu_budget\":", out); fputs(cpu_budget, out);
    fputs("},\"worker_pools\":[", out); hp_runtime_each_worker(dump_worker, &snapshot);
    fputs("],\"hosts\":[", out);
    for (size_t h = 0; global->hosts && global->hosts[h]; ++h) {
        h2o_hostconf_t *host = global->hosts[h]; if (h) fputc(',', out);
        fputs("{\"host\":", out); quote(out, host->authority.hostport.base); fputs(",\"paths\":[", out);
        for (size_t p = 0; p < host->paths.size; ++p) {
            h2o_pathconf_t *path = host->paths.entries[p]; if (p) fputc(',', out);
            fputs("{\"path\":", out); quote(out, path->path.base);
            fprintf(out, ",\"h2o_handler_count\":%zu,\"php_handlers\":[", path->handlers.size);
            bool first = true;
            for (size_t k = 0; k < path->handlers.size; ++k) {
                const hp_route_config_t *r = hp_h2o_handler_config(path->handlers.entries[k]);
                if (!r) continue;
                if (!first) fputc(',', out);
                first = false; route(out, r);
            }
            fputs("],\"php_extension_handlers\":[", out);
            h2o_mimemap_type_t *extension = path->mimemap
                ? h2o_mimemap_get_type_by_extension(path->mimemap, h2o_iovec_init(H2O_STRLIT("php"))) : NULL;
            first = true;
            if (extension && extension->type == H2O_MIMEMAP_TYPE_DYNAMIC) {
                h2o_pathconf_t *dynamic = &extension->data.dynamic.pathconf;
                for (size_t k = 0; k < dynamic->handlers.size; ++k) {
                    const hp_route_config_t *r = hp_h2o_handler_config(dynamic->handlers.entries[k]);
                    if (!r) continue;
                    if (!first) fputc(',', out);
                    first = false; route(out, r);
                }
            }
            fprintf(out, "],\"dynamic_extension_mapping_present\":%s}",
                    path->mimemap && h2o_mimemap_has_dynamic_type(path->mimemap) ? "true" : "false");
        }
        fputs("]}", out);
    }
    fputs("]}\n", out); return ferror(out) || fflush(out) ? EX_IOERR : 0;
}
