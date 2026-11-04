#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "h2o_php.h"
#include "cpu_budget.h"
#include "h2o/memory.h"
#include "h2o/serverutil.h"
#include "../observability/log.h"
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/sysinfo.h>
#endif
#include <time.h>
#include <unistd.h>

hp_runtime_config_t hp_config;
typedef struct hp_worker_thread {
    hp_worker_group_t *group;
    pthread_t thread;
    pthread_cond_t available;
    /* All lifecycle fields except the atomic SAPI predicates use lock. */
    bool occupied, running, assigned, minimum, starting, bootstrapping;
    bool waiting, in_php, parked, replacement;
    atomic_bool deactivating, bootstrap_timed_out;
    uint64_t idle_since_ns, bootstrap_deadline_ns, next_interrupt_ns, retry_after_ns;
    uint64_t failures; /* Go int64 bits; unsigned arithmetic defines wrap. */
} hp_worker_thread_t;
struct hp_worker_group {
    hp_worker_config_t config;
    hp_php_metric_worker_t *metrics;
    hp_worker_group_t *next;
    hp_request_t *head, *tail;
    atomic_uint generation;
    uint64_t watch_hash;
    unsigned started;
    size_t queued_count;
    atomic_uint live, ready, busy, failed_threads;
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t monitor_cond;
static bool monitor_cond_initialized;
static hp_worker_group_t *groups;
static hp_request_t *classic_head, *classic_tail, *active;
static pthread_t controller;
static hp_worker_thread_t *thread_slots;
static unsigned nclassic, alive, minimum_threads, scale_cursor;
static size_t queued, classic_queued;
static int start_state;
static bool defaults_initialized, draining, stop_requested, controller_created, startup_failed, watcher_enabled;
static atomic_bool controller_stopped, fork_child_detached;
static bool fork_gate;
static void *fork_owner;
static uint64_t drain_started_ns, next_queue_deadline_ns = UINT64_MAX;
static _Thread_local hp_worker_group_t *local_group;
static _Thread_local hp_worker_thread_t *local_worker;
static _Thread_local unsigned local_generation, local_requests;
static _Thread_local bool local_ready, local_busy, local_bootstrapped;
static atomic_uint_fast64_t completed, failed, timed_out, restarts, startup_failures, queue_ns, execution_ns;
static atomic_uint_fast64_t scaled_up, idle_retired, reactivated, thread_reboots, unhealthy_reboots;
static atomic_uint active_count, ready_workers;
static double (*cpu_sampler)(uint64_t, void *);
static void *cpu_sampler_arg;
static bool cpu_probe_active;
static hp_worker_group_t *cpu_probe_group;
static uint64_t cpu_probe_started, cpu_probe_cpu_ns;
static double last_cpu_usage;
static atomic_uint_fast64_t cpu_scale_rejections;

static void wake_monitor(void) {
    if (monitor_cond_initialized) pthread_cond_broadcast(&monitor_cond);
}
/* The controller does not need ordinary request progress once startup has
 * settled. Fork rendezvous uses the same condition, so boundary changes must
 * still broadcast while that gate is held; shutdown also observes progress. */
static void wake_monitor_at_boundary(void) {
    if (start_state != 2 || fork_gate || draining) wake_monitor();
}
static uint64_t deadline_after_ms(uint64_t started, uint64_t ms) {
    return ms > (UINT64_MAX - started) / 1000000ULL ? UINT64_MAX : started + ms * 1000000ULL;
}
static uint64_t queue_deadline(hp_request_t *r) {
    uint64_t deadline = hp_config.max_wait_ms ? deadline_after_ms(r->queued_ns, hp_config.max_wait_ms) : UINT64_MAX;
    return deadline < r->deadline_ns ? deadline : r->deadline_ns;
}
static void wait_for_monitor(uint64_t deadline) {
    struct timespec until = {(time_t)(deadline / 1000000000ULL), (long)(deadline % 1000000000ULL)};
    pthread_cond_timedwait(&monitor_cond, &lock, &until);
}
static void wake_slots(void) {
    if (thread_slots) for (unsigned i = 0; i < hp_config.max_threads; ++i)
        if (thread_slots[i].running) pthread_cond_signal(&thread_slots[i].available);
    pthread_cond_broadcast(&cond); wake_monitor();
}
void hp_runtime_config_defaults(void) {
    if (defaults_initialized) return;
    defaults_initialized = true;
    hp_config = (hp_runtime_config_t){ .queue_limit = 0,
        .max_buffered_body = 1048576, .max_buffered_response = 262144, .max_buffered_global = 268435456,
        .shutdown_timeout_ms = 30000, .max_idle_ms = 5000, .startup_timeout_ms = 30000 };
}
hp_worker_group_t *hp_worker_group_create(const hp_worker_config_t *c) {
    unsigned num = c->num;
    if (!num) {
        /* PHP defaults follow the captured stable startup CPU policy;
         * H2O network-loop defaults keep their separate CPU interpretation. */
        unsigned cpus;
        if (!hp_cpu_budget_default_count_checked(&cpus) || cpus > (unsigned)(INT_MAX - 2) / 2) return NULL;
        num = (unsigned)cpus * 2;
    }
    hp_worker_group_t *g = calloc(1, sizeof(*g)); if (!g) return NULL;
    g->config = *c;
    g->config.file = strdup(c->file); g->config.name = strdup(c->name ? c->name : c->file);
    g->config.metrics_name = strdup(c->metrics_name ? c->metrics_name : (c->name ? c->name : c->file));
    g->config.root = c->root ? strdup(c->root) : NULL; g->config.env = hp_headers_clone(c->env);
    g->config.num = num;
    g->config.watch = calloc(c->num_watch, sizeof(char *));
    if (!g->config.file || !g->config.name || !g->config.metrics_name || (c->root && !g->config.root) ||
        (c->env && !g->config.env) || (c->num_watch && !g->config.watch)) goto Error;
    for (size_t i = 0; i < c->num_watch; ++i) {
        if (c->watch[i][0] == '/') g->config.watch[i] = strdup(c->watch[i]);
        else { char cwd[PATH_MAX]; if (!getcwd(cwd, sizeof(cwd)) || asprintf(&g->config.watch[i], "%s/%s", cwd, c->watch[i]) < 0) goto Error; }
        if (!g->config.watch[i]) goto Error;
    }
    atomic_init(&g->generation, 1); atomic_init(&g->live, 0); atomic_init(&g->ready, 0);
    atomic_init(&g->busy, 0); atomic_init(&g->failed_threads, 0);
    g->metrics = hp_php_metrics_worker_register(g->config.metrics_name);
    if (!g->metrics) goto Error;
    g->next = groups; groups = g; return g;
Error:
    free(g->config.file); free(g->config.name); free(g->config.metrics_name); free(g->config.root); hp_headers_free(g->config.env);
    if (g->config.watch) for (size_t i = 0; i < c->num_watch; ++i) free(g->config.watch[i]);
    free(g->config.watch); free(g); return NULL;
}
const hp_worker_config_t *hp_worker_group_config(hp_worker_group_t *g) { return &g->config; }
void hp_runtime_set_cpu_sampler(double (*sampler)(uint64_t, void *), void *arg) {
    pthread_mutex_lock(&lock);
    if (!start_state && !controller_created) { cpu_sampler = sampler; cpu_sampler_arg = arg; }
    pthread_mutex_unlock(&lock);
}
bool hp_worker_should_restart(hp_worker_group_t *g) {
    if (atomic_load(&fork_child_detached)) return true;
    return atomic_load(&local_worker->deactivating) || atomic_load(&local_worker->bootstrap_timed_out) ||
        atomic_load(&g->generation) != local_generation ||
        (g->config.max_requests && local_requests >= g->config.max_requests);
}
void hp_worker_ready(hp_worker_group_t *g) {
    if (atomic_load(&fork_child_detached)) return;
    if (local_ready) return;
    pthread_mutex_lock(&lock);
    if (local_worker->bootstrapping && (hp_now_ns() >= local_worker->bootstrap_deadline_ns ||
                                        atomic_load(&local_worker->bootstrap_timed_out))) {
        atomic_store(&local_worker->bootstrap_timed_out, true);
        hp_sapi_interrupt_worker(local_worker); wake_monitor(); pthread_mutex_unlock(&lock); return;
    }
    bool bootstrap_completed = local_worker->bootstrapping || local_worker->starting;
    local_worker->bootstrapping = local_worker->starting = false;
    if (!local_bootstrapped) hp_php_metrics_worker_ready(g->metrics);
    local_bootstrapped = local_ready = true; local_worker->failures = 0;
    atomic_fetch_add(&ready_workers, 1); atomic_fetch_add(&g->ready, 1);
    if (start_state == 1) pthread_cond_broadcast(&cond);
    if (bootstrap_completed) wake_monitor(); else wake_monitor_at_boundary();
    pthread_mutex_unlock(&lock);
}
static void clear_ready(void) {
    if (local_ready) {
        local_ready = false; atomic_fetch_sub(&ready_workers, 1);
        if (local_group) atomic_fetch_sub(&local_group->ready, 1);
    }
}
static void activate(hp_request_t *r) {
    r->active_next = active; active = r; atomic_fetch_add(&active_count, 1);
}
static bool dispatch_worker(hp_worker_group_t *g) {
    for (unsigned i = 0; i < hp_config.max_threads; ++i) {
        hp_worker_thread_t *slot = &thread_slots[i];
        if (slot->running && slot->assigned && slot->group == g && slot->waiting && !atomic_load(&slot->deactivating)) {
            slot->waiting = false; pthread_cond_signal(&slot->available); return true;
        }
    }
    return false;
}
static void wake_workers(hp_worker_group_t *g) {
    if (thread_slots) for (unsigned i = 0; i < hp_config.max_threads; ++i)
        if (thread_slots[i].running && thread_slots[i].assigned && thread_slots[i].group == g)
            pthread_cond_signal(&thread_slots[i].available);
}
/* Enter/leave a known PHP boundary. A worker waiting in take() is quiescent;
 * no Zend stack moves when a slot changes assignments. */
static bool php_enter(bool shutting_down) {
    pthread_mutex_lock(&lock);
    while (fork_gate && fork_owner != local_worker) {
        local_worker->parked = true; wake_monitor();
        pthread_cond_wait(&local_worker->available, &lock);
    }
    bool allowed = shutting_down || (!draining && !startup_failed);
    local_worker->parked = false; local_worker->in_php = allowed;
    wake_monitor_at_boundary(); pthread_mutex_unlock(&lock); return allowed;
}
static void php_leave(void) {
    pthread_mutex_lock(&lock); local_worker->in_php = false; local_worker->parked = true;
    wake_monitor_at_boundary(); pthread_mutex_unlock(&lock);
}
static hp_request_t *take(hp_worker_group_t *g) {
Retry:
    pthread_mutex_lock(&lock);
    hp_request_t **head = g ? &g->head : &classic_head, **tail = g ? &g->tail : &classic_tail;
    for (;;) {
        if (draining || atomic_load(&local_worker->deactivating) || (g && hp_worker_should_restart(g))) break;
        if (!fork_gate && *head) break;
        if (!local_worker->idle_since_ns) local_worker->idle_since_ns = hp_now_ns();
        local_worker->waiting = true; local_worker->parked = true; wake_monitor_at_boundary();
        pthread_cond_wait(&local_worker->available, &lock);
    }
    local_worker->waiting = false; local_worker->parked = false;
    if (draining || atomic_load(&local_worker->deactivating) || (g && hp_worker_should_restart(g)) || !*head) {
        pthread_mutex_unlock(&lock); return NULL;
    }
    local_worker->idle_since_ns = 0;
    hp_request_t *r = *head; *head = r->queue_next; if (!*head) *tail = NULL;
    r->queue_next = NULL; --queued; if (g) --g->queued_count; else --classic_queued;
    activate(r);
    /* Publish handoff under L -> R before cancellation can mistake a removed
     * request for a still-queued context. No callback takes R -> L. */
    pthread_mutex_lock(&r->mutex);
    hp_php_metrics_request_dispatched_locked(&r->php_metrics);
    wake_monitor_at_boundary(); pthread_mutex_unlock(&lock);
    r->started_ns = hp_now_ns(); if (!r->cancelled) r->state = HP_RUNNING;
    if (r->started_ns >= queue_deadline(r)) {
        r->state = HP_TIMED_OUT; r->cancelled = true; r->status = 503; r->headers_ready = r->response_finished = true;
    }
    bool cancelled = r->cancelled; pthread_mutex_unlock(&r->mutex);
    atomic_fetch_add(&queue_ns, r->started_ns - r->queued_ns);
    if (cancelled) { hp_response_finish(r); hp_runtime_request_done(r); goto Retry; }
    return r;
}
hp_request_t *hp_worker_next(hp_worker_group_t *g) {
    if (atomic_load(&fork_child_detached)) return NULL;
    hp_worker_ready(g); hp_request_t *r = take(g); clear_ready();
    if (r) { local_busy = true; atomic_fetch_add(&g->busy, 1); }
    return r;
}
void hp_runtime_request_done(hp_request_t *r) {
    if (atomic_load(&fork_child_detached)) return;
    hp_response_finish(r);
    pthread_mutex_lock(&r->mutex);
    bool bad = r->cancelled || r->status >= 500, timeout = r->state == HP_TIMED_OUT;
    if (!r->cancelled) r->state = HP_COMPLETED;
    pthread_mutex_unlock(&r->mutex);
    if (bad) atomic_fetch_add(&failed, 1);
    if (timeout) atomic_fetch_add(&timed_out, 1);
    atomic_fetch_add(&completed, 1); atomic_fetch_add(&execution_ns, hp_now_ns() - r->started_ns);
    pthread_mutex_lock(&lock);
    hp_request_t **p = &active; while (*p && *p != r) p = &(*p)->active_next;
    if (*p) { *p = r->active_next; atomic_fetch_sub(&active_count, 1); }
    wake_monitor_at_boundary(); pthread_mutex_unlock(&lock);
    if (local_group && r->group == local_group) {
        ++local_requests; if (local_busy) { local_busy = false; atomic_fetch_sub(&local_group->busy, 1); }
    }
    hp_request_release(r);
}
static int64_t retry_signed_count(uint64_t bits) {
    return bits <= INT64_MAX ? (int64_t)bits : -1 - (int64_t)(UINT64_MAX - bits);
}
static uint64_t retry_delay_ns(uint64_t failures) {
    /* Go int64 arithmetic wraps; time.Sleep ignores nonpositive Duration.
     * Use modulo unsigned arithmetic, never C signed overflow or a cast of
     * an unrepresentable unsigned value to signed. Keep sub-ms residues. */
    uint64_t delay = failures * failures * UINT64_C(100000000);
    if (delay > INT64_MAX) return 0;
    return delay > UINT64_C(1000000000) ? UINT64_C(1000000000) : delay;
}
static uint64_t retry_deadline_ns(uint64_t now, uint64_t delay) {
    return delay > UINT64_MAX - now ? UINT64_MAX : now + delay;
}
static bool wait_restart(uint64_t delay, hp_worker_group_t *g) {
    uint64_t deadline = retry_deadline_ns(hp_now_ns(), delay);
    pthread_mutex_lock(&lock);
    while (!draining && !atomic_load(&local_worker->deactivating) && atomic_load(&g->generation) == local_generation) {
        if (hp_now_ns() >= deadline) break;
        local_worker->parked = true; wake_monitor_at_boundary();
        struct timespec until = {(time_t)(deadline / 1000000000ULL), (long)(deadline % 1000000000ULL)};
        pthread_cond_timedwait(&local_worker->available, &lock, &until);
    }
    bool stop = draining || atomic_load(&local_worker->deactivating);
    local_worker->parked = false;
    /* Generation wakes retain this worker handler's consecutive history. */
    pthread_mutex_unlock(&lock); return stop;
}
/* Returns true only when this OS thread must exit and be replaced. */
static bool run_worker(hp_worker_group_t *g) {
    for (;;) {
        if (!php_enter(false)) return false;
        pthread_mutex_lock(&lock);
        local_worker->bootstrapping = true; local_worker->idle_since_ns = 0;
        uint64_t timeout = g->config.startup_timeout_ms ? g->config.startup_timeout_ms : hp_config.startup_timeout_ms;
        local_worker->bootstrap_deadline_ns = deadline_after_ms(hp_now_ns(), timeout);
        local_worker->next_interrupt_ns = 0; atomic_store(&local_worker->bootstrap_timed_out, false);
        /* A restart may introduce a bootstrap deadline before housekeeping. */
        wake_monitor();
        pthread_mutex_unlock(&lock);
        local_generation = atomic_load(&g->generation); local_requests = 0; local_bootstrapped = false;
        hp_php_metrics_worker_start(g->metrics);
        hp_execution_result_t result = hp_sapi_worker(g, g->config.file);
        hp_php_metrics_worker_stop(g->metrics, !local_bootstrapped ? HP_PHP_METRIC_BOOT_FAILURE :
            (result.exit_status ? HP_PHP_METRIC_CRASH : HP_PHP_METRIC_RESTART));
        clear_ready(); php_leave();
        pthread_mutex_lock(&lock); local_worker->bootstrapping = false; wake_monitor();
        bool stop = draining || atomic_load(&local_worker->deactivating);
        bool bootstrap_timeout = atomic_load(&local_worker->bootstrap_timed_out);
        pthread_mutex_unlock(&lock);
        if (stop) return false;
        if (local_bootstrapped && result.exit_status != 0) {
            char message[96];
            int n = snprintf(message, sizeof(message), "worker script exited with status %d", result.exit_status);
            if (n > 0 && (size_t)n < sizeof(message))
                hp_log_message("php.worker", message, 4, 0);
        }
        atomic_fetch_add(&restarts, 1);
        if (!local_bootstrapped) {
            atomic_fetch_add(&startup_failures, 1);
            char record[224]; int n = snprintf(record, sizeof(record),
                "{\"component\":\"php.worker\",\"event\":\"startup_failed\",\"attempt\":%" PRId64 ",\"code\":%d}",
                retry_signed_count(local_worker->failures + UINT64_C(1)), bootstrap_timeout ? 255 : result.exit_status);
            hp_log_json(record, (size_t)n);
            pthread_mutex_lock(&lock);
            bool terminal = start_state == 1 && !watcher_enabled && g->config.max_consecutive_failures >= 0 &&
                retry_signed_count(local_worker->failures) >= g->config.max_consecutive_failures;
            if (terminal) { startup_failed = true; atomic_fetch_add(&g->failed_threads, 1); wake_monitor(); }
            uint64_t delay = retry_delay_ns(local_worker->failures);
            if (!terminal) ++local_worker->failures;
            pthread_mutex_unlock(&lock);
            if (terminal) return false;
            if (result.health == HP_THREAD_UNHEALTHY) {
                local_worker->retry_after_ns = retry_deadline_ns(hp_now_ns(), delay);
                atomic_fetch_add(&unhealthy_reboots, 1); return true;
            }
            if (wait_restart(delay, g)) return false;
        } else {
            local_worker->failures = 0;
            if (result.health == HP_THREAD_UNHEALTHY || (g->config.max_requests && local_requests >= g->config.max_requests)) {
                if (result.health == HP_THREAD_UNHEALTHY) atomic_fetch_add(&unhealthy_reboots, 1);
                return true;
            }
        }
    }
}
static void detach_slot(hp_worker_thread_t *slot) {
    if (!slot->assigned) return;
    if (slot->group) { --slot->group->started; atomic_fetch_sub(&slot->group->live, 1); }
    else --nclassic;
    slot->assigned = false; slot->group = NULL;
}
static void *execution_main(void *arg) {
    local_worker = arg;
    if (!php_enter(false)) goto EndNoTls;
    hp_sapi_thread_init(); hp_sapi_set_worker_token(local_worker); php_leave();
    pthread_mutex_lock(&lock); local_worker->starting = false; wake_monitor(); pthread_mutex_unlock(&lock);
    bool reboot = false;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!local_worker->assigned && !draining) {
            local_worker->parked = true; wake_monitor_at_boundary();
            pthread_cond_wait(&local_worker->available, &lock);
        }
        bool stop = draining; local_group = local_worker->group; local_worker->parked = false;
        pthread_mutex_unlock(&lock);
        if (stop) break;
        if (local_group) reboot = run_worker(local_group);
        else {
            unsigned requests = 0; hp_request_t *r;
            while ((r = take(NULL))) {
                if (!php_enter(false)) { hp_request_cancel(r); hp_runtime_request_done(r); break; }
                hp_execution_result_t result = hp_sapi_execute(r); php_leave();
                if (result.health == HP_THREAD_UNHEALTHY) hp_log_message("php.thread", "PHP lifecycle bailout; retiring execution thread", 8, r->id);
                hp_runtime_request_done(r);
                if (result.health == HP_THREAD_UNHEALTHY || (hp_config.max_requests && ++requests >= hp_config.max_requests)) {
                    reboot = true;
                    if (result.health == HP_THREAD_UNHEALTHY) atomic_fetch_add(&unhealthy_reboots, 1);
                    break;
                }
            }
        }
        if (reboot) break;
        pthread_mutex_lock(&lock);
        stop = draining || startup_failed;
        if (!stop && atomic_load(&local_worker->deactivating)) {
            detach_slot(local_worker); local_group = NULL;
            local_worker->waiting = false; local_worker->parked = true; local_worker->idle_since_ns = 0;
            atomic_store(&local_worker->deactivating, false);
            wake_monitor();
        }
        pthread_mutex_unlock(&lock);
        if (stop) break;
    }
    php_enter(true); hp_sapi_thread_shutdown(); php_leave();
    pthread_mutex_lock(&lock);
    local_worker->replacement = reboot && !draining;
    if (local_worker->replacement) atomic_fetch_add(&thread_reboots, 1);
    else detach_slot(local_worker);
    local_worker->running = local_worker->waiting = local_worker->starting = local_worker->bootstrapping = false;
    --alive; wake_monitor(); pthread_mutex_unlock(&lock); return NULL;
