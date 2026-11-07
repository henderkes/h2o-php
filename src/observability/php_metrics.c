#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "php_metrics.h"
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Deliberately independent of H2O, Zend and the scheduler implementation. */
extern bool hp_process_is_fork_child(void);
_Static_assert(sizeof(double) == sizeof(uint64_t) && FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "metrics duration requires IEEE-754 binary64 storage");

enum { TOTAL, READY, BUSY, CRASHES, RESTARTS, TIME, COUNT, QUEUE, WORKER_FAMILIES };
struct hp_php_metric_worker {
    struct hp_php_metric_worker *next;
    char *label;
    _Atomic unsigned seen;
    _Atomic int64_t total, ready, busy, queue;
    _Atomic uint64_t crashes, restarts, time_bits, count;
};
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static hp_php_metric_worker_t *workers, **workers_tail = &workers;
static _Atomic bool active, opcache_available;
static bool started, collect_metrics;
static _Atomic pid_t owner_pid;
static locale_t numeric_locale;
static _Atomic int64_t total_threads, busy_threads, queue_depth;
static _Atomic uint64_t opcache_restarts[4];
static _Atomic bool opcache_unknown_seen;

static bool hot_enabled(void)
{
    return !hp_process_is_fork_child() && atomic_load_explicit(&active, memory_order_relaxed);
}
bool hp_php_metrics_owner_process(void)
{
    pid_t owner = atomic_load_explicit(&owner_pid, memory_order_relaxed);
    return !hp_process_is_fork_child() && (!owner || owner == getpid());
}
static bool valid_utf8(const unsigned char *p)
{
    /* Reject overlong forms, UTF-16 surrogates and values beyond U+10FFFF.
     * ASCII control bytes remain legal label data and are escaped on export. */
    while (*p) {
        unsigned a = *p++;
        if (a < 0x80) continue;
        if (a < 0xc2 || a > 0xf4) return false;
        unsigned b = *p;
        if (b < 0x80 || b > 0xbf) return false;
        ++p;
        if (a < 0xe0) continue;
        if ((a == 0xe0 && b < 0xa0) || (a == 0xed && b >= 0xa0)) return false;
        unsigned c = *p;
        if (c < 0x80 || c > 0xbf) return false;
        ++p;
        if (a < 0xf0) continue;
        if ((a == 0xf0 && b < 0x90) || (a == 0xf4 && b >= 0x90)) return false;
        unsigned d = *p;
        if (d < 0x80 || d > 0xbf) return false;
        ++p;
    }
    return true;
}
hp_php_metric_worker_t *hp_php_metrics_worker_register(const char *label)
{
    if (!hp_php_metrics_owner_process()) { errno = EPERM; return NULL; }
    if (!label || !valid_utf8((const unsigned char *)label)) { errno = EINVAL; return NULL; }
    pthread_mutex_lock(&registry_mutex);
    if (!atomic_load_explicit(&owner_pid, memory_order_relaxed))
        atomic_store_explicit(&owner_pid, getpid(), memory_order_relaxed);
    for (hp_php_metric_worker_t *w = workers; w; w = w->next) {
        if (!strcmp(label, w->label)) { pthread_mutex_unlock(&registry_mutex); return w; }
    }
    hp_php_metric_worker_t *w = calloc(1, sizeof(*w));
    if (w && !(w->label = strdup(label))) { free(w); w = NULL; }
    if (w) {
        atomic_init(&w->seen, 0);
        atomic_init(&w->total, 0); atomic_init(&w->ready, 0);
        atomic_init(&w->busy, 0); atomic_init(&w->queue, 0);
        atomic_init(&w->crashes, 0); atomic_init(&w->restarts, 0);
        atomic_init(&w->time_bits, 0); atomic_init(&w->count, 0);
        *workers_tail = w; workers_tail = &w->next;
    }
    pthread_mutex_unlock(&registry_mutex);
    return w;
}
int hp_php_metrics_start(unsigned initial_total, bool collect)
{
    if (!hp_php_metrics_owner_process()) { errno = EPERM; return -1; }
    pthread_mutex_lock(&registry_mutex);
    if (started) { pthread_mutex_unlock(&registry_mutex); errno = EALREADY; return -1; }
    if (!collect) {
        /* Collection policy is fixed before PHP threads start. Keep mandatory
         * admission, budget and JSON accounting outside this collector. */
        atomic_store_explicit(&owner_pid, getpid(), memory_order_relaxed);
        started = true;
        pthread_mutex_unlock(&registry_mutex); return 0;
    }
    numeric_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (!numeric_locale) { pthread_mutex_unlock(&registry_mutex); return -1; }
    /* Do not hide an inherited lock inside a non-lock-free atomic on another
     * ABI. This deployment is x86-64; other targets need an explicit review. */
    if (!atomic_is_lock_free(&busy_threads) || !atomic_is_lock_free(&opcache_restarts[0])) {
        freelocale(numeric_locale); numeric_locale = (locale_t)0;
        pthread_mutex_unlock(&registry_mutex); errno = ENOTSUP; return -1;
    }
    atomic_store_explicit(&owner_pid, getpid(), memory_order_relaxed);
    atomic_store_explicit(&total_threads, initial_total, memory_order_relaxed);
    started = collect_metrics = true;
    atomic_store_explicit(&active, true, memory_order_release);
    pthread_mutex_unlock(&registry_mutex);
    return 0;
}
void hp_php_metrics_shutdown(void)
{
    if (hp_php_metrics_owner_process()) atomic_store_explicit(&active, false, memory_order_release);
    /* Handles/locale intentionally live until process exit; a scrape which
     * already entered can safely finish while shutdown unregisters families. */
}
void hp_php_metrics_opcache_available(bool available)
{
    if (hp_php_metrics_owner_process()) atomic_store_explicit(&opcache_available, available, memory_order_release);
}
static void seen(hp_php_metric_worker_t *w, unsigned family)
{
    atomic_fetch_or_explicit(&w->seen, 1U << family, memory_order_relaxed);
}
void hp_php_metrics_worker_start(hp_php_metric_worker_t *w)
{
    if (!hot_enabled()) return;
    atomic_fetch_add_explicit(&busy_threads, 1, memory_order_relaxed);
    if (w) { atomic_fetch_add_explicit(&w->total, 1, memory_order_relaxed); seen(w, TOTAL); }
}
void hp_php_metrics_worker_ready(hp_php_metric_worker_t *w)
{
    if (!hot_enabled() || !w) return;
    atomic_fetch_add_explicit(&w->ready, 1, memory_order_relaxed); seen(w, READY);
}
void hp_php_metrics_worker_stop(hp_php_metric_worker_t *w, hp_php_metric_stop_t reason)
{
    if (!hot_enabled()) return;
    atomic_fetch_sub_explicit(&busy_threads, 1, memory_order_relaxed);
    if (!w) return;
    atomic_fetch_sub_explicit(&w->total, 1, memory_order_relaxed); seen(w, TOTAL);
    if (reason != HP_PHP_METRIC_BOOT_FAILURE) {
        atomic_fetch_sub_explicit(&w->ready, 1, memory_order_relaxed); seen(w, READY);
    }
    if (reason == HP_PHP_METRIC_CRASH || reason == HP_PHP_METRIC_BOOT_FAILURE) {
        atomic_fetch_add_explicit(&w->crashes, 1, memory_order_relaxed); seen(w, CRASHES);
    } else if (reason == HP_PHP_METRIC_RESTART) {
        atomic_fetch_add_explicit(&w->restarts, 1, memory_order_relaxed); seen(w, RESTARTS);
    }
}
void hp_php_metrics_request_begin_locked(hp_php_metric_request_t *r, hp_php_metric_worker_t *w, uint64_t origin_ns)
{
    if (!hot_enabled() || r->begun) return;
    r->worker = w; r->origin_ns = origin_ns; r->begun = true;
    if (w) { atomic_fetch_add_explicit(&w->busy, 1, memory_order_relaxed); seen(w, BUSY); }
    else atomic_fetch_add_explicit(&busy_threads, 1, memory_order_relaxed);
}
void hp_php_metrics_request_queued_locked(hp_php_metric_request_t *r)
{
    if (!hot_enabled() || !r->begun || r->queued || r->dispatched || r->ended) return;
    r->queued = true;
    if (r->worker) { atomic_fetch_add_explicit(&r->worker->queue, 1, memory_order_relaxed); seen(r->worker, QUEUE); }
    else atomic_fetch_add_explicit(&queue_depth, 1, memory_order_relaxed);
}
static void dequeue(hp_php_metric_request_t *r)
{
    if (!r->queued) return;
    r->queued = false;
    if (r->worker) atomic_fetch_sub_explicit(&r->worker->queue, 1, memory_order_relaxed);
    else atomic_fetch_sub_explicit(&queue_depth, 1, memory_order_relaxed);
}
void hp_php_metrics_request_dispatched_locked(hp_php_metric_request_t *r)
{
    if (!hot_enabled() || !r->begun || r->ended) return;
    dequeue(r); r->dispatched = true;
}
static void add_seconds(_Atomic uint64_t *bits, double seconds)
{
    uint64_t old_bits = atomic_load_explicit(bits, memory_order_relaxed), new_bits;
    do {
        double value;
        memcpy(&value, &old_bits, sizeof(value));
        value += seconds;
        memcpy(&new_bits, &value, sizeof(new_bits));
    } while (!atomic_compare_exchange_weak_explicit(bits, &old_bits, new_bits, memory_order_relaxed, memory_order_relaxed));
}
void hp_php_metrics_request_end_locked(hp_php_metric_request_t *r, uint64_t now_ns)
{
    if (!hot_enabled() || !r->begun || r->ended) return;
    r->ended = true; dequeue(r);
    if (!r->worker) { atomic_fetch_sub_explicit(&busy_threads, 1, memory_order_relaxed); return; }
    hp_php_metric_worker_t *w = r->worker;
    atomic_fetch_sub_explicit(&w->busy, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&w->count, 1, memory_order_relaxed);
    double seconds = now_ns >= r->origin_ns ? (double)(now_ns - r->origin_ns) / 1e9 : 0;
    add_seconds(&w->time_bits, seconds);
    seen(w, COUNT); seen(w, TIME);
}
void hp_php_metrics_request_cancel_locked(hp_php_metric_request_t *r, uint64_t now_ns)
{
    if (!r->dispatched) hp_php_metrics_request_end_locked(r, now_ns);
}
int64_t hp_php_metrics_queue_depth(hp_php_metric_worker_t *w)
{
    if (!hot_enabled()) return 0;
    return atomic_load_explicit(w ? &w->queue : &queue_depth, memory_order_relaxed);
}
void hp_php_metrics_opcache_restart(int reason)
{
    if (!hp_php_metrics_owner_process() || !atomic_load_explicit(&active, memory_order_relaxed) ||
        !atomic_load_explicit(&opcache_available, memory_order_acquire)) return;
    unsigned index = reason >= 0 && reason < 3 ? (unsigned)reason : 3;
    atomic_fetch_add_explicit(&opcache_restarts[index], 1, memory_order_relaxed);
    if (index == 3) atomic_store_explicit(&opcache_unknown_seen, true, memory_order_relaxed);
}

