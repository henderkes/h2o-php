#define _GNU_SOURCE
#include "h2o_php.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdio.h>

static atomic_uint_fast64_t next_id = 1;
static pthread_mutex_t budget_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t budget_cond = PTHREAD_COND_INITIALIZER;
static size_t buffered;
static atomic_size_t budget_waiters = 0;

uint64_t hp_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}
static struct timespec deadline(hp_request_t *r) {
    uint64_t ns = r->deadline_ns;
    return (struct timespec){ .tv_sec = ns / 1000000000ULL, .tv_nsec = ns % 1000000000ULL };
}
static void budget_release(size_t n) {
    pthread_mutex_lock(&budget_mutex);
    if (buffered < n) abort();
    buffered -= n;
    pthread_cond_broadcast(&budget_cond);
    pthread_mutex_unlock(&budget_mutex);
}
static int budget_try(size_t n) {
    int ok = 0;
    pthread_mutex_lock(&budget_mutex);
    if (n <= hp_config.max_buffered_global && buffered <= hp_config.max_buffered_global - n) {
        buffered += n; ok = 1;
    }
    pthread_mutex_unlock(&budget_mutex);
    return ok;
}
/* Caller holds r->mutex, never budget_mutex. The mutex serializes the
 * false->true transition, so only its first publisher needs a budget wake. */