EndNoTls:
    pthread_mutex_lock(&lock); detach_slot(local_worker); local_worker->running = local_worker->starting = false;
    --alive; wake_monitor(); pthread_mutex_unlock(&lock); return NULL;
}
/* lock held; slots are reused only after the old pthread was joined. */
static int boot_slot(hp_worker_thread_t *slot) {
    slot->occupied = slot->running = slot->starting = true;
    slot->waiting = slot->in_php = false; slot->parked = true;
    ++alive;
    int rc = pthread_create(&slot->thread, NULL, execution_main, slot);
    if (rc) {
        --alive; slot->occupied = slot->running = slot->starting = false;
        char message[192]; snprintf(message, sizeof(message), "cannot create execution thread: %s", strerror(rc));
        hp_log_message("php.runtime", message, 8, 0); return -1;
    }
    return 0;
}
static void assign_slot(hp_worker_thread_t *slot, hp_worker_group_t *g, bool minimum) {
    slot->group = g; slot->assigned = true; slot->minimum = minimum;
    slot->idle_since_ns = slot->next_interrupt_ns = slot->retry_after_ns = 0; slot->failures = 0;
    atomic_store(&slot->deactivating, false); atomic_store(&slot->bootstrap_timed_out, false);
    if (g) { ++g->started; atomic_fetch_add(&g->live, 1); }
    else ++nclassic;
}
static int add_thread(hp_worker_group_t *g, bool minimum) {
    hp_worker_thread_t *slot = NULL;
    for (unsigned i = 0; i < hp_config.max_threads; ++i)
        if (thread_slots[i].running && !thread_slots[i].assigned) { slot = &thread_slots[i]; break; }
    bool reuse = slot != NULL;
    if (!slot) for (unsigned i = 0; i < hp_config.max_threads; ++i)
        if (!thread_slots[i].occupied && !thread_slots[i].replacement) { slot = &thread_slots[i]; break; }
    if (!slot) return -1;
    assign_slot(slot, g, minimum);
    if (reuse) { atomic_fetch_add(&reactivated, 1); pthread_cond_signal(&slot->available); }
    else if (boot_slot(slot)) { detach_slot(slot); return -1; }
    if (!minimum) atomic_fetch_add(&scaled_up, 1);
    return 0;
}
static void reap_threads(void) {
    if (!thread_slots) return;
    for (unsigned i = 0; i < hp_config.max_threads; ++i) {
        hp_worker_thread_t *slot = &thread_slots[i];
        pthread_mutex_lock(&lock); bool ended = slot->occupied && !slot->running; pthread_t thread = slot->thread;
        pthread_mutex_unlock(&lock);
        if (ended) {
            pthread_join(thread, NULL);
            pthread_mutex_lock(&lock); slot->occupied = false; pthread_mutex_unlock(&lock);
        }
    }
}
static uint64_t restart_threads(uint64_t now) {
    uint64_t next = UINT64_MAX;
    for (unsigned i = 0; i < hp_config.max_threads; ++i) {
        hp_worker_thread_t *slot = &thread_slots[i];
        if (!slot->replacement || slot->occupied || draining || fork_gate) continue;
        if (now < slot->retry_after_ns) { if (slot->retry_after_ns < next) next = slot->retry_after_ns; continue; }
        slot->replacement = false;
        if (boot_slot(slot)) { startup_failed = true; stop_requested = true; }
    }
    return next;
}
static uint64_t bootstrap_deadlines(uint64_t now) {
    uint64_t next = UINT64_MAX;
    uint64_t drain_deadline = draining ? deadline_after_ms(drain_started_ns, hp_config.shutdown_timeout_ms) : UINT64_MAX;
    if (draining && now < drain_deadline) next = drain_deadline;
    if (draining && now >= drain_deadline && hp_sapi_aux_threads()) {
        hp_sapi_interrupt_request(NULL); next = deadline_after_ms(now, 10);
    }
    if (!thread_slots) return next;
    for (unsigned i = 0; i < hp_config.max_threads; ++i) {
        hp_worker_thread_t *slot = &thread_slots[i]; if (!slot->running) continue;
        bool overdue = slot->bootstrapping && now >= slot->bootstrap_deadline_ns;
        if (overdue && !atomic_exchange(&slot->bootstrap_timed_out, true))
            hp_log_message("php.worker", "worker startup deadline exceeded", 8, 0);
        if (overdue || (draining && now >= drain_deadline && slot->in_php && !slot->parked)) {
            if (now >= slot->next_interrupt_ns) {
                hp_sapi_interrupt_worker(slot); slot->next_interrupt_ns = deadline_after_ms(now, 10);
            }
            if (slot->next_interrupt_ns < next) next = slot->next_interrupt_ns;
        } else if (slot->bootstrapping && slot->bootstrap_deadline_ns < next) next = slot->bootstrap_deadline_ns;
    }
    return next;
}
static bool pool_demand(hp_worker_group_t *g, uint64_t now, uint64_t *stall_at) {
    hp_request_t *head = g ? g->head : classic_head;
    if (!head || (g && g->config.max_threads && g->started >= g->config.max_threads)) return false;
    pthread_mutex_lock(&head->mutex); bool cancelled = head->cancelled; pthread_mutex_unlock(&head->mutex);
    if (cancelled) return false;
    unsigned available = 0;
    for (unsigned i = 0; i < hp_config.max_threads; ++i) {
        hp_worker_thread_t *slot = &thread_slots[i];
        if (slot->running && slot->assigned && slot->group == g && !atomic_load(&slot->deactivating) &&
            (slot->waiting || slot->starting || slot->bootstrapping)) ++available;
    }
    size_t demand = g ? g->queued_count : classic_queued;
    if (demand <= available) return false;
    *stall_at = deadline_after_ms(head->queued_ns, 5);
    return now >= *stall_at;
}
static uint64_t cpu_time_ns(void) {
    struct timespec time; if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time)) return 0;
    return (uint64_t)time.tv_sec * 1000000000ULL + (uint64_t)time.tv_nsec;
}
static uint64_t scale_threads(uint64_t now) {
    if (draining || fork_gate || start_state != 2 || hp_config.max_threads <= minimum_threads) {
        cpu_probe_active = false; return UINT64_MAX;
    }
    bool slot_available = false;
    for (unsigned i = 0; i < hp_config.max_threads; ++i)
        if ((thread_slots[i].running && !thread_slots[i].assigned) || (!thread_slots[i].occupied && !thread_slots[i].replacement)) slot_available = true;
    if (!slot_available) { cpu_probe_active = false; return UINT64_MAX; }
    if (cpu_probe_active) {
        uint64_t deadline = deadline_after_ms(cpu_probe_started, 120), stalled;
        if (!pool_demand(cpu_probe_group, now, &stalled)) { cpu_probe_active = false; return UINT64_MAX; }
        if (now < deadline) return deadline;
        uint64_t elapsed = now - cpu_probe_started, used = cpu_time_ns() - cpu_probe_cpu_ns;
        unsigned cpus = hp_cpu_budget_scaling_count();
        last_cpu_usage = cpu_sampler ? cpu_sampler(elapsed, cpu_sampler_arg) : (double)used / (double)elapsed / cpus;
        hp_worker_group_t *g = cpu_probe_group; cpu_probe_active = false;
        if (last_cpu_usage < 0.8) {
            if (!add_thread(g, false)) hp_log_message("php.runtime", "execution pool grew by one thread", 0, 0);
        } else atomic_fetch_add(&cpu_scale_rejections, 1);
    }
    unsigned pools = 1; for (hp_worker_group_t *g = groups; g; g = g->next) ++pools;
    uint64_t next = UINT64_MAX;
    for (unsigned offset = 0; offset < pools; ++offset) {
        unsigned index = (scale_cursor + offset) % pools; hp_worker_group_t *g = NULL;
        if (index) { g = groups; for (unsigned j = 1; j < index; ++j) g = g->next; }
        uint64_t stalled = UINT64_MAX;
        if (!pool_demand(g, now, &stalled)) { if (stalled < next) next = stalled; continue; }
        cpu_probe_active = true; cpu_probe_group = g; cpu_probe_started = now; cpu_probe_cpu_ns = cpu_time_ns();
        scale_cursor = (index + 1) % pools; return deadline_after_ms(now, 120);
    }
    return next;
}
static void deactivate_threads(uint64_t now) {
    if (fork_gate || draining) return;
    unsigned deactivated = 0;
    for (unsigned i = hp_config.max_threads; i-- > 0 && deactivated < 11;) {
        hp_worker_thread_t *slot = &thread_slots[i];
        if (slot->running && slot->assigned && !slot->minimum && slot->waiting && slot->idle_since_ns &&
            now >= deadline_after_ms(slot->idle_since_ns, hp_config.max_idle_ms) && !atomic_load(&slot->deactivating)) {
            atomic_store(&slot->deactivating, true); ++deactivated; atomic_fetch_add(&idle_retired, 1);
            pthread_cond_signal(&slot->available);
        }
    }
}
int hp_runtime_submit(hp_request_t *r) {
    if (atomic_load(&fork_child_detached)) return -1;
    pthread_mutex_lock(&r->mutex);
    hp_php_metrics_request_begin_locked(&r->php_metrics, r->group ? r->group->metrics : NULL,
        r->php_metrics.origin_ns ? r->php_metrics.origin_ns : r->created_ns);
    r->queued_ns = hp_now_ns(); r->state = HP_QUEUED; pthread_mutex_unlock(&r->mutex);
    pthread_mutex_lock(&lock);
    unsigned applications = 1, assigned = nclassic;
    for (hp_worker_group_t *g = groups; g; g = g->next) { ++applications; assigned += g->started; }
    size_t waiting = r->group ? r->group->queued_count : classic_queued;
    /* Representability is a resource limit, independent of configured policy. */
    if (draining || start_state != 2 || queued == SIZE_MAX || waiting == SIZE_MAX) { pthread_mutex_unlock(&lock); return -1; }
    if (hp_config.queue_limit) {
        unsigned share = hp_config.queue_limit / applications; if (!share) share = 1;
        if (queued >= hp_config.queue_limit || waiting >= share) { pthread_mutex_unlock(&lock); return -1; }
    }
    hp_request_retain(r);
    hp_request_t **head = r->group ? &r->group->head : &classic_head, **tail = r->group ? &r->group->tail : &classic_tail;
    if (*tail) (*tail)->queue_next = r; else *head = r; *tail = r; ++queued;
    if (r->group) ++r->group->queued_count; else ++classic_queued;
    uint64_t deadline = queue_deadline(r);
    bool earlier_deadline = deadline < next_queue_deadline_ns;
    if (earlier_deadline) next_queue_deadline_ns = deadline;
    bool scalable = !fork_gate && hp_config.max_threads > minimum_threads && assigned < hp_config.max_threads &&
        (!r->group || !r->group->config.max_threads || r->group->started < r->group->config.max_threads);
    bool direct = dispatch_worker(r->group);
    /* Every accepted request occupies the native linked queue. Upstream's
     * queue metric counts actual waiting, excluding an available receiver's
     * immediate reservation; retain native FIFO/admission/wakeup policy. */
    if (fork_gate || !direct || hp_php_metrics_queue_depth(r->group ? r->group->metrics : NULL) > 0) {
        pthread_mutex_lock(&r->mutex);
        hp_php_metrics_request_queued_locked(&r->php_metrics);
        pthread_mutex_unlock(&r->mutex);
    }
    if (earlier_deadline || scalable) wake_monitor();
    pthread_mutex_unlock(&lock); return 0;
}
static void reject_queued(hp_request_t *r, int status, bool timeout) {
    pthread_mutex_lock(&r->mutex); r->status = status; r->headers_ready = r->response_finished = r->cancelled = true;
    r->state = timeout ? HP_TIMED_OUT : HP_CANCELLED;
    hp_php_metrics_request_end_locked(&r->php_metrics, hp_now_ns());
    pthread_cond_broadcast(&r->body_cond); pthread_cond_broadcast(&r->output_cond); pthread_mutex_unlock(&r->mutex);
    atomic_fetch_add(&failed, 1); atomic_fetch_add(&completed, 1); if (timeout) atomic_fetch_add(&timed_out, 1);
    hp_request_notify(r); hp_request_release(r);
}
void hp_runtime_begin_shutdown(void) {
    if (atomic_load(&fork_child_detached)) return;
    pthread_mutex_lock(&lock); if (!draining) drain_started_ns = hp_now_ns(); draining = true;
    hp_request_t *pending = classic_head; classic_head = classic_tail = NULL; classic_queued = 0;
    for (hp_worker_group_t *g = groups; g; g = g->next) {
        if (g->tail) { g->tail->queue_next = pending; pending = g->head; }
        g->head = g->tail = NULL; g->queued_count = 0;
    }
    queued = 0; next_queue_deadline_ns = UINT64_MAX; wake_slots(); pthread_mutex_unlock(&lock);
    while (pending) { hp_request_t *next = pending->queue_next; reject_queued(pending, 503, false); pending = next; }
}
void hp_runtime_restart_workers(void) {
    if (atomic_load(&fork_child_detached)) return;
    pthread_mutex_lock(&lock);
    for (hp_worker_group_t *g = groups; g; g = g->next) { atomic_fetch_add(&g->generation, 1); wake_workers(g); }
    wake_slots(); pthread_mutex_unlock(&lock);
}
static uint64_t path_hash(const char *path, unsigned depth) {
    if (depth > 32) return 0;
    struct stat st; if (lstat(path, &st)) return 0;
    uint64_t h = ((uint64_t)st.st_mtim.tv_sec * 1000000007ULL) ^ st.st_mtim.tv_nsec ^ st.st_size ^ st.st_ino;
    if (!S_ISDIR(st.st_mode)) return h;
    DIR *d = opendir(path); if (!d) return h;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *p; if (asprintf(&p, "%s/%s", path, e->d_name) < 0) continue;
        h ^= path_hash(p, depth + 1); free(p);
    }
    closedir(d); return h;
}
/* Shell-style component matching with recursive ** and brace alternatives.
 * Walking instead of one-shot glob expansion also sees newly created files. */