typedef struct { char *data; size_t length, capacity; bool failed; } text_t;
static void append(text_t *t, const char *data, size_t length)
{
    if (t->failed) return;
    if (length > SIZE_MAX - t->length - 1) { t->failed = true; errno = EOVERFLOW; return; }
    size_t needed = t->length + length + 1;
    if (needed > t->capacity) {
        size_t capacity = t->capacity ? t->capacity : 1024;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        char *next = realloc(t->data, capacity);
        if (!next) { t->failed = true; return; }
        t->data = next; t->capacity = capacity;
    }
    memcpy(t->data + t->length, data, length); t->length += length; t->data[t->length] = 0;
}
static void literal(text_t *t, const char *value) { append(t, value, strlen(value)); }
static void format(text_t *t, const char *fmt, ...)
{
    char buffer[160]; va_list args; va_start(args, fmt);
    int length = vsnprintf(buffer, sizeof(buffer), fmt, args); va_end(args);
    if (length < 0 || (size_t)length >= sizeof(buffer)) { t->failed = true; errno = EOVERFLOW; return; }
    append(t, buffer, (size_t)length);
}
static void escaped(text_t *t, const char *s, bool label)
{
    for (; *s; ++s) {
        if (*s == '\\') literal(t, "\\\\");
        else if (*s == '\n') literal(t, "\\n");
        else if (label && *s == '"') literal(t, "\\\"");
        else append(t, s, 1);
    }
}
static void family(text_t *t, const char *name, const char *help, const char *type)
{
    literal(t, "# HELP "); literal(t, name); literal(t, " "); escaped(t, help, false);
    literal(t, "\n# TYPE "); literal(t, name); literal(t, " "); literal(t, type); literal(t, "\n");
}
static const struct { const char *name, *help, *type; } worker_family[WORKER_FAMILIES] = {
    {"frankenphp_total_workers", "Total number of PHP workers for this worker", "gauge"},
    {"frankenphp_ready_workers", "Running workers that have successfully called frankenphp_handle_request at least once", "gauge"},
    {"frankenphp_busy_workers", "Number of busy PHP workers for this worker", "gauge"},
    {"frankenphp_worker_crashes", "Number of PHP worker crashes for this worker", "counter"},
    {"frankenphp_worker_restarts", "Number of PHP worker restarts for this worker", "counter"},
    {"frankenphp_worker_request_time", "", "counter"},
    {"frankenphp_worker_request_count", "", "counter"},
    {"frankenphp_worker_queue_depth", "", "gauge"}
};
static void worker_value(text_t *t, hp_php_metric_worker_t *w, unsigned index)
{
    switch (index) {
    case TOTAL: format(t, "%" PRId64, atomic_load_explicit(&w->total, memory_order_relaxed)); break;
    case READY: format(t, "%" PRId64, atomic_load_explicit(&w->ready, memory_order_relaxed)); break;
    case BUSY: format(t, "%" PRId64, atomic_load_explicit(&w->busy, memory_order_relaxed)); break;
    case QUEUE: format(t, "%" PRId64, atomic_load_explicit(&w->queue, memory_order_relaxed)); break;
    case CRASHES: format(t, "%" PRIu64, atomic_load_explicit(&w->crashes, memory_order_relaxed)); break;
    case RESTARTS: format(t, "%" PRIu64, atomic_load_explicit(&w->restarts, memory_order_relaxed)); break;
    case COUNT: format(t, "%" PRIu64, atomic_load_explicit(&w->count, memory_order_relaxed)); break;
    case TIME: {
        uint64_t bits = atomic_load_explicit(&w->time_bits, memory_order_relaxed); double value;
        memcpy(&value, &bits, sizeof(value));
        if (isinf(value)) literal(t, value > 0 ? "+Inf" : "-Inf");
        else if (isnan(value)) literal(t, "NaN");
        else format(t, "%.17g", value);
        break;
    }
    }
}
int hp_php_metrics_prometheus(char **output, size_t *length)
{
    if (!output || !length) { errno = EINVAL; return -1; }
    *output = NULL; *length = 0;
    if (!hp_php_metrics_owner_process()) { errno = EPERM; return -1; }
    pthread_mutex_lock(&registry_mutex);
    if (started && !collect_metrics) {
        /* Do not advertise synthetic zero families when collection is OFF. */
        *output = strdup("");
        pthread_mutex_unlock(&registry_mutex); return *output ? 0 : -1;
    }
    if (!atomic_load_explicit(&active, memory_order_acquire) || !numeric_locale) {
        pthread_mutex_unlock(&registry_mutex); errno = ENOENT; return -1;
    }
    locale_t previous = uselocale(numeric_locale);
    if (!previous) { pthread_mutex_unlock(&registry_mutex); return -1; }
    text_t t = {0};
    family(&t, "frankenphp_total_threads", "Total number of PHP threads", "gauge");
    format(&t, "frankenphp_total_threads %" PRId64 "\n", atomic_load_explicit(&total_threads, memory_order_relaxed));
    family(&t, "frankenphp_busy_threads", "Number of busy PHP threads", "gauge");
    format(&t, "frankenphp_busy_threads %" PRId64 "\n", atomic_load_explicit(&busy_threads, memory_order_relaxed));
    family(&t, "frankenphp_queue_depth", "Number of regular queued requests", "gauge");
    format(&t, "frankenphp_queue_depth %" PRId64 "\n", atomic_load_explicit(&queue_depth, memory_order_relaxed));
    for (unsigned index = 0; index < WORKER_FAMILIES; ++index) {
        bool present = false;
        for (hp_php_metric_worker_t *w = workers; w; w = w->next)
            if (atomic_load_explicit(&w->seen, memory_order_relaxed) & (1U << index)) { present = true; break; }
        if (!present) continue;
        family(&t, worker_family[index].name, worker_family[index].help, worker_family[index].type);
        for (hp_php_metric_worker_t *w = workers; w; w = w->next) {
            if (!(atomic_load_explicit(&w->seen, memory_order_relaxed) & (1U << index))) continue;
            literal(&t, worker_family[index].name); literal(&t, "{worker=\""); escaped(&t, w->label, true);
            literal(&t, "\"} "); worker_value(&t, w, index); literal(&t, "\n");
        }
    }
    if (atomic_load_explicit(&opcache_available, memory_order_acquire)) {
        family(&t, "frankenphp_opcache_restarts", "Number of restarts of opcache's shared memory scheduled, by reason (experimental, should stay at zero)", "counter");
        const char *reasons[] = {"oom", "hash", "manual", "unknown"};
        unsigned n = atomic_load_explicit(&opcache_unknown_seen, memory_order_relaxed) ? 4 : 3;
        for (unsigned i = 0; i < n; ++i)
            format(&t, "frankenphp_opcache_restarts{reason=\"%s\"} %" PRIu64 "\n", reasons[i], atomic_load_explicit(&opcache_restarts[i], memory_order_relaxed));
    }
    int saved_errno = errno;
    (void)uselocale(previous);
    pthread_mutex_unlock(&registry_mutex);
    if (t.failed) { free(t.data); errno = saved_errno ? saved_errno : ENOMEM; return -1; }
    *output = t.data; *length = t.length; return 0;
}
