#define _GNU_SOURCE
#include "h2o_php.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

hp_runtime_config_t hp_config;
struct hp_worker_group {
    hp_worker_config_t config;
    hp_worker_group_t *next;
    hp_request_t *head, *tail;
    pthread_t *threads;
    atomic_uint generation;
    uint64_t watch_hash;
    unsigned started, queued_count;
    atomic_uint live, ready, busy;
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static hp_worker_group_t *groups;
static hp_request_t *classic_head, *classic_tail, *active;
static pthread_t controller, *classic_threads;
static unsigned nclassic, queued, alive, classic_queued;
static int start_state; /* 0 not started, 1 starting, 2 ready, -1 failed */
static bool draining, stop_requested, controller_created;
static uint64_t drain_started_ns;
static _Thread_local hp_worker_group_t *local_group;
static _Thread_local unsigned local_generation, local_requests;
static _Thread_local bool local_ready, local_busy;
static atomic_uint_fast64_t completed, failed, timed_out, restarts, startup_failures, queue_ns, execution_ns;
static atomic_uint active_count, ready_workers;

void hp_runtime_config_defaults(void) {
    if (hp_config.threads) return;
    hp_config = (hp_runtime_config_t){ .threads = 4, .max_threads = 64, .queue_limit = 1024, .max_requests = 10000,
        .max_buffered_body = 1048576, .max_buffered_response = 262144, .max_buffered_global = 268435456,
        .max_wait_ms = 10000, .request_timeout_ms = 30000, .shutdown_timeout_ms = 30000 };
}
hp_worker_group_t *hp_worker_group_create(const hp_worker_config_t *c) {
    hp_worker_group_t *g = calloc(1, sizeof(*g)); if (!g) return NULL;
    g->config = *c;
    g->config.file = strdup(c->file); g->config.name = strdup(c->name ? c->name : c->file);
    g->config.root = c->root ? strdup(c->root) : NULL; g->config.env = hp_headers_clone(c->env);
    g->config.num = c->num ? c->num : 1;
    g->config.max_consecutive_failures = c->max_consecutive_failures ? c->max_consecutive_failures : 10;
    g->config.watch = calloc(c->num_watch, sizeof(char *));
    for (size_t i = 0; i < c->num_watch; ++i) g->config.watch[i] = strdup(c->watch[i]);
    atomic_init(&g->generation, 1); atomic_init(&g->live, 0); atomic_init(&g->ready, 0); atomic_init(&g->busy, 0);
    g->next = groups; groups = g; return g;
}
const hp_worker_config_t *hp_worker_group_config(hp_worker_group_t *g) { return &g->config; }
bool hp_worker_should_restart(hp_worker_group_t *g) {
    return atomic_load(&g->generation) != local_generation ||
        (g->config.max_requests && local_requests >= g->config.max_requests);
}
void hp_worker_ready(hp_worker_group_t *g) {
    if (!local_ready) { local_ready = true; atomic_fetch_add(&ready_workers, 1); atomic_fetch_add(&g->ready, 1); }
}
static void clear_ready(void) {
    if (local_ready) { local_ready = false; atomic_fetch_sub(&ready_workers, 1); if (local_group) atomic_fetch_sub(&local_group->ready, 1); }
}
static void activate(hp_request_t *r) {
    r->active_next = active; active = r;
    atomic_fetch_add(&active_count, 1);
}
static hp_request_t *take(hp_worker_group_t *g) {
Retry:
    pthread_mutex_lock(&lock);
    hp_request_t **head = g ? &g->head : &classic_head;
    hp_request_t **tail = g ? &g->tail : &classic_tail;
    while (!*head && !draining && !(g && hp_worker_should_restart(g))) pthread_cond_wait(&cond, &lock);
    if ((g && hp_worker_should_restart(g)) || !*head) { pthread_mutex_unlock(&lock); return NULL; }
    hp_request_t *r = *head; *head = r->queue_next; if (!*head) *tail = NULL;
    r->queue_next = NULL; --queued;
    if (g) --g->queued_count; else --classic_queued;
    activate(r);
    pthread_mutex_unlock(&lock);
    pthread_mutex_lock(&r->mutex);
    r->started_ns = hp_now_ns(); r->state = HP_RUNNING;
    if (r->started_ns - r->queued_ns > hp_config.max_wait_ms * 1000000ULL || r->started_ns >= r->deadline_ns) {
        r->state = HP_TIMED_OUT; r->cancelled = true;
        r->status = 504; r->headers_ready = r->response_finished = true;
    }
    bool cancelled = r->cancelled;
    pthread_mutex_unlock(&r->mutex);
    atomic_fetch_add(&queue_ns, r->started_ns - r->queued_ns);
    if (cancelled) {
        hp_response_finish(r); hp_runtime_request_done(r); goto Retry;
    }
    return r;
}
hp_request_t *hp_worker_next(hp_worker_group_t *g) {
    hp_worker_ready(g);
    hp_request_t *r = take(g);
    clear_ready();
    if (r) { local_busy = true; atomic_fetch_add(&g->busy, 1); }
    return r;
}
void hp_runtime_request_done(hp_request_t *r) {
    hp_response_finish(r);
    pthread_mutex_lock(&r->mutex);
    bool bad = r->cancelled || r->status >= 500;
    bool timeout = r->state == HP_TIMED_OUT;
    if (!r->cancelled) r->state = HP_COMPLETED;
    pthread_mutex_unlock(&r->mutex);
    if (bad) atomic_fetch_add(&failed, 1);
    if (timeout) atomic_fetch_add(&timed_out, 1);
    atomic_fetch_add(&completed, 1);
    atomic_fetch_add(&execution_ns, hp_now_ns() - r->started_ns);
    pthread_mutex_lock(&lock);
    hp_request_t **p = &active; while (*p && *p != r) p = &(*p)->active_next;
    if (*p) { *p = r->active_next; atomic_fetch_sub(&active_count, 1); }
    pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock);
    if (local_group && r->group == local_group) { ++local_requests; if (local_busy) { local_busy = false; atomic_fetch_sub(&local_group->busy, 1); } }
    hp_request_release(r);
}
static void thread_ended(void) {
    pthread_mutex_lock(&lock); --alive; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock);
}
static void *classic_main(void *unused) {
    (void)unused; hp_sapi_thread_init();
    hp_request_t *r;
    unsigned requests = 0;
    while ((r = take(NULL))) {
        hp_sapi_execute(r); hp_runtime_request_done(r);
        if (hp_config.max_requests && ++requests >= hp_config.max_requests) {
            hp_sapi_thread_shutdown(); hp_sapi_thread_init(); requests = 0;
        }
    }
    hp_sapi_thread_shutdown(); thread_ended(); return NULL;
}
static bool wait_restart(unsigned ms) {
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000; until.tv_nsec += (ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&lock);
    while (!draining) if (pthread_cond_timedwait(&cond, &lock, &until) == ETIMEDOUT) break;
    bool stop = draining; pthread_mutex_unlock(&lock); return stop;
}
static void *worker_main(void *arg) {
    hp_worker_group_t *g = arg; local_group = g; unsigned failures = 0;
    hp_sapi_thread_init();
    for (;;) {
        pthread_mutex_lock(&lock); bool stop = draining && !g->head; pthread_mutex_unlock(&lock);
        if (stop) break;
        local_generation = atomic_load(&g->generation); local_requests = 0;
        int rc = hp_sapi_worker(g, g->config.file);
        clear_ready();
        pthread_mutex_lock(&lock); stop = draining; pthread_mutex_unlock(&lock);
        if (stop) break;
        atomic_fetch_add(&restarts, 1);
        if (rc != 0 && local_requests == 0) {
            atomic_fetch_add(&startup_failures, 1); ++failures;
            fprintf(stderr, "{\"component\":\"php.worker\",\"event\":\"startup_failed\",\"attempt\":%u,\"code\":%d}\n", failures, rc);
            if (failures >= g->config.max_consecutive_failures) break;
            unsigned delay = 100U << (failures < 7 ? failures - 1 : 6);
            if (wait_restart(delay)) break;
        } else failures = 0;
    }
    hp_sapi_thread_shutdown(); atomic_fetch_sub(&g->live, 1); thread_ended(); return NULL;
}
int hp_runtime_submit(hp_request_t *r) {
    pthread_mutex_lock(&r->mutex); r->queued_ns = hp_now_ns(); r->state = HP_QUEUED; pthread_mutex_unlock(&r->mutex);
    pthread_mutex_lock(&lock);
    unsigned applications = 1; for (hp_worker_group_t *g = groups; g; g = g->next) ++applications;
    unsigned share = hp_config.queue_limit / applications; if (!share) share = 1;
    unsigned waiting = r->group ? r->group->queued_count : classic_queued;
    if (draining || start_state != 2 || queued >= hp_config.queue_limit || waiting >= share ||
        (r->group && !atomic_load(&r->group->live))) { pthread_mutex_unlock(&lock); return -1; }
    hp_request_retain(r);
    hp_request_t **head = r->group ? &r->group->head : &classic_head;
    hp_request_t **tail = r->group ? &r->group->tail : &classic_tail;
    if (*tail) (*tail)->queue_next = r; else *head = r; *tail = r; ++queued;
    if (r->group) ++r->group->queued_count; else ++classic_queued;
    pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock); return 0;
}
static void reject_queued(hp_request_t *r, int status, bool timeout) {
    pthread_mutex_lock(&r->mutex);
    r->status = status; r->headers_ready = true; r->response_finished = true;
    r->cancelled = true; r->state = timeout ? HP_TIMED_OUT : HP_CANCELLED;
    pthread_cond_broadcast(&r->body_cond); pthread_cond_broadcast(&r->output_cond);
    pthread_mutex_unlock(&r->mutex);
    atomic_fetch_add(&failed, 1); atomic_fetch_add(&completed, 1);
    if (timeout) atomic_fetch_add(&timed_out, 1);
    hp_request_notify(r); hp_request_release(r);
}
void hp_runtime_begin_shutdown(void) {
    pthread_mutex_lock(&lock);
    if (!draining) drain_started_ns = hp_now_ns();
    draining = true;
    hp_request_t *pending = classic_head; classic_head = classic_tail = NULL; classic_queued = 0;
    for (hp_worker_group_t *g = groups; g; g = g->next) {
        if (g->tail) { g->tail->queue_next = pending; pending = g->head; }
        g->head = g->tail = NULL; g->queued_count = 0;
    }
    queued = 0; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock);
    while (pending) { hp_request_t *next = pending->queue_next; reject_queued(pending, 503, false); pending = next; }
}
void hp_runtime_restart_workers(void) {
    pthread_mutex_lock(&lock);
    for (hp_worker_group_t *g = groups; g; g = g->next) atomic_fetch_add(&g->generation, 1);
    pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock);
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
static void expire_queue(hp_request_t **head, hp_request_t **tail, unsigned *count, hp_request_t **expired) {
    hp_request_t **p = head; hp_request_t *previous = NULL;
    uint64_t now = hp_now_ns();
    while (*p) {
        hp_request_t *r = *p;
        if (now >= r->deadline_ns || now - r->queued_ns >= hp_config.max_wait_ms * 1000000ULL) {
            *p = r->queue_next;
            if (*tail == r) *tail = previous;
            --*count; --queued; r->queue_next = *expired; *expired = r;
        } else { previous = r; p = &r->queue_next; }
    }
}
static void monitor(void) {
    hp_request_t *expired = NULL;
    pthread_mutex_lock(&lock);
    expire_queue(&classic_head, &classic_tail, &classic_queued, &expired);
    for (hp_worker_group_t *g = groups; g; g = g->next)
        expire_queue(&g->head, &g->tail, &g->queued_count, &expired);
    pthread_mutex_unlock(&lock);
    while (expired) { hp_request_t *r = expired; expired = r->queue_next; reject_queued(r, 504, true); }
    hp_request_t **list = NULL; size_t n = 0;
    pthread_mutex_lock(&lock);
    bool drain_overdue = draining && hp_now_ns() - drain_started_ns >= hp_config.shutdown_timeout_ms * 1000000ULL;
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
    if (drain_overdue) hp_sapi_interrupt_request(NULL);
    for (hp_worker_group_t *g = groups; g; g = g->next) {
        uint64_t h = 0;
        for (size_t j = 0; j < g->config.num_watch; ++j) h ^= path_hash(g->config.watch[j], 0);
        if (g->watch_hash && h != g->watch_hash) {
            atomic_fetch_add(&g->generation, 1);
            pthread_mutex_lock(&lock); pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock);
        }
        g->watch_hash = h;
    }
}
static void *control_main(void *unused) {
    (void)unused;
    uint64_t total = hp_config.threads;
    for (hp_worker_group_t *g = groups; g; g = g->next) total += g->config.num;
    if (total > hp_config.max_threads || hp_config.max_buffered_response == 0 || hp_config.max_buffered_body == 0 ||
        hp_config.max_buffered_global < 16384 || !hp_config.queue_limit || !hp_config.request_timeout_ms) {
        fprintf(stderr, "php.runtime: invalid thread or buffer limits (total threads %llu, maximum %u)\n", (unsigned long long)total, hp_config.max_threads);
        goto failed_start;
    }
    bool watched = false;
    for (hp_worker_group_t *g = groups; g; g = g->next) if (g->config.num_watch) watched = true;
    if (watched) {
        char *entries;
        if (asprintf(&entries, "%sopcache.validate_timestamps=1\nopcache.revalidate_freq=0\n", hp_config.ini_entries ? hp_config.ini_entries : "") < 0)
            goto failed_start;
        free(hp_config.ini_entries); hp_config.ini_entries = entries;
        fprintf(stderr, "php.worker: watch enables OPcache timestamp validation with revalidate_freq=0\n");
    }
    if (hp_sapi_init(hp_config.ini_entries, (unsigned)total + 1)) goto failed_start;
    classic_threads = calloc(hp_config.threads, sizeof(pthread_t));
    if (!classic_threads) { hp_sapi_shutdown(); goto failed_start; }
    for (unsigned i = 0; i < hp_config.threads; ++i) {
        pthread_mutex_lock(&lock); ++alive; pthread_mutex_unlock(&lock);
        if (pthread_create(&classic_threads[i], NULL, classic_main, NULL)) { thread_ended(); break; }
        ++nclassic;
    }
    for (hp_worker_group_t *g = groups; g; g = g->next) {
        g->threads = calloc(g->config.num, sizeof(pthread_t));
        if (!g->threads) continue;
        for (unsigned i = 0; i < g->config.num; ++i) {
            pthread_mutex_lock(&lock); ++alive; pthread_mutex_unlock(&lock);
            atomic_fetch_add(&g->live, 1);
            if (pthread_create(&g->threads[i], NULL, worker_main, g)) { atomic_fetch_sub(&g->live, 1); thread_ended(); break; }
            ++g->started;
        }
    }
    unsigned spawned = nclassic;
    for (hp_worker_group_t *g = groups; g; g = g->next) spawned += g->started;
    if (spawned != total) {
        fprintf(stderr, "php.runtime: could not create all %llu configured execution threads\n", (unsigned long long)total);
        hp_runtime_begin_shutdown();
        for (unsigned i = 0; i < nclassic; ++i) pthread_join(classic_threads[i], NULL);
        for (hp_worker_group_t *g = groups; g; g = g->next)
            for (unsigned i = 0; i < g->started; ++i) pthread_join(g->threads[i], NULL);
        hp_sapi_shutdown(); goto failed_start;
    }
    pthread_mutex_lock(&lock); start_state = 2; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock);
    for (;;) {
        pthread_mutex_lock(&lock);
        bool stop = stop_requested;
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec++;
        while (!stop_requested && !stop) {
            int rc = pthread_cond_timedwait(&cond, &lock, &until);
            if (rc == ETIMEDOUT) break;
        }
        stop = stop_requested;
        pthread_mutex_unlock(&lock);
        monitor(); if (stop) break;
    }
    /* PHP producers must finish before the module and TSRM are shut down. */
    hp_runtime_begin_shutdown();
    pthread_mutex_lock(&lock);
    uint64_t drain_started = drain_started_ns;
    while (alive) {
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec++;
        pthread_cond_timedwait(&cond, &lock, &until);
        bool overdue = hp_now_ns() - drain_started >= hp_config.shutdown_timeout_ms * 1000000ULL;
        pthread_mutex_unlock(&lock);
        if (overdue) {
            pthread_mutex_lock(&lock);
            size_t n = atomic_load(&active_count), i = 0; hp_request_t **rs = calloc(n, sizeof(*rs));
            if (rs) for (hp_request_t *r = active; r && i < n; r = r->active_next) { hp_request_retain(r); rs[i++] = r; }
            pthread_mutex_unlock(&lock);
            for (size_t j = 0; j < i; ++j) { hp_request_cancel(rs[j]); hp_sapi_interrupt_request(rs[j]); hp_request_release(rs[j]); }
            free(rs); hp_sapi_interrupt_request(NULL);
        }
        monitor(); pthread_mutex_lock(&lock);
    }
    pthread_mutex_unlock(&lock);
    for (unsigned i = 0; i < nclassic; ++i) pthread_join(classic_threads[i], NULL);
    for (hp_worker_group_t *g = groups; g; g = g->next)
        for (unsigned i = 0; i < g->started; ++i) pthread_join(g->threads[i], NULL);
    hp_sapi_shutdown(); return NULL;