static bool watch_glob_plain(const char *pattern, const char *path) {
    while (*pattern == '/') ++pattern;
    while (*path == '/') ++path;
    const char *end = strchr(pattern, '/'); size_t n = end ? (size_t)(end - pattern) : strlen(pattern);
    if (n == 2 && pattern[0] == '*' && pattern[1] == '*') {
        const char *rest = end ? end + 1 : pattern + n;
        if (!*rest) return true;
        if (watch_glob_plain(rest, path)) return true;
        for (const char *p = path; (p = strchr(p, '/')) != NULL; ++p)
            if (watch_glob_plain(rest, p + 1)) return true;
        return false;
    }
    const char *path_end = strchr(path, '/'); size_t m = path_end ? (size_t)(path_end - path) : strlen(path);
    if (n >= PATH_MAX || m >= PATH_MAX) return false;
    char component[PATH_MAX], name[PATH_MAX];
    memcpy(component, pattern, n); component[n] = 0; memcpy(name, path, m); name[m] = 0;
    if (fnmatch(component, name, 0)) return false;
    if (!end) return !path_end;
    return watch_glob_plain(end + 1, path_end ? path_end + 1 : "");
}
static bool watch_glob(const char *pattern, const char *path, unsigned depth) {
    if (depth > 8) return false;
    const char *open = NULL;
    for (const char *p = pattern; *p; ++p) {
        if (*p == '\\' && p[1]) { ++p; continue; }
        if (*p == '{') { open = p; break; }
    }
    if (!open) return watch_glob_plain(pattern, path);
    unsigned nesting = 1; const char *close = open + 1;
    for (; *close && nesting; ++close) { if (*close == '{') ++nesting; else if (*close == '}') --nesting; }
    if (nesting) return false;
    --close;
    const char *alternative = open + 1; nesting = 0;
    for (const char *p = alternative;; ++p) {
        if (p == close || (*p == ',' && !nesting)) {
            size_t prefix = (size_t)(open - pattern), middle = (size_t)(p - alternative), suffix = strlen(close + 1);
            if (prefix + middle + suffix < PATH_MAX) {
                char expanded[PATH_MAX]; memcpy(expanded, pattern, prefix); memcpy(expanded + prefix, alternative, middle);
                memcpy(expanded + prefix + middle, close + 1, suffix + 1);
                if (watch_glob(expanded, path, depth + 1)) return true;
            }
            if (p == close) break;
            alternative = p + 1;
        } else if (*p == '{') ++nesting;
        else if (*p == '}' && nesting) --nesting;
    }
    return false;
}
static uint64_t watch_pattern_walk(const char *path, const char *pattern, unsigned depth) {
    if (depth > 32) return 0;
    struct stat st; if (lstat(path, &st)) return 0;
    uint64_t hash = 0;
    if (watch_glob(pattern, path, 0)) {
        hash = 14695981039346656037ULL;
        for (const unsigned char *p = (const unsigned char *)path; *p; ++p) hash = (hash ^ *p) * 1099511628211ULL;
        uint64_t fingerprint = path_hash(path, depth);
        for (unsigned shift = 0; shift < 64; shift += 8) hash = (hash ^ ((fingerprint >> shift) & 255U)) * 1099511628211ULL;
    }
    if (!S_ISDIR(st.st_mode)) return hash;
    DIR *dir = opendir(path); if (!dir) return hash;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char *child; if (asprintf(&child, "%s/%s", path, entry->d_name) < 0) continue;
        hash ^= watch_pattern_walk(child, pattern, depth + 1); free(child);
    }
    closedir(dir); return hash;
}
static uint64_t watch_path_hash(const char *pattern) {
    const char *magic = strpbrk(pattern, "*?[{");
    if (!magic) return path_hash(pattern, 0);
    const char *slash = magic;
    while (slash > pattern && slash[-1] != '/') --slash;
    size_t n = (size_t)(slash - pattern);
    char root[PATH_MAX];
    if (n >= sizeof(root)) return 0;
    if (n) { memcpy(root, pattern, n); root[n] = 0; if (n > 1) root[n - 1] = 0; }
    else strcpy(root, ".");
    return watch_pattern_walk(root, pattern, 0);
}
static uint64_t worker_watch_hash(hp_worker_group_t *g) {
    /* Combine roots in order: XOR would cancel a file's changes when both
     * that file and its containing directory are watched. Include path names
     * so duplicate or overlapping configured roots cannot cancel each other. */
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < g->config.num_watch; ++i) {
        const char *path = g->config.watch[i];
        for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
            hash = (hash ^ *p) * 1099511628211ULL;
        hash = (hash ^ 0xffU) * 1099511628211ULL;
        uint64_t fingerprint = watch_path_hash(path);
        for (unsigned shift = 0; shift < 64; shift += 8)
            hash = (hash ^ ((fingerprint >> shift) & 255U)) * 1099511628211ULL;
    }
    return hash;
}
static void expire_queue(hp_request_t **head, hp_request_t **tail, size_t *count, hp_request_t **expired, uint64_t now) {
    hp_request_t **p = head; hp_request_t *previous = NULL;
    while (*p) {
        hp_request_t *r = *p; uint64_t deadline = queue_deadline(r);
        pthread_mutex_lock(&r->mutex);
        bool cancelled = r->cancelled;
        if (now >= deadline) r->state = HP_TIMED_OUT;
        pthread_mutex_unlock(&r->mutex);
        if (cancelled || now >= deadline) {
            *p = r->queue_next; if (*tail == r) *tail = previous;
            --*count; --queued; r->queue_next = *expired; *expired = r;
        } else {
            if (deadline < next_queue_deadline_ns) next_queue_deadline_ns = deadline;
            previous = r; p = &r->queue_next;
        }
    }
}
static void expire_queued(void) {
    hp_request_t *expired = NULL;
    pthread_mutex_lock(&lock); uint64_t now = hp_now_ns(); next_queue_deadline_ns = UINT64_MAX;
    expire_queue(&classic_head, &classic_tail, &classic_queued, &expired, now);
    for (hp_worker_group_t *g = groups; g; g = g->next) expire_queue(&g->head, &g->tail, &g->queued_count, &expired, now);
    pthread_mutex_unlock(&lock);
    while (expired) {
        hp_request_t *r = expired; expired = r->queue_next;
        pthread_mutex_lock(&r->mutex); bool timeout = r->state == HP_TIMED_OUT; pthread_mutex_unlock(&r->mutex);
        reject_queued(r, 503, timeout);
    }
}
static void monitor(bool scan_watch) {
    hp_request_t **list = NULL; size_t n = 0;
    pthread_mutex_lock(&lock);
    bool drain_overdue = draining && hp_now_ns() >= deadline_after_ms(drain_started_ns, hp_config.shutdown_timeout_ms);
    for (hp_request_t *r = active; r; r = r->active_next) ++n;
    list = calloc(n, sizeof(*list)); size_t i = 0;
    if (list) for (hp_request_t *r = active; r; r = r->active_next) { list[i++] = r; hp_request_retain(r); }
    pthread_mutex_unlock(&lock);
    for (size_t j = 0; j < i; ++j) {
        hp_request_t *r = list[j];
        if (drain_overdue || hp_now_ns() >= r->deadline_ns) {
            pthread_mutex_lock(&r->mutex); r->state = HP_TIMED_OUT; pthread_mutex_unlock(&r->mutex);
            hp_request_cancel(r); hp_sapi_interrupt_request(r); hp_request_notify(r);
        }
        hp_request_release(r);
    }
    free(list);
    if (drain_overdue && hp_sapi_aux_threads()) hp_sapi_interrupt_request(NULL);
    if (!scan_watch) return;
    bool changed = false;
    for (hp_worker_group_t *g = groups; g; g = g->next) if (g->config.num_watch) {
        uint64_t fingerprint = worker_watch_hash(g);
        if (fingerprint != g->watch_hash) changed = true;
        g->watch_hash = fingerprint;
    }
    if (changed) {
        pthread_mutex_lock(&lock);
        /* FrankenPHP's watcher restarts the watched worker set together. */
        for (hp_worker_group_t *g = groups; g; g = g->next) if (g->config.num_watch) {
            atomic_fetch_add(&g->generation, 1); wake_workers(g);
        }
        wake_slots(); pthread_mutex_unlock(&lock);
    }
}
static uint64_t system_memory(void) {
#if defined(__linux__)
    struct sysinfo info;
    if (!sysinfo(&info) && info.mem_unit && info.totalram <= UINT64_MAX / info.mem_unit)
        return (uint64_t)info.totalram * info.mem_unit;
#endif
    long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    return pages > 0 && size > 0 && (uint64_t)pages <= UINT64_MAX / (uint64_t)size ? (uint64_t)pages * size : 0;
}
static int resolve_thread_counts(void) {
    uint64_t workers = 0, extra = 0;
    for (hp_worker_group_t *g = groups; g; g = g->next) {
        workers += g->config.num;
        if (g->config.max_threads) {
            if (g->config.max_threads < g->config.num) {
                fprintf(stderr, "php.worker: max_threads %u is below num %u for %s\n", g->config.max_threads, g->config.num, g->config.name); return -1;
            }
            extra += g->config.max_threads - g->config.num;
        }
    }
    bool num_set = hp_config.threads_explicit && hp_config.threads > 0;
    bool max_set = hp_config.max_threads_auto || (hp_config.max_threads_explicit && hp_config.max_threads > 0);
    uint64_t regular = num_set ? hp_config.threads : 1;
    uint64_t total = workers + regular, maximum = hp_config.max_threads;
    if (!max_set && extra) { max_set = true; maximum = total + extra; }
    if (!num_set && !max_set) {
        unsigned cpus;
        if (!hp_cpu_budget_default_count_checked(&cpus)) {
            fprintf(stderr, "php.runtime: CPU-derived default is not representable; configure explicit positive thread budgets\n");
            return -1;
        }
        uint64_t cpu_total = (uint64_t)cpus * 2;
        total = workers >= cpu_total ? workers + 1 : cpu_total;
        regular = total - workers; maximum = total;
    } else if (num_set && !max_set) maximum = total;
    if (!regular || total > INT_MAX - 2 || (!hp_config.max_threads_auto && (maximum < total || maximum > INT_MAX - 2))) {
        fprintf(stderr, "php.runtime: invalid thread budget (regular %llu, worker %llu, maximum %llu)\n",
                (unsigned long long)regular, (unsigned long long)workers, (unsigned long long)maximum); return -1;
    }
    if (!hp_config.max_threads_auto) for (hp_worker_group_t *g = groups; g; g = g->next)
        if (g->config.max_threads > maximum) { fprintf(stderr, "php.worker: max_threads exceeds global maximum for %s\n", g->config.name); return -1; }
    hp_config.threads = (unsigned)regular; hp_config.max_threads = hp_config.max_threads_auto ? (unsigned)total : (unsigned)maximum;
    minimum_threads = (unsigned)total;
    if (!hp_config.max_idle_ms) hp_config.max_idle_ms = 5000; /* Upstream zero selects the default. */
    return 0;
}
/* Configuration-only preparation never starts PHP or execution pthreads. */
int hp_runtime_config_prepare(void) {
    hp_runtime_config_defaults();
    hp_cpu_budget_capture_startup(); /* Cold I/O must precede L even for direct embedders. */
    pthread_mutex_lock(&lock);
    if (start_state || controller_created) { pthread_mutex_unlock(&lock); errno = EBUSY; return -1; }
    int rc = resolve_thread_counts();
    pthread_mutex_unlock(&lock); return rc;
}
void hp_runtime_each_worker(void (*visit)(const hp_worker_config_t *, void *), void *data) {
    /* Group declarations are immutable after configuration. Invoke user
     * callbacks without a scheduler lock, including for effective-config. */
    if (visit) for (hp_worker_group_t *g = groups; g; g = g->next) visit(&g->config, data);
}
static void resolve_automatic_maximum(void) {
    if (!hp_config.max_threads_auto) return;
    int64_t per_thread = hp_sapi_memory_limit(); uint64_t memory = system_memory();
    uint64_t maximum = per_thread > 0 && memory ? memory / (uint64_t)per_thread : (uint64_t)minimum_threads * 2;
    if (maximum < minimum_threads) maximum = minimum_threads;
    if (maximum > INT_MAX - 2) maximum = INT_MAX - 2;
    hp_config.max_threads = (unsigned)maximum;
}
static void *control_main(void *unused) {
    (void)unused;
    bool engine_initialized = false, process_initialized = false; unsigned initialized = 0;
    if (resolve_thread_counts() || hp_config.max_buffered_global < 16384 ||
        !hp_config.max_buffered_body || !hp_config.max_buffered_response || !hp_config.startup_timeout_ms) goto Failed;
    watcher_enabled = false;
    for (hp_worker_group_t *g = groups; g; g = g->next) if (g->config.num_watch) {
        watcher_enabled = true; g->watch_hash = worker_watch_hash(g);
    }
    if (watcher_enabled) {
        char *entries;
        if (asprintf(&entries, "%sopcache.validate_timestamps=1\nopcache.revalidate_freq=0\n", hp_config.ini_entries ? hp_config.ini_entries : "") < 0) goto Failed;
        free(hp_config.ini_entries); hp_config.ini_entries = entries;
    }
    if (hp_php_metrics_start(minimum_threads)) goto Failed;
    if (hp_process_start()) goto Failed;
    process_initialized = true;
    if (hp_sapi_init(hp_config.ini_entries, hp_config.max_threads + 1)) goto Failed;
    engine_initialized = true; resolve_automatic_maximum();
    for (hp_worker_group_t *g = groups; g; g = g->next) if (g->config.max_threads > hp_config.max_threads) {
        hp_log_message("php.runtime", "worker maximum exceeds automatic global maximum", 8, 0); goto Failed;
    }
    thread_slots = calloc(hp_config.max_threads, sizeof(*thread_slots)); if (!thread_slots) goto Failed;
    pthread_condattr_t attr; int rc = pthread_condattr_init(&attr);
    if (rc) goto Failed;
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!rc) for (; initialized < hp_config.max_threads; ++initialized) {
        rc = pthread_cond_init(&thread_slots[initialized].available, &attr); if (rc) break;
        atomic_init(&thread_slots[initialized].deactivating, false); atomic_init(&thread_slots[initialized].bootstrap_timed_out, false);
    }
    pthread_condattr_destroy(&attr); if (rc) goto Failed;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < hp_config.threads && !startup_failed; ++i) if (add_thread(NULL, true)) startup_failed = true;
    for (hp_worker_group_t *g = groups; g && !startup_failed; g = g->next)
        for (unsigned i = 0; i < g->config.num; ++i) if (add_thread(g, true)) { startup_failed = true; break; }
    pthread_mutex_unlock(&lock);
    uint64_t next_housekeeping = deadline_after_ms(hp_now_ns(), 1000), next_downscale = deadline_after_ms(hp_now_ns(), 5000);
    pthread_mutex_lock(&lock);
    for (;;) {
        pthread_mutex_unlock(&lock); reap_threads(); pthread_mutex_lock(&lock);
        uint64_t now = hp_now_ns(), boot_next = bootstrap_deadlines(now), restart_next = restart_threads(now);
        if (startup_failed) stop_requested = true;
        if (stop_requested) break;
        if (start_state == 1) {
            bool settled = true;
            for (unsigned i = 0; i < hp_config.max_threads; ++i) {
                hp_worker_thread_t *slot = &thread_slots[i];
                if (slot->minimum && (slot->starting || slot->replacement || (slot->group && !slot->waiting))) settled = false;
            }
            if (settled) { start_state = 2; pthread_cond_broadcast(&cond); }
        }
        uint64_t scaling_next = scale_threads(now);
        if (now >= next_downscale) { deactivate_threads(now); next_downscale = deadline_after_ms(now, 5000); }
        bool expiry = now >= next_queue_deadline_ns, housekeeping = now >= next_housekeeping;
        if (expiry || housekeeping) {
            pthread_mutex_unlock(&lock); if (expiry || housekeeping) expire_queued();
            if (housekeeping) { monitor(true); next_housekeeping = deadline_after_ms(hp_now_ns(), 1000); }
            pthread_mutex_lock(&lock); continue;
        }
        uint64_t deadline = next_queue_deadline_ns < next_housekeeping ? next_queue_deadline_ns : next_housekeeping;
        if (next_downscale < deadline) deadline = next_downscale;
        if (boot_next < deadline) deadline = boot_next;
        if (restart_next < deadline) deadline = restart_next;
        if (scaling_next < deadline) deadline = scaling_next;
        /* An executor can exit after reap_threads scanned its slot while the
         * controller was outside lock. Its wake is then already past. Check
         * the published lifecycle predicate before atomically entering wait. */
        bool reap_pending = false;
        for (unsigned i = 0; i < hp_config.max_threads; ++i)
            if (thread_slots[i].occupied && !thread_slots[i].running) { reap_pending = true; break; }
        if (reap_pending) continue;
        wait_for_monitor(deadline);
    }
    pthread_mutex_unlock(&lock); hp_runtime_begin_shutdown(); monitor(false); pthread_mutex_lock(&lock);
    while (alive || hp_sapi_aux_threads()) {
        uint64_t now = hp_now_ns(), boot_next = bootstrap_deadlines(now);
        if (now >= next_housekeeping) {
            pthread_mutex_unlock(&lock); monitor(true); next_housekeeping = deadline_after_ms(hp_now_ns(), 1000); pthread_mutex_lock(&lock);
        } else {
            uint64_t deadline = boot_next < next_housekeeping ? boot_next : next_housekeeping;
            if (!alive && hp_sapi_aux_threads() && deadline > deadline_after_ms(now, 10)) deadline = deadline_after_ms(now, 10);
            wait_for_monitor(deadline);
        }
    }
    pthread_mutex_unlock(&lock); reap_threads(); hp_sapi_shutdown(); engine_initialized = false;
    hp_process_stop(); process_initialized = false;
    hp_php_metrics_shutdown();
    if (startup_failed) goto Failed;
    atomic_store_explicit(&controller_stopped, true, memory_order_release); return NULL;
