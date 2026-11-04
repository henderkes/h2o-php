#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "master.h"
#include "h2o.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
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

#define MASTER_READY_ENV "H2O_PHP_READY_FD"
typedef struct listener {
    int fd;
    char *label, *path;
    dev_t device;
    ino_t inode;
} listener_t;
typedef struct child {
    pid_t pid;
    uint64_t generation, started, deadline;
    int ready_fd;
} child_t;
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
    if (listener->path) {
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
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) || chmod(path, 0777) || listen(fd, SOMAXCONN)) {
        int saved = errno; close(fd); unlink(path); errno = saved; return -1;
    }
    return append_listener(m, fd, path, path);
}
static int bind_inet(master_t *m, const char *specification)
{
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
        if (!ok) fprintf(stderr, "h2o.master: failed to update status %s: %s\n", m->status_path, strerror(errno));
        unlink(temporary);
    }
    free(temporary);
}
static int redirect_logs(master_t *m)
{
    if (!m->log_path) return 0;
    int fd = h2o_access_log_open_log(m->log_path); if (fd < 0) return -1;
    fflush(NULL);
    int status = dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0 ? -1 : 0;
    if (fd > STDERR_FILENO) close(fd);
    return status;
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
    int ready[2]; if (make_pipe(ready, true)) return -1;
    uint64_t generation = ++m->generation;
    char generation_text[32], descriptor[32];
    snprintf(generation_text, sizeof(generation_text), "%" PRIu64, generation);
    snprintf(descriptor, sizeof(descriptor), "%d", ready[1]);
    pid_t pid = fork();
    if (pid < 0) { close(ready[0]); close(ready[1]); return -1; }
    if (!pid) {
        close(ready[0]); close(m->wake[0]); close(m->wake[1]);
        for (size_t i = 0; i < 5; ++i) {
            struct sigaction action = {.sa_handler = SIG_DFL}; sigemptyset(&action.sa_mask);
            sigaction(managed_signals[i], &action, NULL);
        }
        sigprocmask(SIG_SETMASK, &m->original_mask, NULL);
        for (size_t i = 0; i < m->listeners.size; ++i)
            if (set_fd_flag(m->listeners.entries[i].fd, FD_CLOEXEC, false)) _exit(EX_OSERR);
        if (set_fd_flag(ready[1], FD_CLOEXEC, false) ||
            setenv("SERVER_STARTER_PORT", m->socket_environment, 1) ||
            setenv("SERVER_STARTER_GENERATION", generation_text, 1) ||
            setenv("H2O_VIA_MASTER", "1", 1) || setenv(MASTER_READY_ENV, descriptor, 1)) _exit(EX_OSERR);
        unsetenv("H2O_PHP_CLI_CHILD");
        execvp(m->worker_argv[0], m->worker_argv);
        dprintf(STDERR_FILENO, "h2o.master: exec %s failed: %s\n", m->worker_argv[0], strerror(errno)); _exit(EX_CONFIG);
    }
    close(ready[1]);
    m->candidate = (child_t){.pid = pid, .generation = generation, .started = now,
        .deadline = deadline(now, m->startup_ms), .ready_fd = ready[0]};
    m->retry_at = 0;
    fprintf(stderr, "h2o.master: starting generation %" PRIu64 " pid %ld\n", generation, (long)pid);
    update_status(m); return 0;
}
static void retry_later(master_t *m, uint64_t now)
{
    if (m->failures < UINT_MAX) ++m->failures;
    unsigned shift = m->failures > 6 ? 6 : m->failures - 1;
    uint64_t delay = 100ULL << shift; if (delay > 5000) delay = 5000;
    m->retry_at = deadline(now, delay);
    fprintf(stderr, "h2o.master: retry in %" PRIu64 " ms after %u consecutive failures\n", delay, m->failures);
}
static bool reap_child(master_t *m, child_t *child, const char *kind, uint64_t now)
{
    if (!child->pid) return false;
    int status; pid_t pid = waitpid(child->pid, &status, WNOHANG);
    if (!pid || (pid < 0 && errno == EINTR)) return false;
    if (pid < 0 && errno != ECHILD) return false;
    if (pid < 0) status = 0;
    fprintf(stderr, "h2o.master: %s generation %" PRIu64 " pid %ld exited status %d\n", kind, child->generation, (long)child->pid, status);
    bool was_candidate = child == &m->candidate;
    bool unexpected = child == &m->current || was_candidate;
    if (child == &m->current && now - child->started >= 30000) m->failures = 0;
    if (child->ready_fd >= 0) close(child->ready_fd);
    *child = (child_t){.ready_fd = -1};
    if (unexpected && !m->stopping) {
        if (was_candidate && m->current.pid) m->reload = true;
        retry_later(m, now);
    }
    update_status(m); return true;
}
static void begin_shutdown(master_t *m, uint64_t now)
{
    m->stopping = true; m->shutdown_at = deadline(now, m->shutdown_ms);
    child_t *children[] = {&m->current, &m->candidate, &m->retiring};
    for (size_t i = 0; i < 3; ++i) if (children[i]->pid) kill(children[i]->pid, SIGTERM);
    fprintf(stderr, "h2o.master: draining worker generations\n");
}
static int supervise(master_t *m)
{
    if (install_signals(m)) return EX_OSERR;
    m->retry_at = now_ms();
    for (;;) {
        uint64_t now = now_ms(); unsigned signals = consume_signals(m);
        if ((signals & SIGNAL_STOP) && !m->stopping) begin_shutdown(m, now);
        if ((signals & SIGNAL_RELOAD) && !m->stopping) {
            if (!m->reload && !m->candidate.pid && m->current.pid && now - m->current.started >= 30000) m->failures = 0;
            m->reload = true;
        }
        reap_child(m, &m->candidate, "candidate", now);
        reap_child(m, &m->current, "current", now);
        reap_child(m, &m->retiring, "retiring", now);
        if (m->stopping) {
            if (!m->current.pid && !m->candidate.pid && !m->retiring.pid) break;
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
                fprintf(stderr, "h2o.master: generation %" PRIu64 " startup timed out\n", m->candidate.generation);
                kill(m->candidate.pid, SIGKILL);
                m->candidate.deadline = UINT64_MAX;
            }
            bool need = !m->current.pid || (m->reload && !m->retiring.pid);
            if (!m->candidate.pid && need && now >= m->retry_at) {
                m->reload = false;
                if (start_child(m, now)) {
                    fprintf(stderr, "h2o.master: fork/pipe failed: %s\n", strerror(errno)); retry_later(m, now);
                }
            }
        }
        struct pollfd fds[2] = {{.fd = m->wake[0], .events = POLLIN}, {.fd = m->candidate.pid ? m->candidate.ready_fd : -1, .events = POLLIN}};
        uint64_t next = deadline(now, 1000);
        if (m->stopping) { if (m->shutdown_at < next) next = m->shutdown_at; }
        else {
            if (m->candidate.pid && m->candidate.deadline < next) next = m->candidate.deadline;
            if (m->retiring.pid && m->retiring.deadline < next) next = m->retiring.deadline;
            if (!m->candidate.pid && (!m->current.pid || (m->reload && !m->retiring.pid)) && m->retry_at < next) next = m->retry_at;
        }
        int timeout = next <= now ? 0 : (int)(next - now);
        int ready = poll(fds, 2, timeout);
        if (ready < 0 && errno != EINTR) { begin_shutdown(m, now_ms()); continue; }
        if (ready > 0 && fds[1].revents && m->candidate.pid && !m->stopping) {
            char byte; ssize_t count = read(m->candidate.ready_fd, &byte, 1);
            if (count == 1 && byte == 'R') {
                close(m->candidate.ready_fd); m->candidate.ready_fd = -1;
                if (m->current.pid) {
                    m->retiring = m->current; m->retiring.deadline = deadline(now_ms(), m->shutdown_ms);
                    kill(m->retiring.pid, SIGTERM);
                }
                m->current = m->candidate; m->current.started = now_ms(); m->candidate = (child_t){.ready_fd = -1};
                fprintf(stderr, "h2o.master: generation %" PRIu64 " pid %ld ready\n", m->current.generation, (long)m->current.pid);
                if (m->daemon_ready >= 0) { char byte = 'R'; ssize_t ignored = write(m->daemon_ready, &byte, 1); (void)ignored; close(m->daemon_ready); m->daemon_ready = -1; }
                update_status(m);
            } else if (!count || (count < 0 && errno != EAGAIN && errno != EINTR)) {
                close(m->candidate.ready_fd); m->candidate.ready_fd = -1; kill(m->candidate.pid, SIGKILL);
            }
        }
    }
    signal_pipe = -1;
    for (size_t i = 0; i < 5; ++i) sigaction(managed_signals[i], &m->old_actions[i], NULL);
    return 0;
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
    free(m->listeners.entries); free(m->socket_environment); free(m->worker_argv);
    free(m->pid_path); free(m->status_path); free(m->log_path);
}
int hp_native_master_run(char *const arguments[], uint64_t startup_ms, uint64_t shutdown_ms)
{
    master_t m = {.pid_fd = -1, .wake = {-1, -1}, .daemon_ready = -1,
        .current.ready_fd = -1, .candidate.ready_fd = -1, .retiring.ready_fd = -1,
        .startup_ms = startup_ms ? startup_ms : 30000, .shutdown_ms = shutdown_ms};
    int result = EX_OSERR;
    if (parse_arguments(&m, arguments) || redirect_logs(&m)) {
        fprintf(stderr, "h2o.master: listener/log configuration failed: %s\n", strerror(errno)); goto Done;
    }
    int detached = daemonize(&m);
    if (detached < 0) { fprintf(stderr, "h2o.master: daemon startup failed: %s\n", strerror(errno)); goto Done; }
    if (detached > 0) { dispose(&m, false); return 0; }
    if (open_pid_file(&m)) { fprintf(stderr, "h2o.master: pid file failed: %s\n", strerror(errno)); goto Done; }
    fprintf(stderr, "h2o.master: native supervisor pid %ld starting\n", (long)getpid());
    result = supervise(&m);
Done:
    dispose(&m, true); return result;
}
