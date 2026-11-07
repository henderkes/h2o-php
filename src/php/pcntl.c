/* Embedding adapter for the real PCNTL extension. PHP callbacks and PCNTL's
 * pending queue are touched only by their owning Zend interpreter. Signal
 * frames write bounded C records; H2O threads never enter PHP signal code.
 * PCNTL globals layout and queue handling follow PHP 8.6 (BSD-3-Clause). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "h2o_php.h"
#include "../observability/log.h"
#include "../runtime/host_fds.h"
#include <php.h>
#include <php_main.h>
#include <Zend/zend_exceptions.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/vfs.h>
#include <linux/magic.h>
#endif
#include <time.h>
#include <unistd.h>

typedef struct pcntl_pending {
    struct pcntl_pending *next;
    zend_long signo;
    siginfo_t siginfo;
} pcntl_pending_t;
typedef struct {
    HashTable php_signal_table;
    bool processing_signal_queue;
    volatile bool pending_signals;
    bool async_signals;
    uint16_t num_signals;
    int last_error;
    pcntl_pending_t *head, *tail, *spares;
} pcntl_globals_t;
typedef struct { int signo; pid_t tid; siginfo_t info; } signal_event_t;
#define SIGNAL_QUEUE 128
typedef struct pcntl_context {
    struct pcntl_context *next;
    pthread_t thread;
    pid_t tid;
    uint64_t id;
    zend_atomic_bool *interrupt;
    pcntl_globals_t *globals;
    pthread_cond_t changed;
    sigset_t mask, initial_mask, waiting;
    bool subscribed[NSIG];
    bool restart[NSIG];
    uint64_t alarm_ns;
    signal_event_t events[SIGNAL_QUEUE];
    unsigned count;
} pcntl_context_t;
typedef struct child_record {
    struct child_record *next;
    pid_t pid, pgid;
    uint64_t owner, serial;
    bool orphan, exited;
    int status;
    struct rusage usage;
#ifdef __linux__
    int pidfd;
    dev_t pidfd_device;
    ino_t pidfd_inode;
#endif
} child_record_t;
#define CHILD_STATUS_LIMIT 4096
static pthread_mutex_t signal_lock = PTHREAD_MUTEX_INITIALIZER;
static pcntl_context_t *contexts;
static child_record_t *children;
static zend_module_entry *pcntl_module;
static int (*native_rshutdown)(int, int);
static void (*native_interrupt)(zend_execute_data *);
static zif_handler native_fork, native_signal, native_dispatch, native_mask;
static zif_handler native_waitpid, native_wait, native_waitid, native_alarm, native_sigwait, native_sigtimedwait;
static pthread_t broker_thread;
static int signal_pipe[2] = {-1, -1};
static struct sigaction saved_actions[NSIG];
static volatile sig_atomic_t installed_actions[NSIG];
static bool saved_valid[NSIG];
/* Immutable snapshots keep the raw signal frame independent of registry
 * updates, including H2O's final graceful-shutdown handler installation. */
static _Atomic(struct sigaction *) chained_actions[NSIG];
static atomic_bool broker_stopping;
static bool broker_started;
static uint64_t next_context_id = 1;
static uint64_t next_child_serial = 1;
static volatile sig_atomic_t fork_child;
static int wake_signal, restart_wake_signal;
static _Thread_local pcntl_context_t *local_context;
static _Thread_local bool context_closing;
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "The signal broker needs lock-free pointer loads");

bool hp_process_is_fork_child(void) { return fork_child != 0; }
static void free_child(child_record_t *child) {
    if (!child) return;
#ifdef __linux__
    if (child->pidfd >= 0) close(child->pidfd);
#endif
    free(child);
}
#ifdef __linux__
/* PIDFD is a Linux waitid kind even when PHP's headers omit its constant.
 * PIDFS gives distinct opens of the same process a stable inode. Retaining an
 * anchor prevents inode lifetime/reuse ambiguity after the broker reaps it. */
#define HP_P_PIDFD ((idtype_t)3)
#ifndef PID_FS_MAGIC
#define PID_FS_MAGIC 0x50494446
#endif
static void anchor_child(child_record_t *child) {
    child->pidfd = -1;
#ifdef SYS_pidfd_open
    int fd = (int)syscall(SYS_pidfd_open, child->pid, 0);
    if (fd < 0) return;
    struct stat identity; struct statfs filesystem;
    if (fstat(fd, &identity) || fstatfs(fd, &filesystem) || filesystem.f_type != PID_FS_MAGIC) { close(fd); return; }
    child->pidfd = fd; child->pidfd_device = identity.st_dev; child->pidfd_inode = identity.st_ino;
#endif
}
/* Only called after a nonconsuming kernel wait has validated the pinned FD.
 * A live fallback never associates a cached record by a recycled numeric PID.
 * On pre-PIDFS kernels, a separately opened already-reaped PIDFD therefore
 * retains native ECHILD rather than guessing ownership from an anonymous inode. */