Failed:
    hp_php_metrics_shutdown();
    if (engine_initialized) hp_sapi_shutdown();
    if (process_initialized) hp_process_stop();
    if (thread_slots && !alive) {
        for (unsigned i = 0; i < initialized; ++i) pthread_cond_destroy(&thread_slots[i].available);
        free(thread_slots); thread_slots = NULL;
    }
    atomic_store_explicit(&controller_stopped, true, memory_order_release);
    pthread_mutex_lock(&lock); start_state = -1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock); return NULL;
}
int hp_runtime_start(void) {
    hp_runtime_config_defaults(); hp_cpu_budget_capture_startup();
    pthread_mutex_lock(&lock);
    if (start_state == 2) { pthread_mutex_unlock(&lock); return 0; }
    if (!monitor_cond_initialized) {
        pthread_condattr_t attr; int rc = pthread_condattr_init(&attr);
        if (!rc) { rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC); if (!rc) rc = pthread_cond_init(&monitor_cond, &attr); pthread_condattr_destroy(&attr); }
        if (rc) { fprintf(stderr, "php.runtime: cannot initialize monitor: %s\n", strerror(rc)); start_state = -1; pthread_mutex_unlock(&lock); return -1; }
        monitor_cond_initialized = true;
    }
    start_state = 1; atomic_store_explicit(&controller_stopped, false, memory_order_relaxed);
    if (pthread_create(&controller, NULL, control_main, NULL)) { start_state = -1; pthread_mutex_unlock(&lock); return -1; }
    controller_created = true;
    while (start_state == 1) pthread_cond_wait(&cond, &lock);
    int rc = start_state == 2 ? 0 : -1; pthread_mutex_unlock(&lock); return rc;
}
void hp_runtime_request_stop(void) {
    if (atomic_load(&fork_child_detached)) return;
    pthread_mutex_lock(&lock); if (!draining) drain_started_ns = hp_now_ns(); draining = stop_requested = true;
    wake_slots(); pthread_mutex_unlock(&lock);
}
bool hp_runtime_stopped(void) { return atomic_load_explicit(&controller_stopped, memory_order_acquire); }
void hp_runtime_stop(void) {
    if (atomic_load(&fork_child_detached)) return;
    hp_runtime_request_stop(); pthread_mutex_lock(&lock);
    if (!controller_created) { pthread_mutex_unlock(&lock); return; }
    controller_created = false; pthread_mutex_unlock(&lock); pthread_join(controller, NULL);
}
int hp_runtime_fork_prepare(void *owner_token, uint64_t timeout_ms) {
    if (atomic_load(&fork_child_detached)) return 0;
    pthread_mutex_lock(&lock);
    if (fork_gate || draining || start_state != 2) { pthread_mutex_unlock(&lock); errno = EAGAIN; return -1; }
    fork_gate = true; fork_owner = owner_token; wake_slots();
    uint64_t deadline = deadline_after_ms(hp_now_ns(), timeout_ms);
    for (;;) {
        bool quiet = true;
        for (unsigned i = 0; i < hp_config.max_threads; ++i) {
            hp_worker_thread_t *slot = &thread_slots[i];
            if (slot != owner_token && slot->running && slot->in_php && !slot->parked) { quiet = false; break; }
        }
        if (quiet) { pthread_mutex_unlock(&lock); return 0; }
        if (hp_now_ns() >= deadline || draining) {
            fork_gate = false; fork_owner = NULL; wake_slots(); pthread_mutex_unlock(&lock); errno = EAGAIN; return -1;
        }
        wait_for_monitor(deadline);
    }
}
void hp_runtime_fork_parent(void) {
    if (atomic_load(&fork_child_detached)) return;
    pthread_mutex_lock(&lock); fork_gate = false; fork_owner = NULL; wake_slots(); pthread_mutex_unlock(&lock);
}
void hp_runtime_fork_child(void) {
    /* No inherited mutex/condition or H2O/runtime resource is touched here. */
    atomic_store_explicit(&fork_child_detached, true, memory_order_release);
    fork_gate = false; fork_owner = NULL; controller_created = false;
}
static bool resident_bytes(uint64_t *bytes) {
#if defined(__linux__)
    FILE *file = fopen("/proc/self/statm", "re");
    if (!file) return false;
    unsigned long long virtual_pages, resident_pages;
    int fields = fscanf(file, "%llu %llu", &virtual_pages, &resident_pages);
    fclose(file);
    long page_size = sysconf(_SC_PAGESIZE);
    if (fields != 2 || page_size <= 0 || resident_pages > UINT64_MAX / (uint64_t)page_size) return false;
    *bytes = (uint64_t)resident_pages * (uint64_t)page_size;
    return true;
#else
    (void)bytes;
    return false;
#endif
}
void hp_metrics_json(char *out, size_t cap) {
    if (atomic_load(&fork_child_detached)) { snprintf(out, cap, "{\"detached_fork_child\":true}\n"); return; }
    char cpu_budget[HP_CPU_BUDGET_JSON_CAP];
    if (!hp_cpu_budget_json(cpu_budget, sizeof(cpu_budget))) {
        snprintf(out, cap, "{\"error\":\"CPU metadata formatting failed\"}\n"); return;
    }
    pthread_mutex_lock(&lock);
    size_t q = queued;
    unsigned t = alive, classic = 0, workers = 0, dynamic = 0, inactive = 0, bootstrapping = 0, pending_reboot = 0;
    if (thread_slots) for (unsigned i = 0; i < hp_config.max_threads; ++i) {
        hp_worker_thread_t *slot = &thread_slots[i];
        if (slot->replacement) ++pending_reboot;
        if (!slot->running) continue;
        if (!slot->assigned) { ++inactive; continue; }
        if (slot->group) ++workers; else ++classic;
        if (!slot->minimum) ++dynamic;
        if (slot->bootstrapping) ++bootstrapping;
    }
    double cpu = last_cpu_usage;
    pthread_mutex_unlock(&lock);
    unsigned executing = atomic_load(&active_count), aux = hp_sapi_aux_threads(), busy_workers = 0, failed_workers = 0;
    for (hp_worker_group_t *g = groups; g; g = g->next) { busy_workers += atomic_load(&g->busy); failed_workers += atomic_load(&g->failed_threads); }
    uint64_t rss; char rss_json[32] = "null";
    if (resident_bytes(&rss)) snprintf(rss_json, sizeof(rss_json), "%llu", (unsigned long long)rss);
    snprintf(out, cap,
        "{\"active_php_threads\":%u,\"idle_php_threads\":%u,\"threads\":%u,\"active_worker_scripts\":%u,\"busy_workers\":%u,\"bootstrapping_workers\":%u,\"failed_workers\":%u,\"ready_workers\":%u,\"queued_requests\":%zu,\"completed\":%llu,\"failed\":%llu,\"timed_out\":%llu,\"worker_restarts\":%llu,\"worker_startup_failures\":%llu,\"queue_wait_ns_total\":%llu,\"execution_ns_total\":%llu,\"buffered_bytes\":%zu,\"rss_bytes\":%s,\"minimum_threads\":%u,\"max_threads\":%u,\"classic_threads\":%u,\"worker_threads\":%u,\"dynamic_threads\":%u,\"inactive_threads\":%u,\"reserved_threads\":%u,\"pending_reboots\":%u,\"auxiliary_threads\":%u,\"scaled_up\":%llu,\"idle_retired\":%llu,\"reactivated\":%llu,\"thread_reboots\":%llu,\"unhealthy_reboots\":%llu,\"scaling_cpu_usage\":%.6f,\"cpu_scale_rejections\":%llu,\"cpu_budget\":%s}\n",
        executing, t >= executing ? t - executing : 0, t, workers, busy_workers, bootstrapping, failed_workers, atomic_load(&ready_workers), q,
        (unsigned long long)atomic_load(&completed), (unsigned long long)atomic_load(&failed), (unsigned long long)atomic_load(&timed_out),
        (unsigned long long)atomic_load(&restarts), (unsigned long long)atomic_load(&startup_failures),
        (unsigned long long)atomic_load(&queue_ns), (unsigned long long)atomic_load(&execution_ns), hp_bridge_buffered_bytes(), rss_json,
        minimum_threads, hp_config.max_threads, classic, workers, dynamic, inactive, hp_config.max_threads >= t ? hp_config.max_threads - t : 0,
        pending_reboot, aux, (unsigned long long)atomic_load(&scaled_up), (unsigned long long)atomic_load(&idle_retired),
        (unsigned long long)atomic_load(&reactivated), (unsigned long long)atomic_load(&thread_reboots),
        (unsigned long long)atomic_load(&unhealthy_reboots), cpu, (unsigned long long)atomic_load(&cpu_scale_rejections), cpu_budget);
}
