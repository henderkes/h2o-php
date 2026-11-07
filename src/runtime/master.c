#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "master.h"
#include "lifecycle.h"
#include "h2o.h"
#include "h2o/serverutil.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#define MASTER_READY_ENV "H2O_PHP_READY_FD"
enum log_state { LOG_OPEN, LOG_RETRY, LOG_CONNECTING, LOG_READY };
typedef struct listener {
    int fd;
    char *label, *path;
    dev_t device;
    ino_t inode;
    bool borrowed;
} listener_t;
typedef struct child {
    pid_t pid;
    uint64_t generation, started, deadline;
    int ready_fd;
    hp_lifecycle_config_t *config;
} child_t;
typedef struct control_request {
    struct control_request *next;
    hp_lifecycle_client_t *client;
    bool force;
    char *yaml;
    size_t length;
} control_request_t;
typedef struct master {
    H2O_VECTOR(listener_t) listeners;
    char *socket_environment, *pid_path, *status_path, *log_path;
    char **worker_argv;
    int pid_fd, wake[2], daemon_ready;
    bool daemonize, stopping, reload, pid_owned, status_owned;
    uint64_t generation, startup_ms, shutdown_ms, retry_at, shutdown_at;
    unsigned failures;
    child_t current, candidate, retiring;
    sigset_t signals, original_mask;
    struct sigaction old_actions[5];
    struct master *reuse, *pending;
    hp_lifecycle_config_t *config;
    int control_fd, exit_status;
    hp_lifecycle_control_t *control;
    hp_lifecycle_client_t *reply_client, *validation_client;
    hp_lifecycle_validation_t *validation;
    bool validation_force;
    int next_log_fd, original_output[2];
    enum log_state log_state;
    bool log_socket;
    uint64_t log_retry_at, preparation_deadline;
    control_request_t *requests, **request_tail;
    unsigned request_count;
} master_t;
static volatile sig_atomic_t signal_pipe = -1, signal_flags;
static int worker_ready_fd = -1;
static pid_t worker_ready_owner;
enum { SIGNAL_CHILD = 1, SIGNAL_RELOAD = 2, SIGNAL_STOP = 4 };
static const int managed_signals[] = {SIGCHLD, SIGHUP, SIGTERM, SIGINT, SIGPIPE};

static uint64_t now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static uint64_t deadline(uint64_t now, uint64_t delay)
{
    return UINT64_MAX - now < delay ? UINT64_MAX : now + delay;
}
static int set_fd_flag(int fd, int flag, bool enabled)
{
    int old = fcntl(fd, F_GETFD);
    return old < 0 ? -1 : fcntl(fd, F_SETFD, enabled ? old | flag : old & ~flag);
}
#if !defined(PIPE_BUF)
#define MASTER_DIAGNOSTIC_SIZE 512
#elif PIPE_BUF < 4096
#define MASTER_DIAGNOSTIC_SIZE PIPE_BUF
#else
#define MASTER_DIAGNOSTIC_SIZE 4096
#endif
#if defined(__linux__)
static void diagnostic_pipe_write(int fd, const char *record, size_t length)
{
    sigset_t blocked, previous, pending, pipe_signal;
    if (sigfillset(&blocked) || sigdelset(&blocked, SIGKILL) || sigdelset(&blocked, SIGSTOP) ||
        sigemptyset(&pipe_signal) || sigaddset(&pipe_signal, SIGPIPE) ||
        sigprocmask(SIG_SETMASK, &blocked, &previous)) return;
    /* This also protects initialization and the single-threaded exec-failure
     * child, where the master's normal SIGPIPE-ignore handler is not installed. */
    if (!sigpending(&pending)) {
        int had_sigpipe = sigismember(&pending, SIGPIPE);
        if (had_sigpipe >= 0) {
            ssize_t written = write(fd, record, length);
            if (written < 0 && errno == EPIPE && !had_sigpipe) {
                const struct timespec zero = {0};
                /* Other catchable signals stay blocked: one zero-time wait
                 * consumes only a newly generated SIGPIPE, without retrying. */
                (void)sigtimedwait(&pipe_signal, NULL, &zero);
            }
        }
    }
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
}
#endif
/* Supervisor diagnostics must not hold up control, signals or generation
 * deadlines. Healthy sinks retain ordinary records; pressure/errors may drop
 * or truncate them. Ordinary file/device storage itself is not made bounded. */
static void master_diagnostic(const char *format, ...)
{
    int saved_errno = errno;
    char record[MASTER_DIAGNOSTIC_SIZE];
    va_list args; va_start(args, format);
    int formatted = vsnprintf(record, sizeof(record), format, args);
    va_end(args);
    if (formatted <= 0) goto Done;
    size_t length = (size_t)formatted;
    if (length >= sizeof(record)) {
        static const char suffix[] = "... [truncated]\n";
        length = sizeof(record) - 1;
        memcpy(record + length - (sizeof(suffix) - 1), suffix, sizeof(suffix) - 1);
    }
    struct stat original;
    if (fstat(STDERR_FILENO, &original)) goto Done;
    if (S_ISSOCK(original.st_mode)) {
#if defined(MSG_DONTWAIT) && defined(MSG_NOSIGNAL)
        (void)send(STDERR_FILENO, record, length, MSG_DONTWAIT | MSG_NOSIGNAL);
#endif
    } else if (S_ISFIFO(original.st_mode)) {
#if defined(__linux__)
        /* dup+F_SETFL would change the worker's shared open file description.
         * Reopening the exact FIFO makes a private nonblocking description. */
        int fd = open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            struct stat reopened;
            int flags = fcntl(fd, F_GETFL);
            if (flags >= 0 && (flags & O_NONBLOCK) && !fstat(fd, &reopened) && S_ISFIFO(reopened.st_mode) &&
                reopened.st_dev == original.st_dev && reopened.st_ino == original.st_ino)
                diagnostic_pipe_write(fd, record, length);
            close(fd);
        }
#endif
        /* An unavailable /proc path or unsupported platform must never fall
         * back to a blocking write on the inherited FIFO. */
    } else {
        (void)write(STDERR_FILENO, record, length);
    }