static child_record_t *pidfd_child(int fd) {
    struct stat identity; struct statfs filesystem;
    if (!fstat(fd, &identity) && !fstatfs(fd, &filesystem) && filesystem.f_type == PID_FS_MAGIC) {
        for (child_record_t *child = children; child; child = child->next)
            if (child->pidfd >= 0 && child->pidfd_device == identity.st_dev && child->pidfd_inode == identity.st_ino) return child;
    }
    char path[64], line[128]; long pid = -1;
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    FILE *file = fopen(path, "re");
    if (!file) return NULL;
    while (fgets(line, sizeof(line), file)) if (sscanf(line, "Pid:\t%ld", &pid) == 1) break;
    fclose(file);
    if (pid > 0 && pid <= INT_MAX) for (child_record_t *child = children; child; child = child->next)
        if (!child->exited && child->pid == (pid_t)pid) return child;
    return NULL;
}
static int kernel_waitid(idtype_t kind, id_t id, siginfo_t *info, int options, struct rusage *usage) {
#ifdef SYS_waitid
    return (int)syscall(SYS_waitid, kind, id, info, options, usage);
#else
    (void)usage;
    return waitid(kind, id, info, options);
#endif
}
#endif
static void wake_handler(int sig) { (void)sig; }
static pid_t current_tid(void) {
#ifdef SYS_gettid
    return (pid_t)syscall(SYS_gettid);
#else
    return (pid_t)getpid();
#endif
}
static bool reserved_signal(int sig) {
    return sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT || sig == SIGILL || sig == SIGFPE ||
           sig == wake_signal || sig == restart_wake_signal || sig == SIGRTMIN;
}
static void signal_frame(int signo, siginfo_t *info, void *ucontext) {
    int saved_errno = errno;
    signal_event_t event = {.signo = signo, .tid = current_tid()};
    if (info) event.info = *info;
    else event.info.si_signo = signo;
    if (signal_pipe[1] >= 0) (void)write(signal_pipe[1], &event, sizeof(event));
    /* Preserve H2O's process lifecycle. Only its native handler executes here. */
    if (signo == SIGTERM || signo == SIGHUP) {
        struct sigaction *old = atomic_load_explicit(&chained_actions[signo], memory_order_acquire);
        if (!old) { errno = saved_errno; return; }
        if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN && old->sa_sigaction != signal_frame) {
            if (old->sa_flags & SA_SIGINFO) old->sa_sigaction(signo, info, ucontext);
            else old->sa_handler(signo);
        }
    }
    errno = saved_errno;
}
/* Scheduler/control thread only, with signal_lock held. */
static int install_signal(int signo) {
    if (installed_actions[signo]) return 0;
    if (!saved_valid[signo]) {
        if (sigaction(signo, NULL, &saved_actions[signo])) return -1;
        saved_valid[signo] = true;
        atomic_store_explicit(&chained_actions[signo], &saved_actions[signo], memory_order_release);
    }
    struct sigaction action = {0};
    action.sa_sigaction = signal_frame;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigfillset(&action.sa_mask);
    if (sigaction(signo, &action, NULL)) return -1;
    installed_actions[signo] = true;
    return 0;
}
void hp_process_set_signal(int signo, void (*handler)(int)) {
    struct sigaction *action = calloc(1, sizeof(*action));
    if (!action) abort();
    action->sa_handler = handler; sigemptyset(&action->sa_mask);
    pthread_mutex_lock(&signal_lock);
    /* This process-lifetime snapshot remains reachable until process exit;
     * an in-flight signal frame may still reference the previous snapshot. */
    atomic_store_explicit(&chained_actions[signo], action, memory_order_release);
    saved_valid[signo] = true;
    if (!installed_actions[signo]) sigaction(signo, action, NULL);
    pthread_mutex_unlock(&signal_lock);
}
static void reap_orphans(void) {
    pthread_mutex_lock(&signal_lock);
    child_record_t **link = &children;
    while (*link) {
        child_record_t *child = *link;
        pid_t reaped = 0;
        if (!child->exited) {
            pid_t group = getpgid(child->pid); if (group >= 0) child->pgid = group;
            errno = 0;
            reaped = wait4(child->pid, &child->status, WNOHANG, &child->usage);
            if (reaped == child->pid) child->exited = true;
        }
        if ((child->orphan && child->exited) || (reaped < 0 && errno == ECHILD)) {
            *link = child->next; free_child(child);
        } else link = &child->next;
    }
    pthread_mutex_unlock(&signal_lock);
}
static void deliver_event(const signal_event_t *event) {
    pthread_mutex_lock(&signal_lock);
    pcntl_context_t *chosen = NULL, *blocked = NULL;
    uint64_t child_owner = 0;
    if (event->signo == SIGCHLD) {
        for (child_record_t *c = children; c; c = c->next)
            if (c->pid == event->info.si_pid) { child_owner = c->owner; break; }
    }
    for (pcntl_context_t *c = contexts; c; c = c->next) {
        if (event->signo == SIGCHLD) pthread_cond_broadcast(&c->changed);
        bool wait_match = sigismember(&c->waiting, event->signo) == 1;
        if (sigismember(&c->mask, event->signo) == 1 && !blocked) blocked = c;
        if (!wait_match && (!c->subscribed[event->signo] || sigismember(&c->mask, event->signo) == 1)) continue;
        if (child_owner && c->id != child_owner) continue;
        chosen = c;
        if (wait_match || (event->info.si_code == SI_TKILL && c->tid == event->tid)) break;
    }
    if (!chosen) chosen = blocked;
    if (chosen && chosen->count < SIGNAL_QUEUE) {
        chosen->events[chosen->count++] = *event;
        zend_atomic_bool_store(chosen->interrupt, true);
        pthread_cond_broadcast(&chosen->changed);
        (void)pthread_kill(chosen->thread, chosen->restart[event->signo] ? restart_wake_signal : wake_signal);
    }
    pthread_mutex_unlock(&signal_lock);
}
static void *signal_broker(void *unused) {
    (void)unused;
    signal_event_t event;
    while (!atomic_load(&broker_stopping)) {
        struct pollfd input = {.fd = signal_pipe[0], .events = POLLIN};
        int ready = poll(&input, 1, 50);
        ssize_t n = ready > 0 ? read(signal_pipe[0], &event, sizeof(event)) : 0;
        if (n == sizeof(event) && event.signo > 0 && event.signo < NSIG) deliver_event(&event);
        if (n < 0 && errno == EINTR) continue;
        pthread_mutex_lock(&signal_lock);
        uint64_t now = hp_now_ns();
        for (pcntl_context_t *c = contexts; c; c = c->next) if (c->alarm_ns && c->alarm_ns <= now) {
            c->alarm_ns = 0;
            if (c->count < SIGNAL_QUEUE) {
                signal_event_t alarm = {.signo = SIGALRM, .tid = c->tid};
                alarm.info.si_signo = SIGALRM; alarm.info.si_code = SI_KERNEL;
                c->events[c->count++] = alarm; zend_atomic_bool_store(c->interrupt, true);
                pthread_cond_broadcast(&c->changed);
                (void)pthread_kill(c->thread, c->restart[SIGALRM] ? restart_wake_signal : wake_signal);
            }
        }
        pthread_mutex_unlock(&signal_lock);
        reap_orphans();
    }
    return NULL;
}
int hp_process_start(void) {
    if (broker_started) return 0;
    wake_signal = SIGRTMIN + 6;
    restart_wake_signal = wake_signal + 1;
    if (restart_wake_signal > SIGRTMAX) { errno = ENOTSUP; return -1; }
    if (pipe2(signal_pipe, O_CLOEXEC)) return -1;
    int flags = fcntl(signal_pipe[1], F_GETFL);
    if (flags < 0 || fcntl(signal_pipe[1], F_SETFL, flags | O_NONBLOCK)) goto Failed;
    struct sigaction wake = {0}; wake.sa_handler = wake_handler; sigemptyset(&wake.sa_mask);
    if (sigaction(wake_signal, &wake, NULL)) goto Failed;
    wake.sa_flags = SA_RESTART;
    if (sigaction(restart_wake_signal, &wake, NULL)) goto Failed;
    pthread_mutex_lock(&signal_lock);
    int failed = install_signal(SIGCHLD);
    pthread_mutex_unlock(&signal_lock);
    if (failed) goto Failed;
    atomic_store(&broker_stopping, false);
    int rc = pthread_create(&broker_thread, NULL, signal_broker, NULL);
    if (rc) { errno = rc; goto Failed; }
    broker_started = true; return 0;
Failed:
    close(signal_pipe[0]); close(signal_pipe[1]); signal_pipe[0] = signal_pipe[1] = -1;
    return -1;
}
void hp_process_stop(void) {
    if (!broker_started || fork_child) return;
    atomic_store(&broker_stopping, true);
    signal_event_t end = {0}; (void)write(signal_pipe[1], &end, sizeof(end));
    pthread_join(broker_thread, NULL);
    while (children) { child_record_t *child = children; children = child->next; free_child(child); }
    for (int sig = 1; sig < NSIG; ++sig) if (installed_actions[sig]) {
        sigaction(sig, atomic_load_explicit(&chained_actions[sig], memory_order_acquire), NULL); installed_actions[sig] = false;
    }
    close(signal_pipe[0]); close(signal_pipe[1]); signal_pipe[0] = signal_pipe[1] = -1;
    broker_started = false;
}
static pcntl_globals_t *get_globals(void) {
    if (!pcntl_module || !pcntl_module->globals_id_ptr || pcntl_module->globals_size != sizeof(pcntl_globals_t)) return NULL;
    return ts_resource_ex(*pcntl_module->globals_id_ptr, NULL);
}
void hp_pcntl_context_enter(void) {
    if (local_context || context_closing || !broker_started || fork_child) return;
    pcntl_globals_t *g = get_globals();
    if (!g || g->num_signals < 2) return; /* called after the module RINIT */
    pcntl_context_t *c = calloc(1, sizeof(*c));
    if (!c) return;
    c->thread = pthread_self(); c->tid = current_tid(); c->globals = g; c->interrupt = &EG(vm_interrupt);
    sigemptyset(&c->waiting); pthread_sigmask(SIG_SETMASK, NULL, &c->mask); c->initial_mask = c->mask;
    for (int sig = 1; sig < NSIG; ++sig) c->restart[sig] = sig != SIGALRM;
    pthread_condattr_t attr; pthread_condattr_init(&attr); pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&c->changed, &attr); pthread_condattr_destroy(&attr);
    pthread_mutex_lock(&signal_lock);
    c->id = next_context_id++; c->next = contexts; contexts = c; local_context = c;
    pthread_mutex_unlock(&signal_lock);
}
void hp_pcntl_spawn_prepare(sigset_t *previous) {
    sigset_t managed; sigemptyset(&managed);
    pthread_mutex_lock(&signal_lock);
    for (int sig = 1; sig < NSIG; ++sig) if (installed_actions[sig]) sigaddset(&managed, sig);
    pthread_mutex_unlock(&signal_lock);
    pthread_sigmask(SIG_BLOCK, &managed, previous);
}
void hp_pcntl_spawn_restore(const sigset_t *previous) {
    pthread_sigmask(SIG_SETMASK, previous, NULL);
}
void hp_pcntl_context_leave(void) {
    pcntl_context_t *c = local_context;
    if (!c || fork_child) return;
    pthread_mutex_lock(&signal_lock);
    pcntl_context_t **link = &contexts;
    while (*link && *link != c) link = &(*link)->next;
    if (*link) *link = c->next;
    bool orphaned_children = false;
    for (child_record_t *child = children; child; child = child->next)
        if (child->owner == c->id) { child->orphan = true; orphaned_children = true; }
    for (int sig = 1; sig < NSIG; ++sig) if (installed_actions[sig] && sig != SIGCHLD) {
        bool used = false;
        for (pcntl_context_t *other = contexts; other; other = other->next)
            if (other->subscribed[sig] || sigismember(&other->mask, sig) == 1 || sigismember(&other->waiting, sig) == 1) { used = true; break; }
        if (!used) { sigaction(sig, atomic_load_explicit(&chained_actions[sig], memory_order_acquire), NULL); installed_actions[sig] = false; }
    }
    local_context = NULL;
    pthread_mutex_unlock(&signal_lock);
    pthread_sigmask(SIG_SETMASK, &c->initial_mask, NULL);
    pthread_cond_destroy(&c->changed); free(c);
    /* SIGCHLD and alarms have independent wake paths. A context without child
     * records creates no reaping work when it leaves the interpreter. */
    if (orphaned_children) {
        signal_event_t check = {0}; (void)write(signal_pipe[1], &check, sizeof(check));
    }
}
static void preallocate_signals(pcntl_globals_t *g) {
    if (g->spares) return;
    for (unsigned i = 0; i < g->num_signals; ++i) {
        pcntl_pending_t *p = emalloc(sizeof(*p)); p->next = g->spares; g->spares = p;
    }
}
static void drain_pending(void) {
    pcntl_context_t *c = local_context;
    if (!c || fork_child) return;
    pthread_mutex_lock(&signal_lock);
    signal_event_t ready[SIGNAL_QUEUE]; unsigned n = 0, retained = 0;
    for (unsigned i = 0; i < c->count; ++i) {
        if (sigismember(&c->mask, c->events[i].signo) == 1) c->events[retained++] = c->events[i];
        else ready[n++] = c->events[i];
    }
    c->count = retained;
    pthread_mutex_unlock(&signal_lock);
    pcntl_globals_t *g = c->globals;
    for (unsigned i = 0; i < n; ++i) {
        if (!g->spares) continue;
        pcntl_pending_t *p = g->spares; g->spares = p->next;
        p->signo = ready[i].signo; p->siginfo = ready[i].info; p->next = NULL;
        if (g->head && g->tail) g->tail->next = p; else g->head = p;
        g->tail = p; g->pending_signals = true;
    }
}
static void broker_interrupt(zend_execute_data *execute_data) {
    drain_pending();
    if (native_interrupt) native_interrupt(execute_data);
}
static PHP_FUNCTION(hp_signal) {
    if (fork_child) { native_signal(INTERNAL_FUNCTION_PARAM_PASSTHRU); return; }
    zend_long signo; zval *handler; bool restart = true, restart_null = true;
    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_LONG(signo) Z_PARAM_ZVAL(handler)
        Z_PARAM_OPTIONAL Z_PARAM_BOOL_OR_NULL(restart, restart_null)
    ZEND_PARSE_PARAMETERS_END();
    if (restart_null && signo == SIGALRM) restart = false;
    if (signo < 1 || signo >= NSIG) { zend_argument_value_error(1, "must be a valid signal number"); RETURN_THROWS(); }
    if (reserved_signal((int)signo)) { zend_argument_value_error(1, "is reserved by the PHP/server native runtime"); RETURN_THROWS(); }
    if (signo == SIGKILL || signo == SIGSTOP) { errno = EINVAL; RETURN_FALSE; }
    if (Z_TYPE_P(handler) == IS_LONG) {
        if (Z_LVAL_P(handler) != (zend_long)SIG_DFL && Z_LVAL_P(handler) != (zend_long)SIG_IGN) {
            zend_argument_value_error(2, "must be either SIG_DFL or SIG_IGN when an integer value is given"); RETURN_THROWS();
        }
    } else if (!zend_is_callable(handler, 0, NULL)) {
        zend_argument_type_error(2, "must be of type callable|int, %s given", zend_zval_value_name(handler)); RETURN_THROWS();
    }
    hp_pcntl_context_enter(); pcntl_context_t *c = local_context;
    if (!c) RETURN_FALSE;
    pthread_mutex_lock(&signal_lock);
    int rc = install_signal((int)signo);
    if (!rc) {
        c->subscribed[signo] = Z_TYPE_P(handler) != IS_LONG || Z_LVAL_P(handler) != (zend_long)SIG_DFL;
        c->restart[signo] = restart;
    }
    pthread_mutex_unlock(&signal_lock);
    if (rc) { c->globals->last_error = errno; RETURN_FALSE; }
    preallocate_signals(c->globals);
    zval copy; ZVAL_COPY(&copy, handler);
    zend_hash_index_update(&c->globals->php_signal_table, (zend_ulong)signo, &copy);
    RETURN_TRUE;
}
static PHP_FUNCTION(hp_dispatch) {
    drain_pending(); native_dispatch(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}
static PHP_FUNCTION(hp_mask) {
    if (fork_child) { native_mask(INTERNAL_FUNCTION_PARAM_PASSTHRU); return; }
    hp_pcntl_context_enter();
    native_mask(INTERNAL_FUNCTION_PARAM_PASSTHRU);
    if (Z_TYPE_P(return_value) != IS_TRUE || !local_context) return;
    sigset_t internal; sigemptyset(&internal); sigaddset(&internal, wake_signal); sigaddset(&internal, restart_wake_signal); sigaddset(&internal, SIGRTMIN);
    pthread_sigmask(SIG_UNBLOCK, &internal, NULL);
    sigset_t mask; pthread_sigmask(SIG_SETMASK, NULL, &mask);
    pthread_mutex_lock(&signal_lock);
    local_context->mask = mask;
    for (int sig = 1; sig < NSIG; ++sig)
        if (sigismember(&mask, sig) == 1 && !reserved_signal(sig) && sig != SIGKILL && sig != SIGSTOP) (void)install_signal(sig);
    pthread_mutex_unlock(&signal_lock);
    drain_pending();
}
static PHP_FUNCTION(hp_alarm) {
    if (fork_child) { native_alarm(INTERNAL_FUNCTION_PARAM_PASSTHRU); return; }
    zend_long seconds;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_LONG(seconds) ZEND_PARSE_PARAMETERS_END();
    if (seconds < 0 || (zend_ulong)seconds > UINT_MAX) {
        zend_argument_value_error(1, "must be between 0 and %u", UINT_MAX); RETURN_THROWS();
    }
    hp_pcntl_context_enter();
    if (!local_context) RETURN_LONG(0);
    pthread_mutex_lock(&signal_lock);
    uint64_t now = hp_now_ns(), previous = local_context->alarm_ns;
    local_context->alarm_ns = seconds ? now + (uint64_t)seconds * 1000000000ULL : 0;
    pthread_mutex_unlock(&signal_lock);
    signal_event_t tick = {0}; (void)write(signal_pipe[1], &tick, sizeof(tick));
    RETURN_LONG(previous > now ? (zend_long)((previous - now + 999999999ULL) / 1000000000ULL) : 0);
}
static int adapted_rshutdown(int type, int module_number) {
    if (fork_child) return native_rshutdown ? native_rshutdown(type, module_number) : SUCCESS;
    context_closing = true;
    pcntl_globals_t *g = get_globals();
    hp_pcntl_context_leave();
    /* PCNTL still owns/free its zvals and queues. Suppress only its global
     * disposition reset: other interpreters and H2O retain their handlers. */
    if (g) {
        zval *value;
        ZEND_HASH_FOREACH_VAL(&g->php_signal_table, value) {
            zval old; ZVAL_COPY_VALUE(&old, value); ZVAL_LONG(value, (zend_long)SIG_DFL); zval_ptr_dtor(&old);
        } ZEND_HASH_FOREACH_END();
    }
    int rc = native_rshutdown ? native_rshutdown(type, module_number) : SUCCESS;
    context_closing = false; return rc;
}
static void child_signal(int signo, siginfo_t *info, void *unused) {
    (void)unused;
    pcntl_globals_t *g = local_context ? local_context->globals : NULL;
    if (!g || !g->spares) return;
    pcntl_pending_t *p = g->spares; g->spares = p->next;
    p->signo = signo; p->siginfo = *info; p->next = NULL;
    if (g->head && g->tail) g->tail->next = p; else g->head = p;
    g->tail = p; g->pending_signals = true;
    if (g->async_signals) zend_atomic_bool_store(&EG(vm_interrupt), true);
}
static PHP_FUNCTION(hp_fork) {
    if (fork_child) { native_fork(INTERNAL_FUNCTION_PARAM_PASSTHRU); return; }
    ZEND_PARSE_PARAMETERS_NONE();
    hp_pcntl_context_enter();
    if (!local_context) { errno = EINVAL; RETURN_LONG(-1); }
    child_record_t *record = calloc(1, sizeof(*record));
    if (!record) { local_context->globals->last_error = ENOMEM; RETURN_LONG(-1); }
#ifdef __linux__
    record->pidfd = -1;
#endif
    if (hp_runtime_fork_prepare(hp_sapi_worker_token(), 2000)) { free(record); local_context->globals->last_error = EAGAIN; RETURN_LONG(-1); }
    if (hp_sapi_fork_prepare(2000)) {
        hp_runtime_fork_parent(); free(record); local_context->globals->last_error = EAGAIN; RETURN_LONG(-1);
    }
    if (hp_host_fds_fork_prepare(2000)) {
        hp_sapi_fork_parent(); hp_runtime_fork_parent(); free(record);
        local_context->globals->last_error = EAGAIN; RETURN_LONG(-1);
    }
    pthread_mutex_lock(&signal_lock);
    unsigned recorded = 0; child_record_t **evict = NULL;
    for (child_record_t **link = &children; *link; link = &(*link)->next) {
        ++recorded;
        if ((*link)->exited) evict = link;
    }
    if (recorded >= CHILD_STATUS_LIMIT) {
        if (evict) { child_record_t *old = *evict; *evict = old->next; free_child(old); }
        else {
            pthread_mutex_unlock(&signal_lock);
            hp_host_fds_fork_parent(); hp_sapi_fork_parent(); hp_runtime_fork_parent(); free(record);
            local_context->globals->last_error = EAGAIN; RETURN_LONG(-1);
        }
    }
    native_fork(INTERNAL_FUNCTION_PARAM_PASSTHRU);
    if (Z_TYPE_P(return_value) == IS_LONG && Z_LVAL_P(return_value) == 0) {
        fork_child = 1;
        /* Only this interpreter survives. Never invoke inherited server
         * handlers or write completion records into its parent's pipe. */
        if (signal_pipe[0] >= 0) close(signal_pipe[0]);
        if (signal_pipe[1] >= 0) close(signal_pipe[1]);
        signal_pipe[0] = signal_pipe[1] = -1; broker_started = false;
        contexts = local_context; local_context->next = NULL; local_context->count = 0;
        /* These anchors belong to the discarded host graph, not PHP streams.
         * signal_lock protects their descriptor lifetime across this fork.
         * Explicit close avoids taking the host-FD gate under signal_lock. */
#ifdef __linux__
        for (child_record_t *child = children; child; child = child->next)
            if (child->pidfd >= 0) close(child->pidfd);
#endif
        children = NULL;
        for (int sig = 1; sig < NSIG; ++sig) if (installed_actions[sig]) {
            struct sigaction action = {0}; sigemptyset(&action.sa_mask);
            zval *value = zend_hash_index_find(&local_context->globals->php_signal_table, (zend_ulong)sig);
            if (value && Z_TYPE_P(value) != IS_LONG) { action.sa_sigaction = child_signal; action.sa_flags = SA_SIGINFO | SA_ONSTACK; }
            else action.sa_handler = value && Z_TYPE_P(value) == IS_LONG ? (void (*)(int))Z_LVAL_P(value) : SIG_DFL;
            sigaction(sig, &action, NULL); installed_actions[sig] = false;
        }
        pthread_mutex_unlock(&signal_lock);
        hp_host_fds_fork_child(); hp_runtime_fork_child(); hp_sapi_fork_child(); free(record);
        return;
    }
    if (Z_TYPE_P(return_value) == IS_LONG && Z_LVAL_P(return_value) > 0) {
        record->pid = (pid_t)Z_LVAL_P(return_value); record->owner = local_context->id;
        record->serial = next_child_serial++;
        record->pgid = getpgrp();
#ifdef __linux__
        anchor_child(record);
#endif
        record->next = children; children = record; record = NULL;
    }
    pthread_mutex_unlock(&signal_lock);
    hp_host_fds_fork_parent(); hp_sapi_fork_parent(); hp_runtime_fork_parent(); free(record);
}
static bool child_matches(child_record_t *child, pid_t selector) {
    if (selector == -1) return true;
    if (selector > 0) return child->pid == selector;
    pid_t group = selector == 0 ? getpgrp() : (pid_t)(-(int64_t)selector);
    if (!child->exited) { pid_t current = getpgid(child->pid); if (current >= 0) child->pgid = current; }
    return child->pgid == group;
}
static pid_t wait_owned(pid_t selector, int *status, int options, struct rusage *usage) {
    pcntl_context_t *c = local_context;
    if (!c) { errno = ECHILD; return -1; }
    /* Validate even cached/no-match waits without consuming another context's
     * child: the serving process cannot be its own child. */
    int validation_status = 0;
    if (waitpid(getpid(), &validation_status, options | WNOHANG) < 0 && errno != ECHILD) return -1;
#if defined(__WCLONE) && defined(__WALL)
    /* Every tracked child is an ordinary SIGCHLD fork. A clone-only selection
     * matches none; retain both live records and broker-cached exit status. */
    if ((options & __WCLONE) && !(options & __WALL)) { errno = ECHILD; return -1; }
#endif
    for (;;) {
        pthread_mutex_lock(&signal_lock);
        bool found = false;
        for (child_record_t **link = &children; *link;) {
            child_record_t *child = *link;
            if (child->owner != c->id || child->orphan || !child_matches(child, selector)) { link = &child->next; continue; }
            pid_t pid;
            if (child->exited) { pid = child->pid; *status = child->status; if (usage) *usage = child->usage; }
            else pid = usage ? wait4(child->pid, status, options | WNOHANG, usage) : waitpid(child->pid, status, options | WNOHANG);
            if (pid > 0) {
                if (child->exited || WIFEXITED(*status) || WIFSIGNALED(*status)) { *link = child->next; free_child(child); }
                pthread_mutex_unlock(&signal_lock); return pid;
            }
            if (pid < 0 && errno == ECHILD) { *link = child->next; free_child(child); continue; }
            if (pid < 0) { int saved = errno; pthread_mutex_unlock(&signal_lock); errno = saved; return -1; }
            found = true; link = &child->next;
        }
        pthread_mutex_unlock(&signal_lock);
        if (!found) { errno = ECHILD; return -1; }
        if (options & WNOHANG) return 0;
        pthread_mutex_lock(&signal_lock);
        struct timespec until; clock_gettime(CLOCK_MONOTONIC, &until); until.tv_nsec += 100000000;
        if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&c->changed, &signal_lock, &until);
        pthread_mutex_unlock(&signal_lock);
        drain_pending();
        if (zend_atomic_bool_load(&EG(vm_interrupt))) { errno = EINTR; return -1; }
    }
}
static void fill_usage(zval *value, const struct rusage *u) {
    if (!value) return;
    value = zend_try_array_init(value); if (!value) return;
    add_assoc_long(value, "ru_utime.tv_sec", u->ru_utime.tv_sec); add_assoc_long(value, "ru_utime.tv_usec", u->ru_utime.tv_usec);
    add_assoc_long(value, "ru_stime.tv_sec", u->ru_stime.tv_sec); add_assoc_long(value, "ru_stime.tv_usec", u->ru_stime.tv_usec);
    add_assoc_long(value, "ru_maxrss", u->ru_maxrss); add_assoc_long(value, "ru_minflt", u->ru_minflt);
    add_assoc_long(value, "ru_majflt", u->ru_majflt); add_assoc_long(value, "ru_nvcsw", u->ru_nvcsw); add_assoc_long(value, "ru_nivcsw", u->ru_nivcsw);
    add_assoc_long(value, "ru_oublock", u->ru_oublock); add_assoc_long(value, "ru_inblock", u->ru_inblock);
    add_assoc_long(value, "ru_msgsnd", u->ru_msgsnd); add_assoc_long(value, "ru_msgrcv", u->ru_msgrcv);
    add_assoc_long(value, "ru_ixrss", u->ru_ixrss); add_assoc_long(value, "ru_idrss", u->ru_idrss);
    add_assoc_long(value, "ru_nsignals", u->ru_nsignals); add_assoc_long(value, "ru_nswap", u->ru_nswap);
}
static PHP_FUNCTION(hp_waitpid) {
    if (fork_child) { native_waitpid(INTERNAL_FUNCTION_PARAM_PASSTHRU); return; }
    zend_long pid, options = 0; zval *status_value, *usage_value = NULL;
    ZEND_PARSE_PARAMETERS_START(2, 4)
        Z_PARAM_LONG(pid) Z_PARAM_ZVAL(status_value) Z_PARAM_OPTIONAL Z_PARAM_LONG(options) Z_PARAM_ZVAL(usage_value)
    ZEND_PARSE_PARAMETERS_END();
    pid_t selector = (pid_t)pid;
    if (selector > 0) {
        bool tracked = false;
        pthread_mutex_lock(&signal_lock);
        for (child_record_t *child = children; child; child = child->next)
            if (child->pid == selector) { tracked = true; break; }
        pthread_mutex_unlock(&signal_lock);
        /* Only untracked children (e.g. proc_open) use the process-wide native
         * wait. A known PID must keep its PHP context's ownership filter. */
        if (!tracked) { native_waitpid(INTERNAL_FUNCTION_PARAM_PASSTHRU); return; }
    }
    int status = (int)zval_get_long(status_value); struct rusage usage = {0};
    pid_t result = wait_owned(selector, &status, (int)options, usage_value ? &usage : NULL);
    if (result < 0 && local_context) local_context->globals->last_error = errno;
    ZEND_TRY_ASSIGN_REF_LONG(status_value, status); if (result > 0) fill_usage(usage_value, &usage);
    RETURN_LONG(result);
}
static PHP_FUNCTION(hp_wait) {
    if (fork_child) { native_wait(INTERNAL_FUNCTION_PARAM_PASSTHRU); return; }
    zend_long options = 0; zval *status_value, *usage_value = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_ZVAL(status_value) Z_PARAM_OPTIONAL Z_PARAM_LONG(options) Z_PARAM_ZVAL(usage_value)
    ZEND_PARSE_PARAMETERS_END();
    int status = (int)zval_get_long(status_value); struct rusage usage = {0};
    pid_t result = wait_owned(-1, &status, (int)options, usage_value ? &usage : NULL);
    if (result < 0 && local_context) local_context->globals->last_error = errno;
    ZEND_TRY_ASSIGN_REF_LONG(status_value, status); if (result > 0) fill_usage(usage_value, &usage);
    RETURN_LONG(result);
}
static bool signal_set(HashTable *array, sigset_t *set) {
    sigemptyset(set);
    if (!zend_hash_num_elements(array)) { zend_value_error("signal list must not be empty"); return false; }
    zval *value;
    ZEND_HASH_FOREACH_VAL(array, value) {
        zend_long sig = zval_get_long(value);
        if (sig < 1 || sig >= NSIG || reserved_signal((int)sig) || sigaddset(set, (int)sig)) {
            zend_value_error("signal list contains an invalid or reserved signal"); return false;
        }
    } ZEND_HASH_FOREACH_END();
    return true;
}
static void fill_siginfo(zval *value, const siginfo_t *info) {
    if (!value) return;
    value = zend_try_array_init(value); if (!value) return;
    add_assoc_long(value, "signo", info->si_signo); add_assoc_long(value, "errno", info->si_errno); add_assoc_long(value, "code", info->si_code);
    if (info->si_signo == SIGCHLD || info->si_signo == SIGUSR1 || info->si_signo == SIGUSR2 || info->si_signo >= SIGRTMIN) {
        add_assoc_long(value, "pid", info->si_pid); add_assoc_long(value, "uid", info->si_uid);
    }
    if (info->si_signo == SIGCHLD) { add_assoc_long(value, "status", info->si_status); add_assoc_double(value, "utime", info->si_utime); add_assoc_double(value, "stime", info->si_stime); }
#ifdef SIGPOLL
    if (info->si_signo == SIGPOLL) { add_assoc_long(value, "band", info->si_band); add_assoc_long(value, "fd", info->si_fd); }
#endif
}
static void child_exit_info(const child_record_t *child, siginfo_t *info) {
    memset(info, 0, sizeof(*info));
    info->si_signo = SIGCHLD; info->si_pid = child->pid; info->si_uid = getuid();
    if (WIFEXITED(child->status)) { info->si_code = CLD_EXITED; info->si_status = WEXITSTATUS(child->status); }
    else {
        info->si_code = WCOREDUMP(child->status) ? CLD_DUMPED : CLD_KILLED;
        info->si_status = WTERMSIG(child->status);
    }
    long ticks = sysconf(_SC_CLK_TCK);
    if (ticks > 0) {
        info->si_utime = child->usage.ru_utime.tv_sec * ticks + child->usage.ru_utime.tv_usec * ticks / 1000000;
        info->si_stime = child->usage.ru_stime.tv_sec * ticks + child->usage.ru_stime.tv_usec * ticks / 1000000;
    }
}
#ifdef __linux__
static int wait_pidfd(int fd, int options, siginfo_t *info, struct rusage *usage) {
    int descriptor_flags = fcntl(fd, F_GETFL);
    if (descriptor_flags < 0) return -1;
    pthread_mutex_lock(&signal_lock);
    /* Validate the actual syscall-width flags and descriptor without consuming
     * status. ECHILD is expected for a valid anchor already reaped by the broker.
     * Do not use these added validation flags for the caller's actual wait. */
    siginfo_t probe = {0};
    int valid = kernel_waitid(HP_P_PIDFD, (id_t)fd, &probe, options | WNOHANG | WNOWAIT, NULL);
    if (valid < 0 && errno != ECHILD) {
        int saved = errno; pthread_mutex_unlock(&signal_lock); errno = saved; return -1;
    }
    child_record_t *selected = pidfd_child(fd);
    if (!selected) {
        pthread_mutex_unlock(&signal_lock);
        /* Legitimate untracked children, including proc_open, retain the real
         * kernel's blocking, O_NONBLOCK, flag and error behavior on the pinned FD. */
        return kernel_waitid(HP_P_PIDFD, (id_t)fd, info, options, usage);
    }
    pcntl_context_t *c = local_context;
    if (!c || selected->owner != c->id || selected->orphan) {
        pthread_mutex_unlock(&signal_lock); errno = ECHILD; return -1;
    }
#if defined(__WCLONE) && defined(__WALL)
    if ((options & __WCLONE) && !(options & __WALL)) {
        pthread_mutex_unlock(&signal_lock); errno = ECHILD; return -1;
    }
#endif
    uint64_t serial = selected->serial;
    for (;;) {
        child_record_t **link = &children;
        while (*link && (*link)->serial != serial) link = &(*link)->next;
        child_record_t *child = *link;
        if (!child || child->owner != c->id || child->orphan) {
            pthread_mutex_unlock(&signal_lock); errno = ECHILD; return -1;
        }
        int result;
        if (child->exited) {
            if (!(options & WEXITED)) { pthread_mutex_unlock(&signal_lock); errno = ECHILD; return -1; }
            child_exit_info(child, info); if (usage) *usage = child->usage; result = 0;
        } else result = kernel_waitid(HP_P_PIDFD, (id_t)fd, info, options | WNOHANG, usage);
        if (!result && info->si_pid) {
            if (!(options & WNOWAIT) && (info->si_code == CLD_EXITED || info->si_code == CLD_KILLED || info->si_code == CLD_DUMPED)) {
                *link = child->next; free_child(child);
            }
            pthread_mutex_unlock(&signal_lock); return 0;
        }
        if (result < 0) {
            int saved = errno;
            if (saved == ECHILD) { *link = child->next; free_child(child); }
            pthread_mutex_unlock(&signal_lock); errno = saved; return -1;
        }
        pthread_mutex_unlock(&signal_lock);
        if (options & WNOHANG) return 0;
        if (descriptor_flags & O_NONBLOCK) { errno = EAGAIN; return -1; }
        pthread_mutex_lock(&signal_lock);
        struct timespec until; clock_gettime(CLOCK_MONOTONIC, &until); until.tv_nsec += 100000000;
        if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&c->changed, &signal_lock, &until);
        pthread_mutex_unlock(&signal_lock);
        drain_pending();
        if (zend_atomic_bool_load(&EG(vm_interrupt))) { errno = EINTR; return -1; }
        pthread_mutex_lock(&signal_lock);
    }
}
#endif
static PHP_FUNCTION(hp_waitid) {
    if (fork_child) { native_waitid(INTERNAL_FUNCTION_PARAM_PASSTHRU); return; }
    zend_long kind = P_ALL, id = 0, options = WEXITED; bool id_null = true;
    zval *info_value = NULL, *usage_value = NULL;
    ZEND_PARSE_PARAMETERS_START(0, 5)
        Z_PARAM_OPTIONAL Z_PARAM_LONG(kind) Z_PARAM_LONG_OR_NULL(id, id_null)
        Z_PARAM_ZVAL(info_value) Z_PARAM_LONG(options) Z_PARAM_ZVAL(usage_value)
    ZEND_PARSE_PARAMETERS_END();
    idtype_t selected_kind = (idtype_t)kind;
    pid_t selected_id = (pid_t)(id_t)id;
    pcntl_context_t *c = local_context;
    siginfo_t info = {0}; struct rusage usage = {0}; int result = -1;
#ifdef __linux__
    if (selected_kind == HP_P_PIDFD) {
        /* Pin the open file description before inspecting it. Closing/reusing
         * the caller's numeric FD cannot change the selected process mid-wait. */
        hp_host_fds_begin();
        int fd = fcntl((int)(id_t)id, F_DUPFD_CLOEXEC, 0);
        if (fd >= 0) hp_host_fd_register_locked(fd);
        int pin_error = errno; hp_host_fds_end(); errno = pin_error;
        if (fd >= 0) {
            zend_try {
                result = wait_pidfd(fd, (int)options, &info, usage_value ? &usage : NULL);
            } zend_catch {
                hp_host_fd_close(fd); zend_bailout();
            } zend_end_try();
            int saved = errno; hp_host_fd_close(fd); errno = saved;
        } else {
            /* Linux validates wait flags before resolving the PIDFD. Preserve
             * that precedence without ever retrying the caller's reusable FD.
             * The retained signal pipe is a valid non-PIDFD until PHP stops;
             * unlike -1, it yields EBADF rather than EINVAL for valid flags. */
            int saved = errno; siginfo_t probe = {0};
            pthread_mutex_lock(&signal_lock);
            if ((int)(id_t)id < 0) {
                /* Negative PIDFD IDs have native EINVAL semantics, unlike
                 * fcntl's EBADF. A fixed negative control cannot name an FD. */
                (void)kernel_waitid(HP_P_PIDFD, (id_t)-1, &probe, (int)options | WNOHANG | WNOWAIT, NULL);
                saved = errno;
            } else if (signal_pipe[0] >= 0 && kernel_waitid(HP_P_PIDFD, (id_t)signal_pipe[0], &probe,
                    (int)options | WNOHANG | WNOWAIT, NULL) < 0 && errno == EINVAL)
                saved = EINVAL;
            pthread_mutex_unlock(&signal_lock);
            errno = saved;
        }
        goto Finished;
    }
#endif
    if (selected_kind != P_ALL && selected_kind != P_PID && selected_kind != P_PGID) {
        native_waitid(INTERNAL_FUNCTION_PARAM_PASSTHRU); return;
    }
    if (!c) { errno = ECHILD; RETURN_FALSE; }
    if (selected_kind == P_PGID && selected_id < 0) {
        c->globals->last_error = EINVAL; RETURN_FALSE;
    }
    if (selected_kind == P_PID) {
        bool tracked = false;
        pthread_mutex_lock(&signal_lock);
        for (child_record_t *child = children; child; child = child->next)
            if (child->pid == selected_id) { tracked = true; break; }
        pthread_mutex_unlock(&signal_lock);
        if (!tracked) { native_waitid(INTERNAL_FUNCTION_PARAM_PASSTHRU); return; }
    }
    if (!(options & (WEXITED | WSTOPPED | WCONTINUED)) || (options & ~(WEXITED | WSTOPPED | WCONTINUED | WNOHANG | WNOWAIT))) {
        c->globals->last_error = EINVAL; RETURN_FALSE;
    }
    for (;;) {
        pthread_mutex_lock(&signal_lock);
        bool found = false;
        for (child_record_t **link = &children; *link;) {
            child_record_t *child = *link;
            if (child->owner != c->id || child->orphan ||
                (selected_kind == P_PID && child->pid != selected_id) ||
                (selected_kind == P_PGID && !child_matches(child, selected_id ? (pid_t)-(int64_t)selected_id : 0)) ||
                (child->exited && !(options & WEXITED))) { link = &child->next; continue; }
            if (child->exited) { child_exit_info(child, &info); usage = child->usage; result = 0; }
            else {
#ifdef SYS_waitid
                result = (int)syscall(SYS_waitid, P_PID, (id_t)child->pid, &info, (int)options | WNOHANG, usage_value ? &usage : NULL);
#else
                result = waitid(P_PID, (id_t)child->pid, &info, (int)options | WNOHANG);
#endif
            }
            if (!result && info.si_pid) {
                if (!(options & WNOWAIT) && (info.si_code == CLD_EXITED || info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED)) { *link = child->next; free_child(child); }
                pthread_mutex_unlock(&signal_lock);
                goto Finished;
            }
            if (result < 0 && errno == ECHILD) { *link = child->next; free_child(child); continue; }
            if (result < 0) { int saved = errno; pthread_mutex_unlock(&signal_lock); errno = saved; goto Finished; }
            found = true; link = &child->next;
        }
        pthread_mutex_unlock(&signal_lock);
        if (!found) { result = -1; errno = ECHILD; break; }
        if (options & WNOHANG) { result = 0; break; }
        pthread_mutex_lock(&signal_lock);
        struct timespec until; clock_gettime(CLOCK_MONOTONIC, &until); until.tv_nsec += 100000000;
        if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&c->changed, &signal_lock, &until);
        pthread_mutex_unlock(&signal_lock);
        drain_pending();
        if (zend_atomic_bool_load(&EG(vm_interrupt))) { result = -1; errno = EINTR; break; }
    }
