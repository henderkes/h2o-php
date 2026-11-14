#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lifecycle.h"
#include "yoml.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

#define INPUT_LIMIT (16U * 1024U * 1024U)
#define PROBE_ENV "H2O_PHP_CONFIG_PROBE_FD"
#define WIRE_MAGIC 0x48325031U
typedef struct { uint32_t magic, command, force, length; } request_t;
typedef struct { int32_t status; uint32_t length; } response_t;
static volatile sig_atomic_t interrupted;
static void interrupt_run(int signo) { interrupted = signo; }
static bool run_cancelled(void) { return interrupted != 0; }

static uint64_t milliseconds(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int transfer(int fd, void *data, size_t length, bool writing, uint64_t until)
{
    char *p = data;
    while (length) {
        uint64_t now = milliseconds();
        if (now >= until) { errno = ETIMEDOUT; return -1; }
        struct pollfd event = {.fd = fd, .events = writing ? POLLOUT : POLLIN};
        uint64_t remaining = until - now;
        int ready = poll(&event, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (!ready) { errno = ETIMEDOUT; return -1; }
        if (ready < 0) return -1;
        ssize_t n = writing ? send(fd, p, length, MSG_NOSIGNAL | MSG_DONTWAIT) : recv(fd, p, length, MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) { if (!n) errno = ECONNRESET; return -1; }
        p += n; length -= (size_t)n;
    }
    return 0;
}
static int write_all(int fd, const void *data, size_t length)
{
    const char *p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; length -= (size_t)n;
    }
    return 0;
}
static char *read_file(const char *path, size_t *length)
{
    FILE *file = !strcmp(path, "-") ? stdin : fopen(path, "rb");
    if (!file) return NULL;
    size_t capacity = 4096, used = 0;
    char *text = malloc(capacity + 1);
    if (!text) { if (file != stdin) fclose(file); return NULL; }
    for (;;) {
        size_t n = fread(text + used, 1, capacity - used, file); used += n;
        if (used < capacity) {
            if (ferror(file)) { free(text); text = NULL; }
            break;
        }
        if (capacity == INPUT_LIMIT) { free(text); text = NULL; errno = EFBIG; break; }
        capacity *= 2;
        char *next = realloc(text, capacity + 1);
        if (!next) { free(text); text = NULL; break; }
        text = next;
    }
    if (file != stdin) fclose(file);
    if (text) { text[used] = 0; *length = used; }
    return text;
}
void hp_lifecycle_retain(hp_lifecycle_config_t *config) { if (config) ++config->refs; }
void hp_lifecycle_release(hp_lifecycle_config_t *config)
{
    if (!config || --config->refs) return;
    if (config->path) unlink(config->path);
    if (config->argv) {
        for (size_t i = 0; config->argv[i]; ++i) free(config->argv[i]);
        free(config->argv);
    }
    free(config->yaml); free(config->path); free(config);
}
static void quote(FILE *out, const char *value)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') { fputc('\\', out); fputc(*p, out); }
        else if (*p < 32) fprintf(out, "\\u%04x", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}
static int resolved_yaml(FILE *out, const yoml_t *node, unsigned depth)
{
    if (depth > 256 || ftell(out) > INPUT_LIMIT) return -1;
    switch (node->type) {
    case YOML_TYPE_SCALAR: quote(out, node->data.scalar); break;
    case YOML_TYPE_SEQUENCE:
        fputc('[', out);
        for (size_t i = 0; i < node->data.sequence.size; ++i) {
            if (i) fputc(',', out);
            if (resolved_yaml(out, node->data.sequence.elements[i], depth + 1)) return -1;
        }
        fputc(']', out); break;
    case YOML_TYPE_MAPPING:
        fputc('{', out);
        for (size_t i = 0; i < node->data.mapping.size; ++i) {
            if (i) fputc(',', out);
            if (node->data.mapping.elements[i].key->type != YOML_TYPE_SCALAR ||
                resolved_yaml(out, node->data.mapping.elements[i].key, depth + 1)) return -1;
            fputc(':', out);
            if (resolved_yaml(out, node->data.mapping.elements[i].value, depth + 1)) return -1;
        }
        fputc('}', out); break;
    default: return -1;
    }
    return ferror(out) || ftell(out) > INPUT_LIMIT ? -1 : 0;
}
char *hp_lifecycle_resolve_yaml(const yoml_t *document)
{
    if (document->type == YOML_TYPE_MAPPING) for (size_t i = 0; i < document->data.mapping.size; ++i) {
        const yoml_t *key = document->data.mapping.elements[i].key;
        if (key->type == YOML_TYPE_SCALAR && !strcmp(key->data.scalar, "acme")) {
            fputs("h2o-php run: the native ACME loader requires -m master; use externally managed certificates with run\n", stderr);
            return NULL;
        }
    }
    char *yaml = NULL; size_t length = 0;
    FILE *out = open_memstream(&yaml, &length);
    if (!out) return NULL;
    int result = resolved_yaml(out, document, 0);
    if (fclose(out)) result = -1;
    if (result) { free(yaml); return NULL; }
    return yaml;
}
int hp_lifecycle_write_probe(char *const argv[], uint64_t startup_ms, uint64_t shutdown_ms, const char *yaml)
{
    const char *value = getenv(PROBE_ENV);
    if (!value) return -1;
    char *end; errno = 0; long fd = strtol(value, &end, 10);
    if (errno || !*value || *end || fd < 3 || fd > INT_MAX) return EX_USAGE;
    uint64_t times[] = {startup_ms, shutdown_ms};
    if (write_all((int)fd, times, sizeof(times))) return EX_IOERR;
    if (write_all((int)fd, yaml, strlen(yaml) + 1)) return EX_IOERR;
    for (size_t i = 0; argv[i]; ++i)
        if (write_all((int)fd, argv[i], strlen(argv[i]) + 1)) return EX_IOERR;
    return 0;
}
struct hp_lifecycle_validation {
    hp_lifecycle_config_t *config;
    int manifest, diagnostics;
    pid_t pid;
    uint64_t until;
    char *failure;
    bool cancelled;
};
void hp_lifecycle_validation_destroy(hp_lifecycle_validation_t *validation)
{
    if (!validation) return;
    /* Active probes must be cancelled and reaped through step first. */
    if (validation->manifest >= 0) close(validation->manifest);
    if (validation->diagnostics >= 0) close(validation->diagnostics);
    hp_lifecycle_release(validation->config);
    free(validation->failure); free(validation);
}
hp_lifecycle_validation_t *hp_lifecycle_validation_begin(const char *executable, const char *yaml,
                                                         size_t length, char **error)
{
    *error = NULL;
    hp_lifecycle_validation_t *validation = calloc(1, sizeof(*validation));
    if (!validation) { *error = strdup("cannot prepare configuration"); return NULL; }
    validation->manifest = validation->diagnostics = -1;
    hp_lifecycle_config_t *config = calloc(1, sizeof(*config));
    validation->config = config;
    int input = -1;
    char manifest_path[] = "/tmp/h2o-php-manifest-XXXXXX", log_path[] = "/tmp/h2o-php-validate-XXXXXX";
    if (!config) goto Error;
    config->refs = 1; config->length = length;
    config->yaml = strndup(yaml, length);
    config->path = strdup("/tmp/h2o-php-config-XXXXXX");
    if (!config->yaml || !config->path || memchr(yaml, 0, length)) goto Error;
    input = mkstemp(config->path);
    if (input < 0 || write_all(input, yaml, length)) goto Error;
    close(input); input = -1;
    validation->manifest = mkstemp(manifest_path); validation->diagnostics = mkstemp(log_path);
    unlink(manifest_path); unlink(log_path);
    if (validation->manifest < 0 || validation->diagnostics < 0) goto Error;
    /* A separate pre-thread process uses the real H2O configurator. It never
     * binds application listeners or initializes PHP. Its output is a bounded
     * private descriptor manifest, not a second YAML parser. */
    pid_t pid = fork();
    if (pid < 0) goto Error;
    if (!pid) {
        char descriptor[32]; snprintf(descriptor, sizeof(descriptor), "%d", validation->manifest);
        if (setenv(PROBE_ENV, descriptor, 1)) _exit(EX_OSERR);
        unsetenv("SERVER_STARTER_PORT"); unsetenv("H2O_PHP_CLI_CHILD");
        unsetenv("H2O_PHP_READY_FD"); unsetenv("H2O_VIA_MASTER");
        if (dup2(validation->diagnostics, STDERR_FILENO) < 0 || dup2(validation->diagnostics, STDOUT_FILENO) < 0) _exit(EX_OSERR);
        execlp(executable, executable, "-m", "master", "-c", config->path, (char *)NULL);
        _exit(EX_OSERR);
    }
    validation->pid = pid; validation->until = milliseconds() + 30000;
    return validation;
Error:
    if (input >= 0) close(input);
    *error = strdup("cannot prepare configuration");
    hp_lifecycle_validation_destroy(validation); return NULL;
}
uint64_t hp_lifecycle_validation_deadline(const hp_lifecycle_validation_t *validation) { return validation->until; }
void hp_lifecycle_validation_cancel(hp_lifecycle_validation_t *validation, const char *message)
{
    if (validation->cancelled) return;
    validation->cancelled = true; validation->failure = strdup(message);
    if (validation->pid > 0) kill(validation->pid, SIGKILL);
    /* Reaping remains asynchronous; SIGCHLD wakes the supervisor. */
    validation->until = UINT64_MAX;
}
int hp_lifecycle_validation_step(hp_lifecycle_validation_t *validation, hp_lifecycle_config_t **result, char **error)
{
    *result = NULL; *error = NULL;
    if (milliseconds() >= validation->until)
        hp_lifecycle_validation_cancel(validation, "configuration validation timed out");
    int status;
    pid_t reaped = waitpid(validation->pid, &status, WNOHANG);
    if (!reaped || (reaped < 0 && errno == EINTR)) return 0;
    validation->pid = 0;
    if (validation->cancelled) { *error = strdup(validation->failure ? validation->failure : "configuration validation cancelled"); return 1; }
    if (reaped < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) {
        char message[8193]; lseek(validation->diagnostics, 0, SEEK_SET);
        ssize_t n = read(validation->diagnostics, message, sizeof(message) - 1);
        if (n > 0) { message[n] = 0; *error = strdup(message); }
        goto Error;
    }
    hp_lifecycle_config_t *config = validation->config;
    struct stat st;
    if (fstat(validation->manifest, &st) || st.st_size < 16 || st.st_size > INPUT_LIMIT) goto Error;
    size_t size = (size_t)st.st_size;
    char *bytes = malloc(size + 1);
    if (!bytes) goto Error;
    lseek(validation->manifest, 0, SEEK_SET);
    size_t used = 0;
    while (used < size) {
        ssize_t n = read(validation->manifest, bytes + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(bytes); goto Error; }
        used += n;
    }
    uint64_t times[2]; memcpy(times, bytes, sizeof(times));
    config->startup_ms = times[0]; config->shutdown_ms = times[1];
    size_t count = 0;
    for (size_t i = sizeof(times); i < size; ++i) if (!bytes[i]) ++count;
    if (count < 2 || bytes[size - 1]) { free(bytes); goto Error; }
    char *canonical = bytes + sizeof(times);
    free(config->yaml); config->yaml = strdup(canonical); config->length = strlen(canonical);
    if (!config->yaml) { free(bytes); goto Error; }
    int input = open(config->path, O_WRONLY | O_TRUNC);
    if (input < 0) { free(bytes); goto Error; }
    int written = write_all(input, canonical, config->length), closed = close(input);
    if (written || closed) { free(bytes); goto Error; }
    --count;
    config->argv = calloc(count + 1, sizeof(char *));
    if (!config->argv) { free(bytes); goto Error; }
    char *p = canonical + config->length + 1;
    for (size_t i = 0; i < count; ++i) {
        config->argv[i] = strdup(p); p += strlen(p) + 1;
        if (!config->argv[i]) { free(bytes); goto Error; }
    }
    free(bytes);
    *result = config; validation->config = NULL;
    return 1;
Error:
    if (!*error) *error = strdup("cannot prepare configuration");
    return 1;
}
hp_lifecycle_config_t *hp_lifecycle_load(const char *executable, const char *yaml, size_t length, char **error,
                                        bool (*cancelled)(void))
{
    hp_lifecycle_validation_t *validation = hp_lifecycle_validation_begin(executable, yaml, length, error);
    if (!validation) return NULL;
    hp_lifecycle_config_t *config;
    for (;;) {
        if (cancelled && cancelled()) hp_lifecycle_validation_cancel(validation, "configuration validation interrupted");
        if (hp_lifecycle_validation_step(validation, &config, error)) break;
        struct timespec pause = {.tv_nsec = 10000000}; nanosleep(&pause, NULL);
    }
    hp_lifecycle_validation_destroy(validation);
    return config;
}
/* All client slots, including queued/applying requests and pending replies,
 * count against the cap. Body reservations remain charged until the reply,
 * even after ownership has moved to the supervisor's reload queue. */
#define CONTROL_BUFFER_LIMIT (32U * 1024U * 1024U)
#define CONTROL_IO_BUDGET (64U * 1024U)
#define CONTROL_REPLY_LIMIT 8192U
enum client_state { CLIENT_FREE, CLIENT_HEADER, CLIENT_BODY, CLIENT_READY, CLIENT_WAITING, CLIENT_REPLY };
struct hp_lifecycle_client {
    struct hp_lifecycle_control *owner;
    enum client_state state;
    int fd;
    uint64_t until;
    request_t request;
    size_t used, reserved;
    char *body, *output;
    size_t output_length;
};
struct hp_lifecycle_control {
    hp_lifecycle_client_t clients[HP_LIFECYCLE_CLIENT_LIMIT];
    size_t buffered;
};
static void close_client(hp_lifecycle_client_t *client)
{
    if (client->state == CLIENT_FREE) return;
    close(client->fd);
    client->owner->buffered -= client->reserved;
    free(client->body); free(client->output);
    *client = (hp_lifecycle_client_t){0};
}
hp_lifecycle_control_t *hp_lifecycle_control_create(void) { return calloc(1, sizeof(hp_lifecycle_control_t)); }
void hp_lifecycle_control_destroy(hp_lifecycle_control_t *control)
{
    if (!control) return;
    for (size_t i = 0; i < HP_LIFECYCLE_CLIENT_LIMIT; ++i) close_client(&control->clients[i]);
    free(control);
}
bool hp_lifecycle_control_has_capacity(const hp_lifecycle_control_t *control)
{
    if (control) for (size_t i = 0; i < HP_LIFECYCLE_CLIENT_LIMIT; ++i)
        if (control->clients[i].state == CLIENT_FREE) return true;
    return false;
}
void hp_lifecycle_control_accept(hp_lifecycle_control_t *control, int listener, uint64_t now)
{
    /* Finite work per turn, even under an uninterrupted connection flood. */
    for (size_t i = 0; i < HP_LIFECYCLE_CLIENT_LIMIT; ++i) {
        hp_lifecycle_client_t *client = &control->clients[i];
        if (client->state != CLIENT_FREE) continue;
        int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) return;
        *client = (hp_lifecycle_client_t){.owner = control, .state = CLIENT_HEADER, .fd = fd, .until = now + 5000};
    }
}
static void send_reply(hp_lifecycle_client_t *client)
{
    size_t remaining = client->output_length - client->used;
    ssize_t n = send(client->fd, client->output + client->used, remaining, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
        client->used += (size_t)n;
        if (client->used == client->output_length) close_client(client);
    } else if (!n || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) close_client(client);
}
void hp_lifecycle_reply(hp_lifecycle_client_t *client, int status, const char *message)
{
    if (!client) return;
    size_t length = strnlen(message, CONTROL_REPLY_LIMIT);
    response_t response = {.status = status, .length = (uint32_t)length};
    client->output_length = sizeof(response) + length;
    client->output = malloc(client->output_length);
    if (!client->output) { close_client(client); return; }
    memcpy(client->output, &response, sizeof(response));
    memcpy(client->output + sizeof(response), message, length);
    client->state = CLIENT_REPLY; client->used = 0; client->until = milliseconds() + 1000;
    /* Normal small responses complete here; a stalled reader stays in poll. */
    send_reply(client);
}
static void receive_client(hp_lifecycle_client_t *client)
{
    size_t budget = CONTROL_IO_BUDGET;
    while (budget && (client->state == CLIENT_HEADER || client->state == CLIENT_BODY)) {
        bool header = client->state == CLIENT_HEADER;
        size_t length = header ? sizeof(client->request) : client->request.length;
        char *buffer = header ? (char *)&client->request : client->body;
        size_t remaining = length - client->used;
        if (remaining > budget) remaining = budget;
        ssize_t n = recv(client->fd, buffer + client->used, remaining, MSG_DONTWAIT);
        if (n <= 0) {
            if (!n || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) close_client(client);
            return;
        }
        client->used += (size_t)n; budget -= (size_t)n;
        if (client->used != length) continue;
        if (!header) { client->body[length] = 0; client->state = CLIENT_READY; return; }
        request_t *request = &client->request;
        if (request->magic != WIRE_MAGIC || request->length > INPUT_LIMIT || request->force > 1 ||
            (request->command != HP_LIFECYCLE_LOAD && request->command != HP_LIFECYCLE_STOP) ||
            (request->command == HP_LIFECYCLE_STOP && request->length)) {
            hp_lifecycle_reply(client, EX_USAGE, "invalid control request\n"); return;
        }
        if (request->length > CONTROL_BUFFER_LIMIT - client->owner->buffered) {
            hp_lifecycle_reply(client, EX_TEMPFAIL, "control request memory limit reached\n"); return;
        }
        client->body = malloc((size_t)request->length + 1);
        if (!client->body) { hp_lifecycle_reply(client, EX_OSERR, "out of memory\n"); return; }
        client->reserved = request->length; client->owner->buffered += client->reserved;
        client->used = 0;
        if (!request->length) { client->body[0] = 0; client->state = CLIENT_READY; return; }
        client->state = CLIENT_BODY;
    }
}
void hp_lifecycle_control_poll(hp_lifecycle_control_t *control, struct pollfd *fds, uint64_t *next)
{
    for (size_t i = 0; i < HP_LIFECYCLE_CLIENT_LIMIT; ++i) {
        hp_lifecycle_client_t *client = &control->clients[i];
        bool receiving = client->state == CLIENT_HEADER || client->state == CLIENT_BODY;
        bool sending = client->state == CLIENT_REPLY;
        fds[i] = (struct pollfd){.fd = receiving || sending ? client->fd : -1, .events = sending ? POLLOUT : POLLIN};
        if ((receiving || sending) && client->until < *next) *next = client->until;
    }
}
void hp_lifecycle_control_process(hp_lifecycle_control_t *control, const struct pollfd *fds, uint64_t now)
{
    for (size_t i = 0; i < HP_LIFECYCLE_CLIENT_LIMIT; ++i) {
        hp_lifecycle_client_t *client = &control->clients[i];
        if (fds[i].fd < 0 || fds[i].fd != client->fd || client->state == CLIENT_FREE) continue;
        if (now >= client->until || (fds[i].revents & POLLNVAL)) { close_client(client); continue; }
        if (!fds[i].revents) continue;
        /* Read through POLLHUP: a peer may half-close after a complete frame. */
        if (client->state == CLIENT_HEADER || client->state == CLIENT_BODY) receive_client(client);
        else if (client->state == CLIENT_REPLY) send_reply(client);
    }
}
hp_lifecycle_client_t *hp_lifecycle_control_request(hp_lifecycle_control_t *control, unsigned *command,
                                                   bool *force, char **yaml, size_t *length)
{
    for (size_t i = 0; i < HP_LIFECYCLE_CLIENT_LIMIT; ++i) {
        hp_lifecycle_client_t *client = &control->clients[i];
        if (client->state != CLIENT_READY) continue;
        *command = client->request.command; *force = client->request.force;
        *yaml = client->body; client->body = NULL; *length = client->request.length;
        client->state = CLIENT_WAITING;
        return client;
    }
    return NULL;
}
static int control_socket(const char *path, bool listening)
{
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (!*path || strlen(path) >= sizeof(address.sun_path)) { errno = ENAMETOOLONG; return -1; }
    strcpy(address.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (!listening) {
        /* Client mode: never send anything to a path that is not this user's socket. */
        struct stat st;
        if (!lstat(path, &st) && (!S_ISSOCK(st.st_mode) || st.st_uid != getuid())) {
            close(fd); errno = EACCES; return -1;
        }
        if (connect(fd, (struct sockaddr *)&address, sizeof(address))) {
            int saved = errno; close(fd); errno = saved; return -1;
        }
#if defined(SO_PEERCRED)
        struct ucred cred;
        socklen_t cred_length = sizeof(cred);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_length) || cred.uid != getuid()) {
            close(fd); errno = EACCES; return -1;
        }
#endif
        return fd;
    } else {
        /* The caller holds the endpoint lock, including stale-socket cleanup. */
        struct stat old;
        if (!lstat(path, &old)) {
            int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (probe < 0) { close(fd); return -1; }
            int result = connect(probe, (struct sockaddr *)&address, sizeof(address)), reason = errno;
            close(probe);
            if (!result || reason != ECONNREFUSED || !S_ISSOCK(old.st_mode) || old.st_uid != getuid()) {
                close(fd); errno = EADDRINUSE; return -1;
            }
            struct stat current;
            if (lstat(path, &current) || current.st_ino != old.st_ino || current.st_dev != old.st_dev || unlink(path)) {
                close(fd); errno = EADDRINUSE; return -1;
            }
        }
        mode_t previous = umask(0077);
        int result = bind(fd, (struct sockaddr *)&address, sizeof(address));
        umask(previous);
        if (!result) {
            if (!chmod(path, 0600) && !listen(fd, 16)) {
                int flags = fcntl(fd, F_GETFL);
                if (flags >= 0 && !fcntl(fd, F_SETFL, flags | O_NONBLOCK)) return fd;
            }
            int saved = errno; unlink(path); errno = saved;
        }
    }
    int saved = errno; close(fd); errno = saved; return -1;
}
static void default_control_address(char *buffer, size_t size)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime && runtime[0] == '/' &&
        snprintf(buffer, size, "%s/h2o-php.sock", runtime) < (int)size) return;
    snprintf(buffer, size, "/tmp/h2o-php-%lu.sock", (unsigned long)getuid());
}
static void usage(const char *command)
{
    if (!strcmp(command, "run"))
        puts("Usage: h2o-php run --config FILE [--address unix//PATH] [--watch[=GLOB]]\nRuns in the foreground with graceful reload support.\n--watch reloads FILE after edits; repeat --watch=GLOB to watch additional YAML/includes.");
    else if (!strcmp(command, "reload"))
        puts("Usage: h2o-php reload --config FILE [--address unix//PATH] [--force]\nApplies the supplied H2O YAML; waits for success or failure.");
    else
        puts("Usage: h2o-php stop [--address unix//PATH]\nGracefully stops the running instance.");
}
int hp_lifecycle_command(int argc, char **argv)
{
    if (argc < 2 || (strcmp(argv[1], "run") && strcmp(argv[1], "reload") && strcmp(argv[1], "stop"))) return -1;
    const char *command = argv[1], *config_path = NULL, *address = NULL;
    bool run = !strcmp(command, "run"), stop = !strcmp(command, "stop"), force = false;
    const char *watch_patterns[32]; size_t num_watch_patterns = 0;
    bool watch_enabled = false;
    for (int i = 2; i < argc; ++i) {
        const char *argument = argv[i], *value = strchr(argument, '=');
        size_t key_length = value ? (size_t)(value - argument) : strlen(argument);
        if (value) ++value;
        if (!strcmp(argument, "--help") || !strcmp(argument, "-h")) { usage(command); return 0; }
        if (key_length == 7 && !strncmp(argument, "--watch", 7)) {
            if (!run || (value && !*value) || num_watch_patterns == 32) goto Usage;
            watch_enabled = true;
            if (value) watch_patterns[num_watch_patterns++] = value;
            continue;
        }
        if (!strcmp(argument, "--force") || !strcmp(argument, "-f")) {
            if (run || stop) goto Usage;
            force = true; continue;
        }
        const char **slot = NULL;
        if ((key_length == 8 && !strncmp(argument, "--config", 8)) || !strcmp(argument, "-c")) slot = &config_path;
        else if (key_length == 9 && !strncmp(argument, "--address", 9)) slot = &address;
        else goto Usage;
        if (!value && ++i < argc) value = argv[i];
        if (!value || !*value || *slot) goto Usage;
        *slot = value;
    }
    if (!stop && !config_path) goto Usage;
    if (watch_enabled && !strcmp(config_path, "-")) {
        fputs("h2o-php run: --watch requires a configuration file, not stdin\n", stderr); return EX_USAGE;
    }
    char default_address[108];
    default_control_address(default_address, sizeof(default_address));
    if (!address) address = default_address;
    if (!strncmp(address, "unix//", 6)) address += 5;
    if (address[0] != '/') {
        fputs("h2o-php: --address must be an absolute Unix socket path (unix//PATH)\n", stderr); return EX_USAGE;
    }
    hp_config_watch_t *watch = watch_enabled ? hp_config_watch_create(config_path, watch_patterns, num_watch_patterns) : NULL;
    if (watch_enabled && !watch) { perror("h2o-php: cannot watch configuration"); return EX_OSERR; }
    size_t length = 0;
    char *yaml = stop ? NULL : watch ? hp_config_watch_read(watch, &length) : read_file(config_path, &length);
    if (!stop && !yaml) { perror(config_path); hp_config_watch_destroy(watch); return EX_NOINPUT; }
    int lock_fd = -1;
    if (run) {
        char *lock_path = NULL;
        if (asprintf(&lock_path, "%s.lock", address) < 0) { hp_config_watch_destroy(watch); free(yaml); return EX_OSERR; }
        lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600); free(lock_path);
        struct stat st;
        if (lock_fd < 0 || fstat(lock_fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
            flock(lock_fd, LOCK_EX | LOCK_NB)) {
            fputs("h2o-php: instance address is locked or unavailable\n", stderr);
            if (lock_fd >= 0) close(lock_fd);
            hp_config_watch_destroy(watch); free(yaml); return EX_UNAVAILABLE;
        }
    }
    int fd = control_socket(address, run);
    if (fd < 0) { fprintf(stderr, "h2o-php: control socket %s: %s\n", address, strerror(errno)); if (lock_fd >= 0) close(lock_fd); hp_config_watch_destroy(watch); free(yaml); return EX_UNAVAILABLE; }
    int result;
    if (run) {
        struct sigaction action = {.sa_handler = interrupt_run}, previous_int, previous_term;
        sigemptyset(&action.sa_mask); interrupted = 0;
        sigaction(SIGINT, &action, &previous_int); sigaction(SIGTERM, &action, &previous_term);
        char *error;
        hp_lifecycle_config_t *config = hp_lifecycle_load(argv[0], yaml, length, &error, run_cancelled);
        if (!config) { fprintf(stderr, "h2o-php: %s\n", error ? error : "out of memory"); free(error); result = interrupted ? 0 : EX_CONFIG; }
        else { result = interrupted ? 0 : hp_native_lifecycle_run(config, fd, watch); hp_lifecycle_release(config); }
        close(fd); unlink(address);
        close(lock_fd);
        sigaction(SIGINT, &previous_int, NULL); sigaction(SIGTERM, &previous_term, NULL);
    } else {
        request_t request = {.magic = WIRE_MAGIC, .command = stop ? HP_LIFECYCLE_STOP : HP_LIFECYCLE_LOAD,
                             .force = force, .length = (uint32_t)length};
        response_t response;
        /* The server owns startup/drain deadlines. A client must not report a
         * timeout while the requested config can still become active later. */
        uint64_t until = UINT64_MAX;
        if (transfer(fd, &request, sizeof(request), true, until) || transfer(fd, yaml, length, true, until) ||
            transfer(fd, &response, sizeof(response), false, until) || response.length > INPUT_LIMIT) {
            perror("h2o-php control request"); result = EX_IOERR;
        } else {
            char *message = malloc((size_t)response.length + 1);
            if (!message || transfer(fd, message, response.length, false, until)) { free(message); result = EX_IOERR; }
            else { message[response.length] = 0; fputs(message, response.status ? stderr : stdout); free(message); result = response.status; }
        }
        close(fd);
    }
    hp_config_watch_destroy(watch); free(yaml); return result;
Usage:
    usage(command); return EX_USAGE;
}