Done:
    errno = saved_errno;
}
static int open_master_log(const char *path)
{
    if (path[0] != '|') return h2o_access_log_open_log(path);
    int fds[2] = {-1, -1};
    const char *operation = "pipe failed";
    if (pipe(fds)) goto Error;
    operation = "failed to set FD_CLOEXEC on pipefds[1]";
    /* Match upstream's writer-only CLOEXEC. If stdin was closed, read fd0
     * must survive h2o_spawnp's source==destination mapping through exec. */
    if (set_fd_flag(fds[1], FD_CLOEXEC, true)) goto Error;
    char *argv[] = {"/bin/sh", "-c", (char *)path + 1, NULL};
    int mapped_fds[] = {fds[0], STDIN_FILENO, -1};
    operation = NULL;
    if (h2o_spawnp(argv[0], argv, mapped_fds, 0) < 0) goto Error;
    close(fds[0]);
    return fds[1];
Error:
    {
        int saved_errno = errno;
        if (fds[0] >= 0) close(fds[0]);
        if (fds[1] >= 0) close(fds[1]);
        if (operation) master_diagnostic("%s: %s\n", operation, strerror(saved_errno));
        else master_diagnostic("failed to open logger: %s:%s\n", path + 1, strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }
}
static int make_pipe(int fds[2], bool nonblocking)
{
    if (pipe(fds)) return -1;
    for (unsigned i = 0; i < 2; ++i) {
        if (set_fd_flag(fds[i], FD_CLOEXEC, true)) goto Error;
        if (nonblocking) {
            int flags = fcntl(fds[i], F_GETFL);
            if (flags < 0 || fcntl(fds[i], F_SETFL, flags | O_NONBLOCK)) goto Error;
        }
    }
    return 0;
Error:
    { int saved = errno; close(fds[0]); close(fds[1]); errno = saved; return -1; }
}
static void receive_signal(int signo)
{
    int saved = errno;
    signal_flags |= signo == SIGCHLD ? SIGNAL_CHILD : signo == SIGHUP ? SIGNAL_RELOAD : SIGNAL_STOP;
    if (signal_pipe >= 0) { char byte = 1; ssize_t ignored = write(signal_pipe, &byte, 1); (void)ignored; }
    errno = saved;
}
static int install_signals(master_t *m)
{
    if (make_pipe(m->wake, true)) return -1;
    sigemptyset(&m->signals);
    for (size_t i = 0; i < 4; ++i) sigaddset(&m->signals, managed_signals[i]);
    if (sigprocmask(SIG_SETMASK, NULL, &m->original_mask)) return -1;
    struct sigaction action = {.sa_handler = receive_signal, .sa_flags = SA_RESTART};
    action.sa_mask = m->signals; signal_flags = 0; signal_pipe = m->wake[1];
    for (size_t i = 0; i < 5; ++i) {
        if (i == 4) action.sa_handler = SIG_IGN;
        if (sigaction(managed_signals[i], &action, &m->old_actions[i])) {
            int saved = errno; signal_pipe = -1;
            while (i) { --i; sigaction(managed_signals[i], &m->old_actions[i], NULL); }
            errno = saved; return -1;
        }
    }
    return 0;
}
static unsigned consume_signals(master_t *m)
{
    char bytes[128]; while (read(m->wake[0], bytes, sizeof(bytes)) > 0) {}
    sigset_t previous; sigprocmask(SIG_BLOCK, &m->signals, &previous);
    unsigned flags = (unsigned)signal_flags; signal_flags = 0;
    sigprocmask(SIG_SETMASK, &previous, NULL); return flags;
}
void hp_native_master_worker_prepare(void)
{
    const char *value = getenv(MASTER_READY_ENV);
    if (!value) return;
    char *end; errno = 0; long fd = strtol(value, &end, 10);
    struct stat st;
    if (!errno && *value && !*end && fd >= 3 && fd <= INT_MAX && !fstat((int)fd, &st) && S_ISFIFO(st.st_mode)) {
        worker_ready_fd = (int)fd; worker_ready_owner = getpid();
        set_fd_flag(worker_ready_fd, FD_CLOEXEC, true);
        H2O_HOST_FDS_BEGIN();
        H2O_HOST_FD_REGISTER(worker_ready_fd);
        H2O_HOST_FDS_END();
    }
    unsetenv(MASTER_READY_ENV);
}
void hp_native_master_notify_ready(void)
{
    if (worker_ready_fd < 0 || worker_ready_owner != getpid()) return;
    char ready = 'R'; ssize_t n;
    do { n = write(worker_ready_fd, &ready, 1); } while (n < 0 && errno == EINTR);
    H2O_HOST_FD_CLOSE(worker_ready_fd); worker_ready_fd = -1;
}
static void close_listener(listener_t *listener)
{
    if (listener->fd >= 0) close(listener->fd);
    if (listener->path && !listener->borrowed) {
        struct stat st;
        if (!lstat(listener->path, &st) && st.st_dev == listener->device && st.st_ino == listener->inode) unlink(listener->path);
    }
    free(listener->label); free(listener->path);
}
static int append_listener(master_t *m, int fd, const char *label, const char *path)
{
    listener_t entry = {.fd = fd, .label = strdup(label), .path = path ? strdup(path) : NULL};
    struct stat st;
    if (!entry.label || (path && !entry.path) || set_fd_flag(fd, FD_CLOEXEC, true)) {
        free(entry.label); free(entry.path); close(fd); return -1;
    }
    if (path && !lstat(path, &st)) { entry.device = st.st_dev; entry.inode = st.st_ino; }
    h2o_vector_reserve(NULL, &m->listeners, m->listeners.size + 1);
    m->listeners.entries[m->listeners.size++] = entry; return 0;
}
static int bind_unix(master_t *m, const char *path)
{
    if (m->reuse) for (size_t i = 0; i < m->reuse->listeners.size; ++i) {
        listener_t *old = &m->reuse->listeners.entries[i];
        if (!strcmp(old->label, path)) {
            int fd = fcntl(old->fd, F_DUPFD_CLOEXEC, 3);
            if (fd < 0 || append_listener(m, fd, path, path)) return -1;
            m->listeners.entries[m->listeners.size - 1].borrowed = true;
            return 0;
        }
    }
    struct sockaddr_un address = {.sun_family = AF_UNIX}; struct stat st;
    if (!*path || strlen(path) >= sizeof(address.sun_path) || strpbrk(path, ";=")) { errno = EINVAL; return -1; }
    strcpy(address.sun_path, path);
    if (!lstat(path, &st)) {
        if (!S_ISSOCK(st.st_mode)) { errno = EEXIST; return -1; }
        int probe = socket(AF_UNIX, SOCK_STREAM, 0); if (probe < 0) return -1;
        int flags = fcntl(probe, F_GETFL);
        if (flags < 0 || fcntl(probe, F_SETFL, flags | O_NONBLOCK)) { int saved = errno; close(probe); errno = saved; return -1; }
        int connected = connect(probe, (struct sockaddr *)&address, sizeof(address));
        int reason = errno; close(probe);
        if (!connected || (reason != ECONNREFUSED && reason != ENOENT)) { errno = EADDRINUSE; return -1; }
        if (reason == ECONNREFUSED) {
            struct stat current;
            if (lstat(path, &current) || current.st_dev != st.st_dev || current.st_ino != st.st_ino) { errno = EADDRINUSE; return -1; }
            if (unlink(path)) return -1;
        }
    } else if (errno != ENOENT) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); if (fd < 0) return -1;
    mode_t previous_umask = umask(0077);
    int bound = bind(fd, (struct sockaddr *)&address, sizeof(address));
    umask(previous_umask);
    if (bound || listen(fd, SOMAXCONN)) {
        int saved = errno; close(fd); unlink(path); errno = saved; return -1;
    }

    return append_listener(m, fd, path, path);
}
static int parse_acl_field(const char **cursor, long *value, int base)
{
    const char *start = *cursor;
    char *end;
    if (!strncmp(start, "-1", 2)) { *value = -1; *cursor = start + 2; return 0; }
    errno = 0;
    *value = strtol(start, &end, base);
    if (end == start || *value < 0 || errno == ERANGE) return -1;
    *cursor = end;
    return 0;
}
static int apply_path_acl(listener_t *listener, const char *specification)
{
    long mode = -1, uid = -1, gid = -1;
    if (parse_acl_field(&specification, &mode, 8) || *specification++ != ',' ||
        parse_acl_field(&specification, &uid, 10) || *specification++ != ',' ||
        parse_acl_field(&specification, &gid, 10) || *specification) { errno = EINVAL; return -1; }
    if (mode != -1 && chmod(listener->path, (mode_t)mode)) return -1;
    if (uid != -1 || gid != -1) {
        if (chown(listener->path, uid != -1 ? (uid_t)uid : (uid_t)-1, gid != -1 ? (gid_t)gid : (gid_t)-1)) return -1;
    }
    return 0;
}
static int bind_inet(master_t *m, const char *specification)
{
    if (m->reuse) for (size_t i = 0; i < m->reuse->listeners.size; ++i) {
        listener_t *old = &m->reuse->listeners.entries[i];
        if (!strcmp(old->label, specification)) {
            int fd = fcntl(old->fd, F_DUPFD_CLOEXEC, 3);
            return fd < 0 ? -1 : append_listener(m, fd, specification, NULL);
        }
    }
    char *copy = strdup(specification), *host, *service;
    if (!copy) return -1;
    if (copy[0] == '[') {
        char *end = strchr(copy, ']');
        if (!end || end[1] != ':') goto Invalid;
        *end = 0; host = copy + 1; service = end + 2;
    } else {
        service = strrchr(copy, ':'); if (!service) goto Invalid;
        *service++ = 0; host = copy;
    }
    int type = SOCK_STREAM;
    if (*service == 'u') { type = SOCK_DGRAM; ++service; }
    if (!*host || !*service) goto Invalid;
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = type, .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV};
    struct addrinfo *address = NULL;
    int result = getaddrinfo(host, service, &hints, &address);
    if (result) { free(copy); errno = EINVAL; return -1; }
    int fd = socket(address->ai_family, type, address->ai_protocol);
    if (fd < 0) { freeaddrinfo(address); free(copy); return -1; }
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) ||
        (address->ai_family == AF_INET6 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &enabled, sizeof(enabled))) ||
        bind(fd, address->ai_addr, address->ai_addrlen) || (type == SOCK_STREAM && listen(fd, SOMAXCONN))) {
        int saved = errno; close(fd); freeaddrinfo(address); free(copy); errno = saved; return -1;
    }
    freeaddrinfo(address); free(copy); return append_listener(m, fd, specification, NULL);