static void stop_budget_locked(hp_request_t *r) {
    if (atomic_load_explicit(&r->budget_stopped, memory_order_relaxed)) return;
    /* SC pairs with W registration and its mandatory stop recheck: either
     * this load sees a waiter, or that waiter sees the published stop. */
    atomic_store_explicit(&r->budget_stopped, true, memory_order_seq_cst);
    if (atomic_load_explicit(&budget_waiters, memory_order_seq_cst) == 0) return;
    pthread_mutex_lock(&budget_mutex);
    pthread_cond_broadcast(&budget_cond);
    pthread_mutex_unlock(&budget_mutex);
}
static void budget_wait_leave(void) {
    if (atomic_fetch_sub_explicit(&budget_waiters, 1, memory_order_seq_cst) == 0) abort();
}
size_t hp_bridge_buffered_bytes(void) {
    pthread_mutex_lock(&budget_mutex); size_t n = buffered; pthread_mutex_unlock(&budget_mutex); return n;
}
hp_request_t *hp_request_new(void) {
    hp_request_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    atomic_init(&r->refs, 1); atomic_init(&r->notification_pending, false);
    atomic_init(&r->budget_stopped, false);
    pthread_mutex_init(&r->mutex, NULL);
    pthread_condattr_t attr; pthread_condattr_init(&attr); pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&r->body_cond, &attr); pthread_cond_init(&r->output_cond, &attr); pthread_condattr_destroy(&attr);
    r->id = atomic_fetch_add(&next_id, 1); r->created_ns = hp_now_ns();
    r->deadline_ns = r->created_ns + hp_config.request_timeout_ms * 1000000ULL;
    r->body_limit = hp_config.max_buffered_body; r->output_limit = hp_config.max_buffered_response;
    r->content_length = SIZE_MAX; r->status = 200; r->state = HP_CREATED;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); r->request_time = ts.tv_sec + ts.tv_nsec / 1e9;
    return r;
}
void hp_request_retain(hp_request_t *r) { atomic_fetch_add_explicit(&r->refs, 1, memory_order_relaxed); }
void hp_chunk_free(hp_chunk_t *p) { if (p) { budget_release(p->len); free(p); } }
void hp_chunks_free(hp_chunk_t *p) {
    size_t bytes = 0;
    while (p) { hp_chunk_t *next = p->next; bytes += p->len; free(p); p = next; }
    if (bytes) budget_release(bytes);
}
void hp_response_release_chunks(hp_request_t *r, hp_chunk_t *p) {
    size_t bytes = 0;
    while (p) { hp_chunk_t *next = p->next; bytes += p->len; free(p); p = next; }
    if (!bytes) return;
    /* Caller still owns the request. No chunk is released until its H2O send
     * completes/stops, or the owning loop discards a bodyless response batch. */
    pthread_mutex_lock(&r->mutex);
    if (r->output_inflight < bytes) abort();
    r->output_inflight -= bytes;
    budget_release(bytes); /* Nonwaiting request -> global-budget lock order. */
    pthread_cond_broadcast(&r->output_cond);
    pthread_mutex_unlock(&r->mutex);
}
static bool output_unreserve(hp_request_t *r, size_t bytes) {
    pthread_mutex_lock(&r->mutex);
    if (r->output_bytes < bytes) abort();
    r->output_bytes -= bytes;
    bool finished = r->response_finished;
    pthread_cond_broadcast(&r->output_cond);
    pthread_mutex_unlock(&r->mutex);
    return finished;
}
void hp_headers_free(hp_header_t *p) {
    while (p) { hp_header_t *n = p->next; free(p->name); free(p->value); free(p); p = n; }
}
int hp_header_add(hp_header_t **list, const char *name, const char *value) {
    hp_header_t *p = calloc(1, sizeof(*p));
    if (!p) return -1;
    p->name = strdup(name ? name : ""); p->value = strdup(value ? value : "");
    if (!p->name || !p->value) { hp_headers_free(p); return -1; }
    while (*list) list = &(*list)->next;
    *list = p; return 0;
}
hp_header_t *hp_headers_clone(const hp_header_t *p) {
    hp_header_t *out = NULL;
    for (; p; p = p->next) if (hp_header_add(&out, p->name, p->value)) { hp_headers_free(out); return NULL; }
    return out;
}
const char *hp_header_get(const hp_header_t *p, const char *key) {
    for (; p; p = p->next) if (!strcasecmp(p->name, key)) return p->value;
    return NULL;
}
void hp_request_release(hp_request_t *r) {
    if (atomic_fetch_sub_explicit(&r->refs, 1, memory_order_acq_rel) != 1) return;
    if (r->document_uri_owned) free(r->document_uri);
    free(r->method); free(r->uri); free(r->query); free(r->protocol); free(r->script); free(r->script_name);
    free(r->path_info); free(r->document_root); free(r->remote_addr); free(r->remote_port);
    free(r->server_name); free(r->server_port); free(r->content_type); free(r->cookie);
    hp_headers_free(r->headers); hp_headers_free(r->env); hp_headers_free(r->response_headers);
    while (r->body_head) { hp_chunk_t *p = r->body_head; r->body_head = p->next; hp_chunk_free(p); }
    while (r->output_head) { hp_chunk_t *p = r->output_head; r->output_head = p->next; hp_chunk_free(p); }
    while (r->info_head) { hp_info_t *p = r->info_head; r->info_head = p->next; hp_headers_free(p->headers); free(p); }
    pthread_cond_destroy(&r->body_cond); pthread_cond_destroy(&r->output_cond); pthread_mutex_destroy(&r->mutex); free(r);
}
void hp_request_notify(hp_request_t *r) { if (r->notify) r->notify(r); }
void hp_request_cancel(hp_request_t *r) {
    pthread_mutex_lock(&r->mutex);
    r->cancelled = true;
    hp_php_metrics_request_cancel_locked(&r->php_metrics, hp_now_ns());
    if (r->state != HP_COMPLETED && r->state != HP_TIMED_OUT) r->state = HP_CANCELLED;
    stop_budget_locked(r);
    pthread_cond_broadcast(&r->body_cond); pthread_cond_broadcast(&r->output_cond);
    pthread_mutex_unlock(&r->mutex);
}
static void timeout_locked(hp_request_t *r) {
    r->cancelled = true; r->state = HP_TIMED_OUT;
    hp_php_metrics_request_cancel_locked(&r->php_metrics, hp_now_ns());
    stop_budget_locked(r);
    pthread_cond_broadcast(&r->body_cond); pthread_cond_broadcast(&r->output_cond);
}
int hp_request_body_push(hp_request_t *r, const void *data, size_t len, bool eof) {
    pthread_mutex_lock(&r->mutex);
    if (r->cancelled) { pthread_mutex_unlock(&r->mutex); return -1; }
    /* Finish can race the owning loop's earlier room snapshot. The input is
     * no longer visible to PHP; accepting it here means discard, not copy. */
    if (r->response_finished) { pthread_mutex_unlock(&r->mutex); return 0; }
    if (r->body_eof || len > r->body_limit || r->body_bytes > r->body_limit - len) {
        pthread_mutex_unlock(&r->mutex); return -1;
    }
    if (len) {
        if (!budget_try(len)) { pthread_mutex_unlock(&r->mutex); return -1; }
        hp_chunk_t *p = malloc(sizeof(*p) + len);
        if (!p) { budget_release(len); pthread_mutex_unlock(&r->mutex); return -1; }
        p->next = NULL; p->len = len; p->offset = 0; memcpy(p->data, data, len);
        if (r->body_tail) r->body_tail->next = p; else r->body_head = p;
        r->body_tail = p; r->body_bytes += len;
    }
    r->body_eof = eof; pthread_cond_broadcast(&r->body_cond); pthread_mutex_unlock(&r->mutex); return 0;
}
bool hp_request_body_pause(hp_request_t *r) {
    pthread_mutex_lock(&r->mutex);
    /* Arming and testing share the read-side mutex: a read before this test
     * makes the owner retry immediately; a later crossing queues the wake. */
    bool wait = !r->cancelled && !r->body_eof && r->body_bytes > r->body_limit / 2;
    r->body_producer_blocked = wait;
    pthread_mutex_unlock(&r->mutex);
    return wait;
}
ssize_t hp_request_body_read(hp_request_t *r, void *out, size_t len) {
    if (!len) return 0;
    pthread_mutex_lock(&r->mutex);
    while (!r->body_head && !r->body_eof && !r->response_finished && !r->cancelled) {
        struct timespec ts = deadline(r);
        if (pthread_cond_timedwait(&r->body_cond, &r->mutex, &ts) == ETIMEDOUT) timeout_locked(r);
    }
    if (r->response_finished) { pthread_mutex_unlock(&r->mutex); return 0; }
    if (r->cancelled) { pthread_mutex_unlock(&r->mutex); hp_request_notify(r); return -1; }
    size_t n = 0;
    while (len && r->body_head) {
        hp_chunk_t *p = r->body_head; size_t take = p->len - p->offset;
        if (take > len) take = len;
        memcpy((char *)out + n, p->data + p->offset, take); p->offset += take;
        n += take; len -= take; r->body_bytes -= take;
        if (p->offset == p->len) { r->body_head = p->next; if (!r->body_head) r->body_tail = NULL; hp_chunk_free(p); }
    }
    bool resume = r->body_producer_blocked && r->body_bytes <= r->body_limit / 2;
    if (resume) r->body_producer_blocked = false;
    pthread_mutex_unlock(&r->mutex);
    if (resume) hp_request_notify(r);
    return (ssize_t)n;
}
ssize_t hp_response_write(hp_request_t *r, const void *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t n = len - sent; if (n > 16384) n = 16384; if (n > r->output_limit) n = r->output_limit;
        if (!n) return -1;
        pthread_mutex_lock(&r->mutex);
        while (!r->cancelled && !r->response_finished &&
               (r->output_inflight > r->output_limit - n ||
                r->output_bytes > r->output_limit - n - r->output_inflight)) {
            struct timespec ts = deadline(r);
            if (pthread_cond_timedwait(&r->output_cond, &r->mutex, &ts) == ETIMEDOUT) timeout_locked(r);
        }
        if (r->response_finished) { pthread_mutex_unlock(&r->mutex); return (ssize_t)len; }
        if (r->cancelled) { pthread_mutex_unlock(&r->mutex); hp_request_notify(r); return -1; }
        /* Reserve before dropping the lock for global-budget/allocation work.
         * Other producers and detached sends cannot consume the same credit. */
        r->output_bytes += n;
        pthread_mutex_unlock(&r->mutex);
        /* Global budget waits use a condition, without holding request locks. */
        pthread_mutex_lock(&budget_mutex);
        bool registered = false;
        while (!atomic_load_explicit(&r->budget_stopped, memory_order_seq_cst) &&
               buffered > hp_config.max_buffered_global - n) {
            if (!registered) {
                atomic_fetch_add_explicit(&budget_waiters, 1, memory_order_seq_cst);
                registered = true;
            }
            /* Stop may have observed W==0 after our first predicate check.
             * Recheck after registration before atomically entering wait. */
            if (atomic_load_explicit(&r->budget_stopped, memory_order_seq_cst)) break;
            struct timespec realtime; clock_gettime(CLOCK_REALTIME, &realtime); realtime.tv_sec++;
            pthread_cond_timedwait(&budget_cond, &budget_mutex, &realtime);
            pthread_mutex_unlock(&budget_mutex);
            pthread_mutex_lock(&r->mutex); bool stop = r->cancelled || r->response_finished;
            if (!stop && hp_now_ns() >= r->deadline_ns) { timeout_locked(r); stop = true; }
            pthread_mutex_unlock(&r->mutex);
            if (stop) {
                /* Registration spans the B-unlock/R-check interval. This
                 * terminal path never waits again, so retire it here once. */
                budget_wait_leave();
                bool finished = output_unreserve(r, n);
                hp_request_notify(r); return finished ? (ssize_t)len : -1;
            }
            pthread_mutex_lock(&budget_mutex);
        }
        /* Keep the SC stop check before charging; no request mutex under B.
         * With no capacity wait, W has not been incremented/decremented. */
        bool stopped = atomic_load_explicit(&r->budget_stopped, memory_order_seq_cst);
        if (registered) budget_wait_leave();
        if (!stopped) buffered += n;
        pthread_mutex_unlock(&budget_mutex);
        if (stopped) {
            bool finished = output_unreserve(r, n);
            hp_request_notify(r); return finished ? (ssize_t)len : -1;
        }
        hp_chunk_t *p = malloc(sizeof(*p) + n);
        if (!p) {
            budget_release(n);
            return output_unreserve(r, n) ? (ssize_t)len : -1;
        }
        p->next = NULL; p->len = n; p->offset = 0; memcpy(p->data, (const char *)data + sent, n);
        pthread_mutex_lock(&r->mutex);
        if (r->cancelled || r->response_finished) {
            pthread_mutex_unlock(&r->mutex); hp_chunk_free(p);
            return output_unreserve(r, n) ? (ssize_t)len : -1;
        }
        if (r->output_tail) r->output_tail->next = p; else r->output_head = p;
        r->output_tail = p; r->state = HP_RESPONDING; /* Credit was reserved above. */
        /* Enqueue and the consumer's empty-to-idle transition share this lock.
         * An active send/proceed path will drain newly queued output itself. */
        bool notify = !r->output_consumer_active;
        pthread_mutex_unlock(&r->mutex); sent += n;
        if (notify) hp_request_notify(r);
    }
    return (ssize_t)sent;
}
void hp_response_flush_notify(hp_request_t *r) {
    pthread_mutex_lock(&r->mutex);
    r->output_flush_requested = true;
    bool notify = !r->output_consumer_active;
    pthread_mutex_unlock(&r->mutex);
    if (notify) hp_request_notify(r);
}
int hp_response_headers(hp_request_t *r, int status, const hp_header_t *headers, bool info) {
    /* HTTP 101 requires a protocol upgrade, not an informational response. */
    if (info && (status < 100 || status >= 200 || status == 101)) return -1;
    /* A final 101 is only defined on HTTP/1.1; HTTP/2 and HTTP/3 prohibit it. */
    if (!info && status == 101 && (!r->protocol || strcmp(r->protocol, "HTTP/1.1"))) return -1;
    hp_header_t *copy = hp_headers_clone(headers);
    if (headers && !copy) return -1;
    pthread_mutex_lock(&r->mutex);
    if (r->cancelled || r->response_finished) { pthread_mutex_unlock(&r->mutex); hp_headers_free(copy); return -1; }
    if (r->headers_ready && r->status == 101) {
        /* A committed final 101 is terminal: any later header call (info or
         * final, before or after the owning loop takes the headers) succeeds
         * idempotently; the incoming set is dropped, never queued. */
        pthread_mutex_unlock(&r->mutex); hp_headers_free(copy); return 0;
    }
    if (r->headers_taken) { pthread_mutex_unlock(&r->mutex); hp_headers_free(copy); return -1; }
    if (info) {
        unsigned count = 0; for (hp_info_t *i = r->info_head; i; i = i->next) count++;
        if (count >= 16) { pthread_mutex_unlock(&r->mutex); hp_headers_free(copy); return -1; }
        hp_info_t *p = calloc(1, sizeof(*p));
        if (!p) { pthread_mutex_unlock(&r->mutex); hp_headers_free(copy); return -1; }
        p->status = status; p->headers = copy;
        if (r->info_tail) r->info_tail->next = p; else r->info_head = p;
        r->info_tail = p;
    } else {
        hp_headers_free(r->response_headers); r->response_headers = copy; r->status = status; r->headers_ready = true;
    }
    pthread_mutex_unlock(&r->mutex); hp_request_notify(r); return 0;
}
void hp_response_finish(hp_request_t *r) {
    pthread_mutex_lock(&r->mutex);
    r->response_finished = true; r->headers_ready = true;
    hp_php_metrics_request_end_locked(&r->php_metrics, hp_now_ns());
    stop_budget_locked(r);
    /* This is application input EOF, distinct from the owning generator's
     * actual transport EOF. Cached PHP stream bytes remain PHP-owned. */
    hp_chunk_t *body = r->body_head;
    r->body_head = r->body_tail = NULL; r->body_bytes = 0;
    r->body_eof = true; r->body_producer_blocked = false;
    pthread_cond_broadcast(&r->body_cond);
    pthread_cond_broadcast(&r->output_cond);
    pthread_mutex_unlock(&r->mutex);
    /* Detached chunks have one owner; return their global credit outside the
     * request lock. Network input still drains through its owning H2O loop. */
    hp_chunks_free(body);
    hp_request_notify(r);
}
hp_chunk_t *hp_response_pop(hp_request_t *r) {
    /* Preserve the original diagnostic API: detached single chunks are freed
     * with hp_chunk_free and do not participate in batch in-flight accounting. */
    pthread_mutex_lock(&r->mutex);
    r->output_consumer_active = false; /* Legacy pop makes no proceed commitment. */
    hp_chunk_t *p = r->output_head;
    if (p) {
        r->output_head = p->next;
        if (!r->output_head) r->output_tail = NULL;
        p->next = NULL; r->output_bytes -= p->len;
        pthread_cond_broadcast(&r->output_cond);
    }
    pthread_mutex_unlock(&r->mutex);
    return p;
}
hp_chunk_t *hp_response_pop_batch(hp_request_t *r, size_t max_chunks, size_t max_bytes, bool *finished) {
    pthread_mutex_lock(&r->mutex);
    hp_chunk_t *head = r->output_head, *tail = NULL;
    size_t count = 0, bytes = 0;
    while (r->output_head && count < max_chunks) {
        hp_chunk_t *p = r->output_head;
        /* Always permit the first chunk so an unusually small caller limit
         * cannot strand output. Normal bridge chunks are at most 16KiB. */
        if (count && p->len > max_bytes - bytes) break;
        tail = p; r->output_head = p->next; bytes += p->len; count++;
        if (bytes >= max_bytes) break;
    }
    if (tail) {
        tail->next = NULL;
        if (!r->output_head) r->output_tail = NULL;
        r->output_bytes -= bytes;
        r->output_inflight += bytes; /* Credit stays held through send completion. */
    } else head = NULL;
    *finished = r->response_finished && !r->output_head;
    /* Do not clear on detaching the last queued chunk: an open response has
     * an H2O proceed continuation; a finished response accepts no more writes.
     * An empty pop publishes false so the next enqueue must queue a wake. */
    r->output_consumer_active = head != NULL;
    pthread_mutex_unlock(&r->mutex);
    return head;
}