Finished:
    if (result < 0) {
        pcntl_globals_t *globals = c ? c->globals : get_globals();
        if (globals) globals->last_error = errno;
        RETURN_FALSE;
    }
    /* Native waitid includes the SIGCHLD fields even for a WNOHANG miss. */
    if (!info.si_signo) info.si_signo = SIGCHLD;
    fill_siginfo(info_value, &info); fill_usage(usage_value, &usage); RETURN_TRUE;
}
static void wait_signal(INTERNAL_FUNCTION_PARAMETERS, bool timed) {
    HashTable *array; zval *info_value = NULL; zend_long seconds = 0, nanos = 0;
    ZEND_PARSE_PARAMETERS_START(1, timed ? 4 : 2)
        Z_PARAM_ARRAY_HT(array) Z_PARAM_OPTIONAL Z_PARAM_ZVAL(info_value) Z_PARAM_LONG(seconds) Z_PARAM_LONG(nanos)
    ZEND_PARSE_PARAMETERS_END();
    if (seconds < 0 || nanos < 0 || nanos >= 1000000000 || (timed && !seconds && !nanos)) {
        zend_value_error("invalid or zero signal wait timeout"); RETURN_THROWS();
    }
    hp_pcntl_context_enter(); pcntl_context_t *c = local_context;
    if (!c) RETURN_FALSE;
    sigset_t set; if (!signal_set(array, &set)) RETURN_THROWS();
    struct timespec until; clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += seconds; until.tv_nsec += nanos;
    if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
    signal_event_t event = {0}; int rc = 0;
    pthread_mutex_lock(&signal_lock); c->waiting = set;
    for (int sig = 1; sig < NSIG; ++sig) if (sigismember(&set, sig) == 1) (void)install_signal(sig);
    for (;;) {
        for (unsigned i = 0; i < c->count; ++i) if (sigismember(&set, c->events[i].signo) == 1) {
            event = c->events[i]; memmove(c->events + i, c->events + i + 1, (--c->count - i) * sizeof(event)); break;
        }
        if (event.signo || (timed && rc == ETIMEDOUT) || atomic_load(&broker_stopping) || zend_atomic_bool_load(&EG(timed_out))) break;
        if (timed) rc = pthread_cond_timedwait(&c->changed, &signal_lock, &until);
        else {
            struct timespec tick; clock_gettime(CLOCK_MONOTONIC, &tick); tick.tv_nsec += 100000000;
            if (tick.tv_nsec >= 1000000000) { ++tick.tv_sec; tick.tv_nsec -= 1000000000; }
            rc = pthread_cond_timedwait(&c->changed, &signal_lock, &tick);
        }
    }
    sigemptyset(&c->waiting); pthread_mutex_unlock(&signal_lock);
    if (!event.signo) { errno = zend_atomic_bool_load(&EG(timed_out)) ? EINTR : EAGAIN; RETURN_FALSE; }
    fill_siginfo(info_value, &event.info); RETURN_LONG(event.signo);
}
static PHP_FUNCTION(hp_sigwait) {
    if (fork_child) { native_sigwait(INTERNAL_FUNCTION_PARAM_PASSTHRU); return; }
    wait_signal(INTERNAL_FUNCTION_PARAM_PASSTHRU, false);
}
static PHP_FUNCTION(hp_sigtimedwait) {
    if (fork_child) { native_sigtimedwait(INTERNAL_FUNCTION_PARAM_PASSTHRU); return; }
    wait_signal(INTERNAL_FUNCTION_PARAM_PASSTHRU, true);
}
static void replace_handler(const char *name, zif_handler replacement, zif_handler *previous) {
    zend_function *fn = zend_hash_str_find_ptr(CG(function_table), name, strlen(name));
    if (fn && fn->type == ZEND_INTERNAL_FUNCTION && fn->internal_function.handler != replacement) {
        *previous = fn->internal_function.handler; fn->internal_function.handler = replacement;
    }
}
int hp_pcntl_install(void) {
    pcntl_module = zend_hash_str_find_ptr(&module_registry, "pcntl", sizeof("pcntl") - 1);
    if (!pcntl_module || !pcntl_module->globals_id_ptr || pcntl_module->globals_size != sizeof(pcntl_globals_t)) {
        hp_log_message("php.pcntl", "PCNTL is missing or its globals ABI is incompatible with the embedding adapter", 8, 0); return -1;
    }
    replace_handler("pcntl_signal", ZEND_FN(hp_signal), &native_signal);
    replace_handler("pcntl_signal_dispatch", ZEND_FN(hp_dispatch), &native_dispatch);
    replace_handler("pcntl_sigprocmask", ZEND_FN(hp_mask), &native_mask);
    replace_handler("pcntl_fork", ZEND_FN(hp_fork), &native_fork);
    replace_handler("pcntl_waitpid", ZEND_FN(hp_waitpid), &native_waitpid);
    replace_handler("pcntl_wait", ZEND_FN(hp_wait), &native_wait);
    replace_handler("pcntl_waitid", ZEND_FN(hp_waitid), &native_waitid);
    replace_handler("pcntl_alarm", ZEND_FN(hp_alarm), &native_alarm);
    replace_handler("pcntl_sigwaitinfo", ZEND_FN(hp_sigwait), &native_sigwait);
    replace_handler("pcntl_sigtimedwait", ZEND_FN(hp_sigtimedwait), &native_sigtimedwait);
    if (pcntl_module->request_shutdown_func != adapted_rshutdown) {
        native_rshutdown = pcntl_module->request_shutdown_func; pcntl_module->request_shutdown_func = adapted_rshutdown;
    }
    return 0;
}
void hp_pcntl_finalize(void) {
    /* All extension MINIT hooks have run. Install once so another extension
     * cannot capture this wrapper and form a recursive interrupt chain. */
    if (zend_interrupt_function != broker_interrupt) {
        native_interrupt = zend_interrupt_function;
        zend_interrupt_function = broker_interrupt;
    }
}