Invalid:
    free(copy); errno = EINVAL; return -1;
}
static int parse_arguments(master_t *m, char *const arguments[])
{
    size_t i;
    for (i = 1; arguments[i] && strcmp(arguments[i], "--"); ++i) {
        const char *arg = arguments[i];
        if (!strncmp(arg, "--port=", 7)) { if (bind_inet(m, arg + 7)) return -1; }
        else if (!strncmp(arg, "--path=", 7)) { if (bind_unix(m, arg + 7)) return -1; }
        else if (!strncmp(arg, "--path-acl=", 11)) {
            listener_t *listener = m->listeners.size ? &m->listeners.entries[m->listeners.size - 1] : NULL;
            if (!listener || !listener->path) { errno = EINVAL; return -1; }
            if (apply_path_acl(listener, arg + 11)) return -1;
        }
        else if (!strncmp(arg, "--pid-file=", 11)) m->pid_path = strdup(arg + 11);
        else if (!strncmp(arg, "--status-file=", 14)) m->status_path = strdup(arg + 14);
        else if (!strncmp(arg, "--log-file=", 11)) m->log_path = strdup(arg + 11);
        else if (!strcmp(arg, "--daemonize")) m->daemonize = true;
        else { errno = EINVAL; return -1; }
    }
    if (!arguments[i] || !arguments[i + 1] || !m->listeners.size) { errno = EINVAL; return -1; }
    ++i;
    size_t count = 0; while (arguments[i + count]) ++count;
    m->worker_argv = calloc(count + 3, sizeof(char *));
    if (!m->worker_argv) return -1;
    m->worker_argv[0] = arguments[i]; m->worker_argv[1] = "-m"; m->worker_argv[2] = "worker";
    for (size_t n = 1; n < count; ++n) m->worker_argv[n + 2] = arguments[i + n];
    size_t size = 0; FILE *out = open_memstream(&m->socket_environment, &size);
    if (!out) return -1;
    for (size_t n = 0; n < m->listeners.size; ++n) {
        if (n) fputc(';', out);
        fprintf(out, "%s=%d", m->listeners.entries[n].label, m->listeners.entries[n].fd);
    }
    if (fclose(out)) return -1;
    if (!m->status_path && m->pid_path && asprintf(&m->status_path, "%s.status", m->pid_path) < 0) return -1;
    return 0;
}
static int open_pid_file(master_t *m)
{
    if (!m->pid_path) return 0;
    m->pid_fd = open(m->pid_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (m->pid_fd < 0 || flock(m->pid_fd, LOCK_EX | LOCK_NB)) return -1;
    m->pid_owned = true;
    if (ftruncate(m->pid_fd, 0) || dprintf(m->pid_fd, "%ld\n", (long)getpid()) < 0) return -1;
    return 0;
}
static void update_status(master_t *m)
{
    if (!m->status_path) return;
    char *temporary = NULL; if (asprintf(&temporary, "%s.XXXXXX", m->status_path) < 0) return;
    int fd = mkstemp(temporary);
    if (fd >= 0) {
        child_t *children[] = {&m->retiring, &m->current, &m->candidate};
        bool ok = true;
        for (size_t i = 0; i < 3; ++i)
            if (children[i]->pid && dprintf(fd, "%" PRIu64 ":%ld\n", children[i]->generation, (long)children[i]->pid) < 0) ok = false;
        if (close(fd)) ok = false;
        if (ok && rename(temporary, m->status_path)) ok = false;
        if (ok) m->status_owned = true;
        if (!ok) master_diagnostic("h2o.master: failed to update status %s: %s\n", m->status_path, strerror(errno));
        unlink(temporary);
    }
    free(temporary);
}
static int redirect_logs(master_t *m)
{
    if (!m->log_path) return 0;
    /* Master diagnostics bypass stdio; a global flush could instead wait on
     * the previous sink before the owning loop can enforce its deadlines. */
    int fd = open_master_log(m->log_path); if (fd < 0) return -1;
    int status = dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0 ? -1 : 0;
    if (fd > STDERR_FILENO) close(fd);
    return status;
}
static int apply_generation_logs(master_t *m, master_t *target)
{
    int output = target->log_path ? target->next_log_fd : m->original_output[0];
    int errors = target->log_path ? target->next_log_fd : m->original_output[1];
    return dup2(output, STDOUT_FILENO) < 0 || dup2(errors, STDERR_FILENO) < 0 ? -1 : 0;
}
/* Returns 1 in a successfully detached parent, 0 in the serving master. */
static int daemonize(master_t *m)
{
    if (!m->daemonize) return 0;
    int ready[2]; if (make_pipe(ready, false)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(ready[0]); close(ready[1]); return -1; }
    if (!pid) {
        close(ready[0]); m->daemon_ready = ready[1];
        if (setsid() < 0) _exit(EX_OSERR);
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0) _exit(EX_OSERR);
        if (nullfd > STDERR_FILENO) close(nullfd);
        return 0;
    }
    close(ready[1]); struct pollfd pollfd = {.fd = ready[0], .events = POLLIN};
    uint64_t until = deadline(now_ms(), m->startup_ms);
    char byte = 0;
    for (;;) {
        uint64_t now = now_ms(); if (now >= until) break;
        uint64_t wait = until - now; int timeout = wait > INT_MAX ? INT_MAX : (int)wait;
        int n = poll(&pollfd, 1, timeout);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (read(ready[0], &byte, 1) == 1 && byte == 'R') { close(ready[0]); return 1; }
        break;
    }
    close(ready[0]); kill(pid, SIGTERM);
    uint64_t stop = deadline(now_ms(), m->shutdown_ms);
    while (waitpid(pid, NULL, WNOHANG) == 0 && now_ms() < stop) {
        struct timespec pause = {.tv_nsec = 10000000}; nanosleep(&pause, NULL);
    }
    if (waitpid(pid, NULL, WNOHANG) == 0) { kill(pid, SIGKILL); while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {} }
    errno = ETIMEDOUT; return -1;
}
static int start_child(master_t *m, uint64_t now)
{
    master_t *target = m->pending ? m->pending : m;
    int ready[2]; if (make_pipe(ready, true)) return -1;
    uint64_t generation = ++m->generation;
    char generation_text[32], descriptor[32];
    snprintf(generation_text, sizeof(generation_text), "%" PRIu64, generation);
    snprintf(descriptor, sizeof(descriptor), "%d", ready[1]);
    pid_t owner = getpid();
    pid_t pid = fork();
    if (pid < 0) { close(ready[0]); close(ready[1]); return -1; }
    if (!pid) {
        close(ready[0]); close(m->wake[0]); close(m->wake[1]);
        for (size_t i = 0; i < 5; ++i) {
            struct sigaction action = {.sa_handler = SIG_DFL}; sigemptyset(&action.sa_mask);
            sigaction(managed_signals[i], &action, NULL);
        }
        sigprocmask(SIG_SETMASK, &m->original_mask, NULL);
#if defined(__linux__)
        if (m->config && (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != owner)) _exit(EX_OSERR);
#else
        (void)owner;
#endif
        if (m->pending && apply_generation_logs(m, target)) _exit(EX_OSERR);
        for (size_t i = 0; i < target->listeners.size; ++i)
            if (set_fd_flag(target->listeners.entries[i].fd, FD_CLOEXEC, false)) _exit(EX_OSERR);
        if (set_fd_flag(ready[1], FD_CLOEXEC, false) ||
            setenv("SERVER_STARTER_PORT", target->socket_environment, 1) ||
            setenv("SERVER_STARTER_GENERATION", generation_text, 1) ||
            setenv("H2O_VIA_MASTER", "1", 1) || setenv(MASTER_READY_ENV, descriptor, 1)) _exit(EX_OSERR);
        unsetenv("H2O_PHP_CLI_CHILD");
        execvp(target->worker_argv[0], target->worker_argv);
        master_diagnostic("h2o.master: exec %s failed: %s\n", target->worker_argv[0], strerror(errno)); _exit(EX_CONFIG);
    }
    close(ready[1]);
    m->candidate = (child_t){.pid = pid, .generation = generation, .started = now,
        .deadline = m->pending ? target->preparation_deadline : deadline(now, target->startup_ms),
        .ready_fd = ready[0], .config = target->config};
    hp_lifecycle_retain(m->candidate.config);
    m->retry_at = 0;
    master_diagnostic("h2o.master: starting generation %" PRIu64 " pid %ld\n", generation, (long)pid);
    update_status(m); return 0;
}
static void retry_later(master_t *m, uint64_t now)
{
    if (m->failures < UINT_MAX) ++m->failures;
    unsigned shift = m->failures > 6 ? 6 : m->failures - 1;
    uint64_t delay = 100ULL << shift; if (delay > 5000) delay = 5000;
    m->retry_at = deadline(now, delay);
    master_diagnostic("h2o.master: retry in %" PRIu64 " ms after %u consecutive failures\n", delay, m->failures);
}
static void dispose(master_t *m, bool remove_paths);
static void begin_shutdown(master_t *m, uint64_t now);
static void reject_reload(master_t *m, const char *message)
{
    hp_lifecycle_reply(m->reply_client, EX_CONFIG, message); m->reply_client = NULL;
    if (m->pending) {
        dispose(m->pending, true); hp_lifecycle_release(m->pending->config);
        free(m->pending); m->pending = NULL;
    }
    m->reload = false;
}
static bool reap_child(master_t *m, child_t *child, const char *kind, uint64_t now)
{
    if (!child->pid) return false;
    int status; pid_t pid = waitpid(child->pid, &status, WNOHANG);
    if (!pid || (pid < 0 && errno == EINTR)) return false;
    if (pid < 0 && errno != ECHILD) return false;
    if (pid < 0) status = 0;
    master_diagnostic("h2o.master: %s generation %" PRIu64 " pid %ld exited status %d\n", kind, child->generation, (long)child->pid, status);
    bool was_candidate = child == &m->candidate;
    bool unexpected = child == &m->current || was_candidate;
    if (child == &m->current && now - child->started >= 30000) m->failures = 0;
    if (child->ready_fd >= 0) close(child->ready_fd);
    hp_lifecycle_release(child->config);
    *child = (child_t){.ready_fd = -1};
    if (unexpected && !m->stopping) {
        if (m->config) {
            if (was_candidate) reject_reload(m, "replacement exited before readiness; active configuration kept (see server log)\n");
            if (!m->current.pid) { m->exit_status = EX_SOFTWARE; begin_shutdown(m, now); }
            update_status(m); return true;
        }
        if (was_candidate && m->current.pid) m->reload = true;
        retry_later(m, now);
    }
    update_status(m); return true;
}
static void begin_shutdown(master_t *m, uint64_t now)
{
    if (m->config) reject_reload(m, "server is shutting down\n");
    if (m->validation) {
        hp_lifecycle_validation_cancel(m->validation, "configuration validation interrupted by shutdown");
        hp_lifecycle_reply(m->validation_client, EX_TEMPFAIL, "server is shutting down\n");
        m->validation_client = NULL;
    }
    while (m->requests) {
        control_request_t *request = m->requests; m->requests = request->next;
        hp_lifecycle_reply(request->client, EX_TEMPFAIL, "server is shutting down\n");
        free(request->yaml); free(request);
    }
    m->request_tail = &m->requests; m->request_count = 0;
    m->stopping = true; m->shutdown_at = deadline(now, m->shutdown_ms);
    child_t *children[] = {&m->current, &m->candidate, &m->retiring};
    for (size_t i = 0; i < 3; ++i) if (children[i]->pid) kill(children[i]->pid, SIGTERM);
    master_diagnostic("h2o.master: draining worker generations\n");
}
static void start_validation(master_t *m, hp_lifecycle_client_t *client, bool force, char *yaml, size_t length)
{
    char *error;
    m->validation = hp_lifecycle_validation_begin(m->worker_argv[0], yaml, length, &error);
    free(yaml);
    if (!m->validation) {
        hp_lifecycle_reply(client, EX_CONFIG, error ? error : "cannot validate configuration\n"); free(error); return;
    }
    m->validation_client = client; m->validation_force = force;
}
static int finish_log_open(master_t *target)
{
    int flags = fcntl(target->next_log_fd, F_GETFL);
    if (flags < 0 || fcntl(target->next_log_fd, F_SETFL, flags & ~O_NONBLOCK) ||
        set_fd_flag(target->next_log_fd, FD_CLOEXEC, true)) return -1;
    target->log_state = LOG_READY;
    return 1;
}
static int open_generation_log(master_t *target, short events, uint64_t now)
{
    if (target->log_state == LOG_READY) return 1;
    if (!target->log_path) { target->log_state = LOG_READY; return 1; }
    if (target->log_state == LOG_CONNECTING) {
        if (!events) return 0;
        int error = 0; socklen_t length = sizeof(error);
        if (getsockopt(target->next_log_fd, SOL_SOCKET, SO_ERROR, &error, &length)) return -1;
        if (error) { errno = error; return -1; }
        struct sockaddr_un peer; length = sizeof(peer);
        if (getpeername(target->next_log_fd, (struct sockaddr *)&peer, &length)) return -1;
        return finish_log_open(target);
    }
    if (target->log_state == LOG_RETRY && now < target->log_retry_at) return 0;
    bool first_attempt = target->log_state == LOG_OPEN;
    /* Keep the native pipe logger's spawn/ownership behavior. Plain paths use
     * nonblocking open/connect so FIFO readers and Unix backlog space can
     * arrive while the supervisor continues servicing signals and controls. */
    if (target->log_path[0] == '|') {
        target->next_log_fd = open_master_log(target->log_path);
        return target->next_log_fd < 0 ? -1 : finish_log_open(target);
    }
    struct stat st;
    target->log_socket = stat(target->log_path, &st) == 0 && S_ISSOCK(st.st_mode);
    if (target->log_socket) {
        struct sockaddr_un address = {.sun_family = AF_UNIX};
        if (strlen(target->log_path) >= sizeof(address.sun_path)) { errno = ENAMETOOLONG; return -1; }
        strcpy(address.sun_path, target->log_path);
        target->next_log_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (target->next_log_fd < 0) return -1;
        if (!connect(target->next_log_fd, (struct sockaddr *)&address, sizeof(address))) return finish_log_open(target);
        int error = errno;
        if (error == EINPROGRESS || error == EALREADY) { target->log_state = LOG_CONNECTING; return 0; }
        close(target->next_log_fd); target->next_log_fd = -1;
        if (error != EAGAIN && error != EWOULDBLOCK) { errno = error; return -1; }
    } else {
        target->next_log_fd = open(target->log_path, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC | O_NONBLOCK, 0644);
        if (target->next_log_fd >= 0) return finish_log_open(target);
        /* ENXIO from a FIFO means there is no reader yet. Other failed opens
         * retain their native diagnostic and reject the replacement. */
        if (errno != ENXIO || stat(target->log_path, &st) || !S_ISFIFO(st.st_mode)) return -1;
    }
    if (first_attempt) master_diagnostic("h2o.master: waiting for replacement log %s\n", target->log_path);
    target->log_state = LOG_RETRY; target->log_retry_at = deadline(now, 10);
    return 0;
}
static void advance_generation_log(master_t *m, short events, uint64_t now)
{
    master_t *target = m->pending;
    if (!target || m->candidate.pid || m->stopping || target->log_state == LOG_READY) return;
    int result;
    if (now >= target->preparation_deadline) { errno = ETIMEDOUT; result = -1; }
    else result = open_generation_log(target, events, now);
    if (result > 0) { m->reload = true; return; }
    if (!result) return;
    int error = errno;
    master_diagnostic("failed to %s log file:%s:%s\n", target->log_socket ? "connect socket for" : "open",
                     target->log_path ? target->log_path : "", strerror(error));
    char message[256]; snprintf(message, sizeof(message), "cannot prepare replacement log: %s; active configuration kept\n", strerror(error));
    reject_reload(m, message);
}
static void prepare_reload(master_t *m, hp_lifecycle_client_t *client, bool force, hp_lifecycle_config_t *config)
{
    if (!force && config->length == m->config->length && !memcmp(config->yaml, m->config->yaml, config->length)) {
        hp_lifecycle_reply(client, 0, "configuration unchanged\n"); hp_lifecycle_release(config); return;
    }
    master_t *pending = calloc(1, sizeof(*pending));
    if (!pending) { hp_lifecycle_release(config); hp_lifecycle_reply(client, EX_OSERR, "out of memory\n"); return; }
    *pending = (master_t){.pid_fd = -1, .wake = {-1, -1}, .daemon_ready = -1,
        .reuse = m, .config = config, .startup_ms = config->startup_ms ? config->startup_ms : 30000,
        .shutdown_ms = config->shutdown_ms, .next_log_fd = -1, .original_output = {-1, -1}};
    bool parsed = parse_arguments(pending, config->argv) == 0;
    if (parsed && ((pending->pid_path == NULL) != (m->pid_path == NULL) ||
                   (pending->pid_path && strcmp(pending->pid_path, m->pid_path)))) {
        hp_lifecycle_reply(client, EX_CONFIG, "pid-file is a process setting; changing it requires a restart\n");
        dispose(pending, true); hp_lifecycle_release(config); free(pending); return;
    }
    if (!parsed) {
        char message[256]; snprintf(message, sizeof(message), "cannot prepare replacement listeners/log: %s\n", strerror(errno));
        hp_lifecycle_reply(client, EX_CONFIG, message); dispose(pending, true); hp_lifecycle_release(config); free(pending); return;
    }
    pending->preparation_deadline = deadline(now_ms(), pending->startup_ms);
    m->pending = pending; m->reply_client = client;
}
static void advance_validation(master_t *m)
{
    hp_lifecycle_config_t *config; char *error;
    if (!m->validation || !hp_lifecycle_validation_step(m->validation, &config, &error)) return;
    hp_lifecycle_validation_destroy(m->validation); m->validation = NULL;
    hp_lifecycle_client_t *client = m->validation_client; m->validation_client = NULL;
    if (!config || m->stopping) {
        hp_lifecycle_reply(client, EX_CONFIG, error ? error : "server is shutting down\n");
        free(error); hp_lifecycle_release(config); return;
    }
    prepare_reload(m, client, m->validation_force, config);
}
static void handle_control(master_t *m)
{
    unsigned command; bool force; char *yaml; size_t length;
    hp_lifecycle_client_t *client;
    while ((client = hp_lifecycle_control_request(m->control, &command, &force, &yaml, &length)) != NULL) {
        if (command == HP_LIFECYCLE_STOP) {
            free(yaml); hp_lifecycle_reply(client, 0, "graceful shutdown requested\n");
            begin_shutdown(m, now_ms()); return;
        }
        if (m->stopping || m->request_count >= 16) {
            free(yaml); hp_lifecycle_reply(client, EX_TEMPFAIL, "server is stopping or reload queue is full\n"); continue;
        }
        control_request_t *request = calloc(1, sizeof(*request));
        if (!request) { free(yaml); hp_lifecycle_reply(client, EX_OSERR, "out of memory\n"); continue; }
        *request = (control_request_t){.client = client, .force = force, .yaml = yaml, .length = length};
        *m->request_tail = request; m->request_tail = &request->next; ++m->request_count;
    }
}
static void commit_reload(master_t *m)
{
    if (!m->pending) return;
    master_t *pending = m->pending;
    /* Transfer Unix-path ownership before closing the old socket copies. The
     * retiring process keeps its own descriptors until requests drain. */
    for (size_t i = 0; i < pending->listeners.size; ++i) {
        listener_t *next = &pending->listeners.entries[i];
        if (!next->borrowed) continue;
        for (size_t j = 0; j < m->listeners.size; ++j)
            if (!strcmp(next->label, m->listeners.entries[j].label)) m->listeners.entries[j].borrowed = true;
        next->borrowed = false;
    }
    for (size_t i = 0; i < m->listeners.size; ++i) close_listener(&m->listeners.entries[i]);
    free(m->listeners.entries); free(m->socket_environment); free(m->worker_argv);
    free(m->log_path); m->log_path = pending->log_path; pending->log_path = NULL;
    m->listeners = pending->listeners; pending->listeners.entries = NULL; pending->listeners.size = pending->listeners.capacity = 0;
    m->socket_environment = pending->socket_environment; pending->socket_environment = NULL;
    m->worker_argv = pending->worker_argv; pending->worker_argv = NULL;
    m->startup_ms = pending->startup_ms; m->shutdown_ms = pending->shutdown_ms;
    hp_lifecycle_release(m->config); m->config = pending->config; pending->config = NULL;
    dispose(pending, true); free(pending); m->pending = NULL;
    hp_lifecycle_reply(m->reply_client, 0, "configuration loaded\n"); m->reply_client = NULL;
}
static int supervise(master_t *m)
{
    if (install_signals(m)) return EX_OSERR;
    m->retry_at = now_ms();
    for (;;) {
        uint64_t now = now_ms(); unsigned signals = consume_signals(m);
        if ((signals & SIGNAL_STOP) && !m->stopping) begin_shutdown(m, now);
        if ((signals & SIGNAL_RELOAD) && !m->stopping && !m->config) {
            if (!m->reload && !m->candidate.pid && m->current.pid && now - m->current.started >= 30000) m->failures = 0;
            m->reload = true;
        }
        reap_child(m, &m->candidate, "candidate", now);
        reap_child(m, &m->current, "current", now);
        reap_child(m, &m->retiring, "retiring", now);
        advance_validation(m);
        now = now_ms();
        advance_generation_log(m, 0, now);
        if (m->config && !m->stopping && !m->candidate.pid && !m->retiring.pid && !m->pending && !m->validation && m->requests) {
            control_request_t *request = m->requests; m->requests = request->next;
            if (!m->requests) m->request_tail = &m->requests;
            --m->request_count;
            start_validation(m, request->client, request->force, request->yaml, request->length);
            free(request); now = now_ms();
        }
        if (m->stopping) {
            if (!m->current.pid && !m->candidate.pid && !m->retiring.pid && !m->validation) break;
            if (now >= m->shutdown_at) {
                child_t *children[] = {&m->current, &m->candidate, &m->retiring};
                for (size_t i = 0; i < 3; ++i) if (children[i]->pid) kill(children[i]->pid, SIGKILL);
                m->shutdown_at = UINT64_MAX;
            }
        } else {
            if (m->retiring.pid && now >= m->retiring.deadline) {
                kill(m->retiring.pid, SIGKILL); m->retiring.deadline = UINT64_MAX;
            }
            if (m->candidate.pid && now >= m->candidate.deadline) {
                master_diagnostic("h2o.master: generation %" PRIu64 " startup timed out\n", m->candidate.generation);
                kill(m->candidate.pid, SIGKILL);
                m->candidate.deadline = UINT64_MAX;
            }
            bool need = !m->current.pid || (m->reload && !m->retiring.pid);
            if (!m->candidate.pid && need && now >= m->retry_at) {
                m->reload = false;
                if (start_child(m, now)) {
                    master_diagnostic("h2o.master: fork/pipe failed: %s\n", strerror(errno));
                    if (m->config) {
                        reject_reload(m, "cannot start replacement process; active configuration kept\n");
                        if (!m->current.pid) { m->exit_status = EX_OSERR; begin_shutdown(m, now); }
                    } else retry_later(m, now);
                }
            }
        }
        struct pollfd fds[4 + HP_LIFECYCLE_CLIENT_LIMIT] = {{.fd = m->wake[0], .events = POLLIN}, {.fd = m->candidate.pid ? m->candidate.ready_fd : -1, .events = POLLIN},
            {.fd = m->control && !m->stopping && hp_lifecycle_control_has_capacity(m->control) ? m->control_fd : -1, .events = POLLIN},
            {.fd = m->pending && m->pending->log_state == LOG_CONNECTING ? m->pending->next_log_fd : -1, .events = POLLOUT}};
        uint64_t next = deadline(now, 1000);
        if (m->requests && !m->candidate.pid && !m->retiring.pid && !m->pending && !m->validation && !m->stopping) next = now;
        if (m->validation && hp_lifecycle_validation_deadline(m->validation) < next)
            next = hp_lifecycle_validation_deadline(m->validation);
        if (m->pending && !m->candidate.pid && m->pending->log_state != LOG_READY) {
            if (m->pending->preparation_deadline < next) next = m->pending->preparation_deadline;
            if (m->pending->log_state == LOG_RETRY && m->pending->log_retry_at < next) next = m->pending->log_retry_at;
        }
        if (m->stopping) { if (m->shutdown_at < next) next = m->shutdown_at; }
        else {
            if (m->candidate.pid && m->candidate.deadline < next) next = m->candidate.deadline;
            if (m->retiring.pid && m->retiring.deadline < next) next = m->retiring.deadline;
            if (!m->candidate.pid && (!m->current.pid || (m->reload && !m->retiring.pid)) && m->retry_at < next) next = m->retry_at;
        }
        if (m->control) hp_lifecycle_control_poll(m->control, fds + 4, &next);
        int timeout = next <= now ? 0 : (int)(next - now);
        int ready = poll(fds, m->control ? 4 + HP_LIFECYCLE_CLIENT_LIMIT : 4, timeout);
        if (ready < 0 && errno != EINTR) { begin_shutdown(m, now_ms()); continue; }
        if (ready > 0 && fds[1].revents && m->candidate.pid && !m->stopping) {
            char byte; ssize_t count = read(m->candidate.ready_fd, &byte, 1);
            if (count == 1 && byte == 'R') {
                close(m->candidate.ready_fd); m->candidate.ready_fd = -1;
                if (m->pending && apply_generation_logs(m, m->pending)) {
                    kill(m->candidate.pid, SIGKILL);
                    reject_reload(m, "cannot switch logging; active configuration kept\n");
                    continue;
                }
                if (m->current.pid) {
                    m->retiring = m->current; m->retiring.deadline = deadline(now_ms(), m->shutdown_ms);
                    kill(m->retiring.pid, SIGTERM);
                }
                m->current = m->candidate; m->current.started = now_ms(); m->candidate = (child_t){.ready_fd = -1};
                commit_reload(m);
                master_diagnostic("h2o.master: generation %" PRIu64 " pid %ld ready\n", m->current.generation, (long)m->current.pid);
                if (m->daemon_ready >= 0) { char byte = 'R'; ssize_t ignored = write(m->daemon_ready, &byte, 1); (void)ignored; close(m->daemon_ready); m->daemon_ready = -1; }
                update_status(m);
            } else if (!count || (count < 0 && errno != EAGAIN && errno != EINTR)) {
                close(m->candidate.ready_fd); m->candidate.ready_fd = -1; kill(m->candidate.pid, SIGKILL);
            }
        }
        if (ready > 0 && fds[3].revents) advance_generation_log(m, fds[3].revents, now_ms());
        if (m->control) {
            now = now_ms();
            hp_lifecycle_control_process(m->control, fds + 4, now);
            handle_control(m);
            /* Process the old poll snapshot before slots can be reused. */
            if (!m->stopping && ready > 0 && (fds[2].revents & POLLIN))
                hp_lifecycle_control_accept(m->control, m->control_fd, now);
        }
    }
    signal_pipe = -1;
    for (size_t i = 0; i < 5; ++i) sigaction(managed_signals[i], &m->old_actions[i], NULL);
    return m->exit_status;
}
static void dispose(master_t *m, bool remove_paths)
{
    if (m->pid_fd >= 0) {
        struct stat fdstat, pathstat;
        if (remove_paths && m->pid_owned && m->pid_path && !fstat(m->pid_fd, &fdstat) && !lstat(m->pid_path, &pathstat) &&
            fdstat.st_dev == pathstat.st_dev && fdstat.st_ino == pathstat.st_ino) unlink(m->pid_path);
        close(m->pid_fd);
    }
    if (remove_paths && m->status_owned && m->status_path) unlink(m->status_path);
    for (size_t i = 0; i < m->listeners.size; ++i) {
        if (!remove_paths) { free(m->listeners.entries[i].path); m->listeners.entries[i].path = NULL; }
        close_listener(m->listeners.entries + i);
    }
    if (m->wake[0] >= 0) close(m->wake[0]);
    if (m->wake[1] >= 0) close(m->wake[1]);
    if (m->daemon_ready >= 0) close(m->daemon_ready);
    if (m->next_log_fd >= 0) close(m->next_log_fd);
    if (m->config) for (size_t i = 0; i < 2; ++i)
        if (m->original_output[i] >= 0) close(m->original_output[i]);
    free(m->listeners.entries); free(m->socket_environment); free(m->worker_argv);
    free(m->pid_path); free(m->status_path); free(m->log_path);
    hp_lifecycle_control_destroy(m->control);
}
int hp_native_master_run(char *const arguments[], uint64_t startup_ms, uint64_t shutdown_ms)
{
    master_t m = {.pid_fd = -1, .wake = {-1, -1}, .daemon_ready = -1,
        .current.ready_fd = -1, .candidate.ready_fd = -1, .retiring.ready_fd = -1,
        .startup_ms = startup_ms ? startup_ms : 30000, .shutdown_ms = shutdown_ms,
        .control_fd = -1, .next_log_fd = -1, .original_output = {-1, -1}};
    int result = EX_OSERR;
    if (parse_arguments(&m, arguments) || redirect_logs(&m)) {
        master_diagnostic("h2o.master: listener/log configuration failed: %s\n", strerror(errno)); goto Done;
    }
    int detached = daemonize(&m);
    if (detached < 0) { master_diagnostic("h2o.master: daemon startup failed: %s\n", strerror(errno)); goto Done; }
    if (detached > 0) { dispose(&m, false); return 0; }
    if (open_pid_file(&m)) { master_diagnostic("h2o.master: pid file failed: %s\n", strerror(errno)); goto Done; }
    master_diagnostic("h2o.master: native supervisor pid %ld starting\n", (long)getpid());
    result = supervise(&m);
Done:
    dispose(&m, true); return result;
}
int hp_native_lifecycle_run(hp_lifecycle_config_t *config, int control_fd)
{
    master_t m = {.pid_fd = -1, .wake = {-1, -1}, .daemon_ready = -1,
        .current.ready_fd = -1, .candidate.ready_fd = -1, .retiring.ready_fd = -1,
        .startup_ms = config->startup_ms ? config->startup_ms : 30000, .shutdown_ms = config->shutdown_ms,
        .config = config, .control_fd = control_fd, .next_log_fd = -1, .original_output = {-1, -1}};
    hp_lifecycle_retain(config);
    m.control = hp_lifecycle_control_create();
    m.request_tail = &m.requests;
    m.original_output[0] = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    m.original_output[1] = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
    int result = EX_CONFIG;
    if (!m.control || m.original_output[0] < 0 || m.original_output[1] < 0 ||
        parse_arguments(&m, config->argv) || redirect_logs(&m) || open_pid_file(&m))
        master_diagnostic("h2o-php run: cannot initialize instance: %s\n", strerror(errno));
    else
        result = supervise(&m);
    reject_reload(&m, "server stopped\n");
    dispose(&m, true); hp_lifecycle_release(m.config); return result;
}