failed_start:
    pthread_mutex_lock(&lock); start_state = -1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock); return NULL;
}
int hp_runtime_start(void) {
    hp_runtime_config_defaults();
    pthread_mutex_lock(&lock);
    if (start_state == 2) { pthread_mutex_unlock(&lock); return 0; }
    start_state = 1;
    if (pthread_create(&controller, NULL, control_main, NULL)) { start_state = -1; pthread_mutex_unlock(&lock); return -1; }
    controller_created = true;
    while (start_state == 1) pthread_cond_wait(&cond, &lock);
    int rc = start_state == 2 ? 0 : -1; pthread_mutex_unlock(&lock); return rc;
}
void hp_runtime_stop(void) {
    pthread_mutex_lock(&lock);
    if (!controller_created) { pthread_mutex_unlock(&lock); return; }
    controller_created = false;
    if (!draining) drain_started_ns = hp_now_ns();
    draining = stop_requested = true; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock);
    pthread_join(controller, NULL);
}
void hp_metrics_json(char *out, size_t cap) {
    pthread_mutex_lock(&lock); unsigned q = queued, t = alive; pthread_mutex_unlock(&lock);
    unsigned scripts = 0, busy_workers = 0, bootstrapping = 0;
    for (hp_worker_group_t *g = groups; g; g = g->next) {
        unsigned l = atomic_load(&g->live), rd = atomic_load(&g->ready), b = atomic_load(&g->busy);
        scripts += l; busy_workers += b; if (l > rd + b) bootstrapping += l - rd - b;
    }
    snprintf(out, cap, "{\"active_php_threads\":%u,\"idle_php_threads\":%u,\"threads\":%u,\"active_worker_scripts\":%u,\"busy_workers\":%u,\"bootstrapping_workers\":%u,\"ready_workers\":%u,\"queued_requests\":%u,\"completed\":%llu,\"failed\":%llu,\"timed_out\":%llu,\"worker_restarts\":%llu,\"worker_startup_failures\":%llu,\"queue_wait_ns_total\":%llu,\"execution_ns_total\":%llu,\"buffered_bytes\":%zu}\n",
        atomic_load(&active_count), t >= atomic_load(&active_count) ? t - atomic_load(&active_count) : 0, t, scripts, busy_workers, bootstrapping, atomic_load(&ready_workers), q,
        (unsigned long long)atomic_load(&completed), (unsigned long long)atomic_load(&failed), (unsigned long long)atomic_load(&timed_out),
        (unsigned long long)atomic_load(&restarts), (unsigned long long)atomic_load(&startup_failures),
        (unsigned long long)atomic_load(&queue_ns), (unsigned long long)atomic_load(&execution_ns), hp_bridge_buffered_bytes());
}
