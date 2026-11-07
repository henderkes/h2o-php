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
/* Input copies cross from H2O loops to PHP threads. Reuse a small shared
 * set instead of repeatedly freeing their pages after that handoff.
 * Live capacity and idle capacity share the global budget. Idle capacity is
 * immediately evictable and is not reported as queued/in-flight payload. */
#define INPUT_RECYCLE_MIN (256 * 1024)
#define INPUT_RECYCLE_MAX (1024 * 1024)
#define INPUT_RECYCLE_LIMIT (8 * 1024 * 1024)
static hp_chunk_t *input_recycled;
static size_t input_recycled_bytes;
static size_t input_recycled_count;
static bool input_recycle_stopped;

/* budget_mutex held; no request locks acquired here. */
static void input_evict_one(void) {
    hp_chunk_t *p = input_recycled;
    input_recycled = p->next;
    --input_recycled_count;
    input_recycled_bytes -= p->capacity;
    free(p);
}
/* budget_mutex held; keep both idle bounds valid when the budget shrinks. */
static size_t input_recycle_trim(void) {
    size_t limit = hp_config.max_buffered_global / 4;
    if (limit > INPUT_RECYCLE_LIMIT) limit = INPUT_RECYCLE_LIMIT;
    while (input_recycled && (input_recycled_bytes > limit ||
           input_recycled_count > limit / INPUT_RECYCLE_MIN)) input_evict_one();
    return limit;
}
static bool budget_room_locked(size_t n) {
    (void)input_recycle_trim();
    if (n > hp_config.max_buffered_global || buffered > hp_config.max_buffered_global - n) return false;
    size_t idle_room = hp_config.max_buffered_global - n - buffered;
    while (input_recycled && input_recycled_bytes > idle_room) input_evict_one();
    return true;
}
void hp_bridge_cleanup(void) {
    pthread_mutex_lock(&budget_mutex);
    input_recycle_stopped = true;
    while (input_recycled) input_evict_one();
    pthread_mutex_unlock(&budget_mutex);
}

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
static hp_chunk_t *input_chunk_new(size_t len, size_t reserve) {
    size_t capacity = len;
    if (len >= INPUT_RECYCLE_MIN && len <= INPUT_RECYCLE_MAX) {
        capacity = INPUT_RECYCLE_MIN;
        while (capacity < len) capacity *= 2;
    }
    pthread_mutex_lock(&budget_mutex);
    /* Rounding is optional. Keep room for the rest of this request's body
     * window and one output quantum, so slack in an earlier nonfinal copy
     * cannot reject a later prefix which the byte limits otherwise admit. */
    if (capacity > len && (reserve > SIZE_MAX - capacity || !budget_room_locked(capacity + reserve)))
        capacity = len;
    (void)input_recycle_trim();
    hp_chunk_t **slot = &input_recycled;
    while (*slot && (*slot)->capacity != capacity) slot = &(*slot)->next;
    hp_chunk_t *p = *slot;
    if (p) {
        *slot = p->next;
        --input_recycled_count;
        input_recycled_bytes -= capacity;
        buffered += capacity; /* Transfer the whole allocation, not just len. */
        pthread_mutex_unlock(&budget_mutex);
        return p;
    }
    /* Rounding must not reject input which fits the configured byte budget. */
    if (!budget_room_locked(capacity)) capacity = len;
    if (!budget_room_locked(capacity)) { pthread_mutex_unlock(&budget_mutex); return NULL; }
    buffered += capacity;
    pthread_mutex_unlock(&budget_mutex);
    p = malloc(sizeof(*p) + capacity);
    if (!p) { budget_release(capacity); return NULL; }
    p->capacity = capacity;
    return p;
}
static void input_chunk_free(hp_chunk_t *p) {
    size_t capacity = p->capacity;
    pthread_mutex_lock(&budget_mutex);
    if (buffered < capacity) abort();
    buffered -= capacity;
    size_t limit = input_recycle_trim();
    bool reusable = (capacity > 0 && capacity <= INPUT_RECYCLE_MIN) ||
                    capacity == 2 * INPUT_RECYCLE_MIN || capacity == INPUT_RECYCLE_MAX;
    /* Preserve the old list-length bound for exact sub-minimum copies. */
    if (!input_recycle_stopped && reusable && input_recycled_count < limit / INPUT_RECYCLE_MIN &&
        capacity <= limit && input_recycled_bytes <= limit - capacity) {
        p->next = input_recycled;
        input_recycled = p;
        ++input_recycled_count;
        input_recycled_bytes += capacity;
    } else {
        free(p);
    }
    pthread_cond_broadcast(&budget_cond);
    pthread_mutex_unlock(&budget_mutex);
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
/* Two transport chunks share one contiguous owned copy. Keep the entire
 * allocation charged until its last slice is released, including when a send
 * batch ends between slices. Neither PHP buffers nor released credit is reused. */
struct hp_chunk_allocation { atomic_uint refs; size_t bytes; void *owner; void (*release)(void *, size_t); };
static size_t chunk_destroy(hp_chunk_t *p) {
    hp_chunk_allocation_t *allocation = p->allocation;
    if (!allocation) { input_chunk_free(p); return 0; }
    if (atomic_fetch_sub_explicit(&allocation->refs, 1, memory_order_acq_rel) != 1) return 0;
    size_t bytes = allocation->bytes;
    if (allocation->release) {
        /* The consumer only publishes the return. Keep its full capacity
         * charged until the origin loop has actually disposed the storage. */
        allocation->release(allocation->owner, bytes);
        bytes = 0;
    }
    free(allocation);
    return bytes;
}
void hp_chunk_free(hp_chunk_t *p) { if (p) { size_t bytes = chunk_destroy(p); if (bytes) budget_release(bytes); } }
void hp_chunks_free(hp_chunk_t *p) {
    size_t bytes = 0;
    while (p) { hp_chunk_t *next = p->next; bytes += chunk_destroy(p); p = next; }
    if (bytes) budget_release(bytes);
}
void hp_response_release_chunks(hp_request_t *r, hp_chunk_t *p) {
    size_t bytes = 0;
    while (p) { hp_chunk_t *next = p->next; bytes += chunk_destroy(p); p = next; }
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
    if (r->response_lease || r->response_lease_charge || r->response_lease_borrowed ||
        r->response_lease_call_active || r->response_lease_fallback_active) abort();
    if (r->document_uri_owned) free(r->document_uri);
    free(r->method); free(r->uri); free(r->query); free(r->protocol); free(r->script); free(r->script_name);
    free(r->path_info); free(r->document_root); free(r->remote_addr); free(r->remote_port);
    free(r->server_name); free(r->server_port); free(r->content_type); free(r->cookie);
    hp_headers_free(r->headers); hp_headers_free(r->env); hp_headers_free(r->response_headers);
    hp_headers_free(r->sapi_response_headers);
    while (r->body_head) { hp_chunk_t *p = r->body_head; r->body_head = p->next; hp_chunk_free(p); }
    while (r->output_head) { hp_chunk_t *p = r->output_head; r->output_head = p->next; hp_chunk_free(p); }
    while (r->info_head) { hp_info_t *p = r->info_head; r->info_head = p->next; hp_headers_free(p->headers); free(p); }
    pthread_cond_destroy(&r->body_cond); pthread_cond_destroy(&r->output_cond); pthread_mutex_destroy(&r->mutex); free(r);
}
void hp_request_notify(hp_request_t *r) { if (r->notify) r->notify(r); }
/* Request mutex held. Only native-owned data is touched; no PHP callbacks. */
static bool sapi_response_publish_locked(hp_request_t *r) {
    if (!r->sapi_headers_accepted || r->sapi_headers_published ||
        r->cancelled || r->response_finished || r->headers_taken) return false;
    hp_headers_free(r->response_headers);
    r->response_headers = r->sapi_response_headers;
    r->sapi_response_headers = NULL;
    r->status = r->sapi_response_status;
    r->headers_ready = true;
    r->sapi_headers_published = true;
    return true;
}
static void sapi_response_discard_locked(hp_request_t *r) {
    hp_headers_free(r->sapi_response_headers);
    r->sapi_response_headers = NULL;
}
static void response_lease_stop_locked(hp_request_t *r, bool revoke) {
    r->response_lease_return = true;
    r->response_lease_requested = false;
    if (revoke && r->response_lease) r->response_lease_ops->revoke(r->response_lease);
    pthread_cond_broadcast(&r->output_cond);
}
void hp_request_cancel(hp_request_t *r) {
    pthread_mutex_lock(&r->mutex);
    r->cancelled = true;
    sapi_response_discard_locked(r);
    hp_php_metrics_request_cancel_locked(&r->php_metrics, hp_now_ns());
    if (r->state != HP_COMPLETED && r->state != HP_TIMED_OUT) r->state = HP_CANCELLED;
    stop_budget_locked(r);
    response_lease_stop_locked(r, true);
    pthread_cond_broadcast(&r->body_cond); pthread_cond_broadcast(&r->output_cond);
    pthread_mutex_unlock(&r->mutex);
}
static void timeout_locked(hp_request_t *r) {
    r->cancelled = true; r->state = HP_TIMED_OUT;
    sapi_response_discard_locked(r);
    hp_php_metrics_request_cancel_locked(&r->php_metrics, hp_now_ns());
    stop_budget_locked(r);
    response_lease_stop_locked(r, true);
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
        size_t reserve = eof ? 0 : r->body_limit - r->body_bytes - len;
        size_t quantum = r->output_limit < 16384 ? r->output_limit : 16384;
        reserve = reserve > SIZE_MAX - quantum ? SIZE_MAX : reserve + quantum;
        hp_chunk_t *p = input_chunk_new(len, reserve);
        if (!p) { pthread_mutex_unlock(&r->mutex); return -1; }
        p->next = NULL; p->len = len; p->offset = 0;
        p->allocation = NULL; p->data = (char *)(p + 1); memcpy(p->data, data, len);
        if (r->body_tail) r->body_tail->next = p; else r->body_head = p;
        r->body_tail = p; r->body_bytes += len;
    }
    r->body_eof = eof; pthread_cond_broadcast(&r->body_cond); pthread_mutex_unlock(&r->mutex); return 0;
}
int hp_request_body_push_owned(hp_request_t *r, void *owner, char *data, size_t len,
                               size_t capacity, void (*release)(void *, size_t), bool eof) {
    pthread_mutex_lock(&r->mutex);
    if (r->cancelled || !len || !owner || !release || capacity < len) {
        pthread_mutex_unlock(&r->mutex); return -1;
    }
    if (r->response_finished) {
        pthread_mutex_unlock(&r->mutex); release(owner, 0); return 1;
    }
    if (r->body_eof || len > r->body_limit || r->body_bytes > r->body_limit - len) {
        pthread_mutex_unlock(&r->mutex); return -1;
    }
    hp_chunk_allocation_t *allocation = malloc(sizeof(*allocation) + sizeof(hp_chunk_t));
    if (!allocation) { pthread_mutex_unlock(&r->mutex); return 0; }
    pthread_mutex_lock(&budget_mutex);
    /* A detached H2O allocation can be larger than the copied payload. Do not
     * spend the last output quantum on that excess capacity: an application
     * may write its response before reading or finishing this input. */
    size_t quantum = r->output_limit < 16384 ? r->output_limit : 16384;
    bool room = capacity <= SIZE_MAX - quantum && budget_room_locked(capacity + quantum);
    if (room) buffered += capacity;
    pthread_mutex_unlock(&budget_mutex);
    if (!room) { free(allocation); pthread_mutex_unlock(&r->mutex); return 0; }
    atomic_init(&allocation->refs, 1);
    allocation->bytes = capacity; allocation->owner = owner; allocation->release = release;
    hp_chunk_t *p = (hp_chunk_t *)(allocation + 1);
    *p = (hp_chunk_t){ .len = len, .capacity = capacity, .data = data, .allocation = allocation };
    if (r->body_tail) r->body_tail->next = p; else r->body_head = p;
    r->body_tail = p; r->body_bytes += len; r->body_eof = eof;
    pthread_cond_broadcast(&r->body_cond);
    pthread_mutex_unlock(&r->mutex); return 1;
}
void hp_request_body_release_owned_capacity(size_t capacity) {
    if (capacity) budget_release(capacity);
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
        if (p->offset == p->len) {
            r->body_head = p->next;
            if (!r->body_head) r->body_tail = NULL;
            if (p == r->body_read_pin) r->body_read_pin_detached = true;
            else hp_chunk_free(p);
        }
    }
    bool resume = r->body_producer_blocked && r->body_bytes <= r->body_limit / 2;
    if (resume) r->body_producer_blocked = false;
    pthread_mutex_unlock(&r->mutex);
    if (resume) hp_request_notify(r);
    return (ssize_t)n;
}
bool hp_sapi_request_body_borrow(hp_request_t *r, size_t max, hp_sapi_body_view_t *view) {
    if (!max) return false;
    pthread_mutex_lock(&r->mutex);
    hp_chunk_t *p = r->body_head;
    if (r->cancelled || r->response_finished || r->body_read_pin || !p) {
        pthread_mutex_unlock(&r->mutex); return false;
    }
    size_t len = p->len - p->offset;
    /* Never accumulate fragments while pinning their credit: a body window
     * smaller than the SAPI block must still make progress through read_post. */
    if (!len || (len < max && (!r->body_eof || p->next))) {
        pthread_mutex_unlock(&r->mutex); return false;
    }
    if (len > max) len = max;
    r->body_read_pin = p;
    *view = (hp_sapi_body_view_t){ .request = r, .chunk = p, .data = p->data + p->offset, .len = len };
    p->offset += len;
    r->body_bytes -= len;
    r->body_read_pin_detached = p->offset == p->len;
    if (r->body_read_pin_detached) {
        r->body_head = p->next;
        if (!r->body_head) r->body_tail = NULL;
    }
    bool resume = r->body_producer_blocked && r->body_bytes <= r->body_limit / 2;
    if (resume) r->body_producer_blocked = false;
    hp_request_retain(r);
    pthread_mutex_unlock(&r->mutex);
    if (resume) hp_request_notify(r);
    return true;
}
void hp_sapi_request_body_release(hp_sapi_body_view_t *view) {
    hp_request_t *r = view->request;
    if (!r) return;
    pthread_mutex_lock(&r->mutex);
    hp_chunk_t *p = r->body_read_pin;
    if (!p || p != view->chunk) abort();
    bool detached = r->body_read_pin_detached;
    r->body_read_pin = NULL;
    r->body_read_pin_detached = false;
    if (detached) hp_chunk_free(p);
    *view = (hp_sapi_body_view_t){0};
    pthread_mutex_unlock(&r->mutex);
    hp_request_release(r);
}
bool hp_response_lease_requested(hp_request_t *r) {
    pthread_mutex_lock(&r->mutex);
    bool requested = r->response_lease_requested && !r->response_lease_return &&
        !r->cancelled && !r->response_finished;
    pthread_mutex_unlock(&r->mutex);
    return requested;
}
void hp_response_lease_acquire(hp_request_t *r, void *(*acquire)(void *), void *data) {
    pthread_mutex_lock(&r->mutex);
    /* A timeout or another producer permanently withdrew this generation.
     * The callback must never allocate a token after that withdrawal. */
    if (r->response_lease_requested && !r->response_lease_return &&
        !r->cancelled && !r->response_finished) {
        r->response_lease = acquire ? acquire(data) : NULL;
        r->response_lease_requested = false;
        if (!r->response_lease) r->response_lease_return = true;
        pthread_cond_broadcast(&r->output_cond);
    }
    pthread_mutex_unlock(&r->mutex);
}
void *hp_response_lease_get(hp_request_t *r, bool *return_requested, bool *call_active) {
    pthread_mutex_lock(&r->mutex);
    void *token = r->response_lease;
    *return_requested = r->response_lease_return || r->cancelled || r->response_finished;
    *call_active = r->response_lease_call_active;
    pthread_mutex_unlock(&r->mutex);
    return token;
}
void hp_response_lease_yield(hp_request_t *r) {
    pthread_mutex_lock(&r->mutex);
    bool notify = r->response_lease || r->response_lease_requested;
    response_lease_stop_locked(r, false);
    pthread_cond_broadcast(&r->output_cond);
    pthread_mutex_unlock(&r->mutex);
    if (notify) hp_request_notify(r);
}
void hp_response_lease_returned(hp_request_t *r, void *token) {
    if (!token) return;
    pthread_mutex_lock(&r->mutex);
    if (r->response_lease != token) { pthread_mutex_unlock(&r->mutex); return; }
    response_lease_stop_locked(r, false);
    /* H2O can detach an idle token after the worker publishes its borrowed
     * CALL but just before it enters the API. Keep the caller reference until
     * that entire bridge interval has ended; a detached API safely refuses
     * before I/O. The worker's exit wake schedules a second, idempotent return. */
    if (r->response_lease_call_active) { pthread_mutex_unlock(&r->mutex); return; }
    const hp_response_lease_ops_t *ops = r->response_lease_ops;
    size_t charge = r->response_lease_charge;
    r->response_lease = NULL;
    r->response_lease_charge = 0;
    pthread_mutex_unlock(&r->mutex);
    /* The allocation and cancel descriptor remain charged through actual
     * release. Never hold a request lock while entering the FD fork gate. */
    ops->release(token);
    pthread_mutex_lock(&r->mutex);
    if (r->output_inflight < charge) abort();
    r->output_inflight -= charge;
    budget_release(charge);
    pthread_cond_broadcast(&r->output_cond);
    pthread_mutex_unlock(&r->mutex);
}
static void response_lease_unborrow_locked(hp_request_t *r) {
    size_t charge = r->response_lease_borrowed;
    r->response_lease_borrowed = 0;
    /* An unsuccessful initial handshake has no token to return. */
    if (!r->response_lease) {
        charge += r->response_lease_charge;
        r->response_lease_charge = 0;
    }
    if (r->output_inflight < charge) abort();
    r->output_inflight -= charge;
    budget_release(charge);
    r->response_lease_call_active = false;
    pthread_cond_broadcast(&r->output_cond);
}
/* Only the initial large write waits, for at most one millisecond and never
 * beyond the request deadline. All capacity admission and later eligibility
 * are try-only. Owned copies retain their original 16KiB progress quantum. */
static bool response_lease_write(hp_request_t *r, const void *data, size_t len, ssize_t *result, bool *fallback_owner) {
    pthread_mutex_lock(&r->mutex);
    const hp_response_lease_ops_t *ops = r->response_lease_ops;
    if (!ops || !len || r->cancelled || r->response_finished) {
        pthread_mutex_unlock(&r->mutex); return false;
    }
    if (hp_now_ns() >= r->deadline_ns) {
        timeout_locked(r);
        pthread_mutex_unlock(&r->mutex);
        hp_request_notify(r);
        return false;
    }
    bool other_owner = r->response_lease_owner_set &&
        !pthread_equal(r->response_lease_owner, pthread_self());
    if (other_owner) {
        /* Auxiliary output follows the complete currently borrowed CALL.
         * Graceful return permits its frame to finish; revoke would corrupt
         * an interrupted frame and is reserved for terminal cancellation. */
        bool notify = r->response_lease || r->response_lease_requested;
        response_lease_stop_locked(r, false);
        pthread_mutex_unlock(&r->mutex);
        if (notify) hp_request_notify(r);
        pthread_mutex_lock(&r->mutex);
        while ((r->response_lease_call_active || r->response_lease_fallback_active) &&
               !r->cancelled && !r->response_finished) {
            struct timespec ts = deadline(r);
            if (pthread_cond_timedwait(&r->output_cond, &r->mutex, &ts) == ETIMEDOUT) timeout_locked(r);
        }
        pthread_mutex_unlock(&r->mutex);
        return false;
    }
    bool initial = !r->response_lease_attempted;
    if (initial) r->response_lease_attempted = true;
    bool eligible = len >= 32768 && r->output_limit > 24576 && r->headers_ready &&
        !r->info_head && !r->output_bytes && !r->output_head &&
        !r->response_lease_call_active && !r->response_lease_return &&
        (initial || r->response_lease);
    size_t metadata = initial ? ops->charge : 0;
    bool room = eligible && metadata <= SIZE_MAX - len &&
        r->output_inflight <= r->output_limit &&
        len + metadata <= r->output_limit - r->output_inflight;
    if (room) {
        pthread_mutex_lock(&budget_mutex);
        room = !atomic_load_explicit(&r->budget_stopped, memory_order_seq_cst) && budget_room_locked(len + metadata);
        if (room) buffered += len + metadata;
        pthread_mutex_unlock(&budget_mutex);
    }
    if (!room) {
        bool notify = r->response_lease != NULL;
        if (notify) response_lease_stop_locked(r, false);
        pthread_mutex_unlock(&r->mutex);
        if (notify) hp_request_notify(r);
        return false;
    }
    r->output_inflight += len + metadata;
    r->response_lease_charge += metadata;
    r->response_lease_borrowed = len;
    r->response_lease_call_active = true;
    r->response_lease_owner_set = true;
    r->response_lease_owner = pthread_self();
    if (initial) {
        r->response_lease_requested = true;
        uint64_t now = hp_now_ns(), until = now > UINT64_MAX - 1000000 ? UINT64_MAX : now + 1000000;
        if (until > r->deadline_ns) until = r->deadline_ns;
        pthread_mutex_unlock(&r->mutex);
        hp_request_notify(r);
        pthread_mutex_lock(&r->mutex);
        while (r->response_lease_requested && !r->response_lease_return &&
               !r->cancelled && !r->response_finished) {
            struct timespec ts = { .tv_sec = until / 1000000000ULL, .tv_nsec = until % 1000000000ULL };
            if (pthread_cond_timedwait(&r->output_cond, &r->mutex, &ts) == ETIMEDOUT) {
                response_lease_stop_locked(r, false);
                break;
            }
        }
    }
    if (hp_now_ns() >= r->deadline_ns && !r->cancelled && !r->response_finished) timeout_locked(r);
    void *token = r->response_lease;
    if (!token || r->response_lease_return || r->cancelled || r->response_finished) {
        bool notify = token != NULL || r->cancelled || r->response_finished;
        response_lease_stop_locked(r, false);
        /* An auxiliary producer must not overtake this original CALL when
         * its pending lease falls back. Retire borrowed credit now so owned
         * chunks can make progress, but keep producer order until they have
         * all entered the normal queue. Network draining remains enabled. */
        if (!r->cancelled && !r->response_finished) {
            r->response_lease_fallback_active = true;
            *fallback_owner = true;
        }
        response_lease_unborrow_locked(r);
        pthread_mutex_unlock(&r->mutex);
        if (notify) hp_request_notify(r);
        return false;
    }
    uint64_t until = r->deadline_ns;
    if (r->response_lease_timeout_ns) {
        uint64_t now = hp_now_ns();
        uint64_t idle_until = r->response_lease_timeout_ns > UINT64_MAX - now
            ? UINT64_MAX : now + r->response_lease_timeout_ns;
        if (idle_until < until) until = idle_until;
    }
    pthread_mutex_unlock(&r->mutex);
    size_t written = 0;
    int error = 0;
    int status = ops->write(token, data, len, until, &written, &error);
    /* The token can be detached after write returns and before this mutex is
     * reacquired. No worker dereferences it after the synchronous API call. */
    pthread_mutex_lock(&r->mutex);
    if (written && !r->cancelled && !r->response_finished) r->state = HP_RESPONDING;
    if (status != 0 || written != len) response_lease_stop_locked(r, false);
    if (status == 1 && written == 0 && !r->cancelled && !r->response_finished) {
        r->response_lease_fallback_active = true;
        *fallback_owner = true;
    }
    response_lease_unborrow_locked(r);
    bool notify = r->response_lease_return || r->cancelled || r->response_finished;
    pthread_mutex_unlock(&r->mutex);
    if (status < 0 || (status == 0 && written != len) || (status == 1 && written != 0)) {
        pthread_mutex_lock(&r->mutex);
        if (error == ETIMEDOUT || hp_now_ns() >= r->deadline_ns) timeout_locked(r);
        pthread_mutex_unlock(&r->mutex);
        hp_request_cancel(r);
        hp_request_notify(r);
        *result = -1; return true; /* Never duplicate a partially framed CALL. */
    }
    if (notify) hp_request_notify(r);
    if (status == 1) return false; /* Guaranteed zero I/O by transport contract. */
    *result = (ssize_t)len;
    return true;
}
static ssize_t response_write_owned(hp_request_t *r, const void *data, size_t len) {
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
        /* Pair only credit already available; never wait for a larger slab.
         * Transport chunks remain 16KiB and a short write is copied at once. */
        size_t room = r->output_limit - r->output_inflight - r->output_bytes;
        size_t pair = len - sent;
        if (pair > 32768) pair = 32768;
        if (pair <= room) n = pair;
        /* Reserve before dropping the lock for global-budget/allocation work.
         * Other producers and detached sends cannot consume the same credit. */
        r->output_bytes += n;
        pthread_mutex_unlock(&r->mutex);
        /* Global budget waits use a condition, without holding request locks. */
        pthread_mutex_lock(&budget_mutex);
        bool registered = false;
        while (!atomic_load_explicit(&r->budget_stopped, memory_order_seq_cst) &&
               !budget_room_locked(n)) {
            size_t available = buffered < hp_config.max_buffered_global
                ? hp_config.max_buffered_global - buffered : 0;
            size_t quantum = available && available < 16384 ? available : 16384;
            if (n > quantum) {
                /* Unread input can leave less than one transport chunk free.
                 * Admit the real available credit before waiting; the smaller
                 * allocation remains fully charged through send completion. */
                size_t excess = n - quantum;
                pthread_mutex_unlock(&budget_mutex);
                (void)output_unreserve(r, excess);
                n = quantum;
                pthread_mutex_lock(&budget_mutex);
                continue;
            }
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
        size_t count = (n + 16383) / 16384;
        hp_chunk_allocation_t *allocation = malloc(sizeof(*allocation) + count * sizeof(hp_chunk_t) + n);
        if (!allocation) {
            budget_release(n);
            return output_unreserve(r, n) ? (ssize_t)len : -1;
        }
        atomic_init(&allocation->refs, count); allocation->bytes = n;
        allocation->owner = NULL; allocation->release = NULL;
        hp_chunk_t *p = (hp_chunk_t *)(allocation + 1), *tail = p + count - 1;
        char *payload = (char *)(p + count);
        memcpy(payload, (const char *)data + sent, n);
        for (size_t i = 0, offset = 0; i < count; ++i) {
            size_t take = n - offset; if (take > 16384) take = 16384;
            p[i] = (hp_chunk_t){ .next = i + 1 < count ? p + i + 1 : NULL,
                .len = take, .data = payload + offset, .allocation = allocation };
            offset += take;
        }
        pthread_mutex_lock(&r->mutex);
        if (r->cancelled || r->response_finished) {
            pthread_mutex_unlock(&r->mutex); hp_chunks_free(p);
            return output_unreserve(r, n) ? (ssize_t)len : -1;
        }
        if (r->output_tail) r->output_tail->next = p; else r->output_head = p;
        r->output_tail = tail; r->state = HP_RESPONDING; /* Credit was reserved above. */
        /* Enqueue and the consumer's empty-to-idle transition share this lock.
         * An active send/proceed path will drain newly queued output itself. */
        bool notify = !r->output_consumer_active;
        pthread_mutex_unlock(&r->mutex); sent += n;
        if (notify) hp_request_notify(r);
    }
    return (ssize_t)sent;
}
ssize_t hp_response_write(hp_request_t *r, const void *data, size_t len) {
    /* Direct bridge producers must see the same logically accepted SAPI
     * final state before any capacity or lease-owner wait can begin. */
    (void)hp_sapi_response_publish(r);
    ssize_t leased;
    bool fallback_owner = false;
    if (response_lease_write(r, data, len, &leased, &fallback_owner)) return leased;
    ssize_t result = response_write_owned(r, data, len);
    if (fallback_owner) {
        pthread_mutex_lock(&r->mutex);
        r->response_lease_fallback_active = false;
        pthread_cond_broadcast(&r->output_cond);
        pthread_mutex_unlock(&r->mutex);
    }
    return result;
}
int hp_sapi_response_headers(hp_request_t *r, int status, hp_header_t *headers, bool defer) {
    if ((status >= 100 && status < 200 && status != 101) ||
        (status == 101 && (!r->protocol || strcmp(r->protocol, "HTTP/1.1")))) {
        hp_headers_free(headers); return -1;
    }
    pthread_mutex_lock(&r->mutex);
    if (r->sapi_headers_accepted) {
        pthread_mutex_unlock(&r->mutex); hp_headers_free(headers); return 0;
    }
    if (r->cancelled || r->response_finished) {
        pthread_mutex_unlock(&r->mutex); hp_headers_free(headers); return -1;
    }
    /* A final 101 already committed by the immediate public bridge is terminal. */
    if (r->headers_ready && r->status == 101) {
        r->sapi_headers_accepted = r->sapi_headers_published = true;
        pthread_mutex_unlock(&r->mutex); hp_headers_free(headers); return 0;
    }
    if (r->headers_taken) {
        pthread_mutex_unlock(&r->mutex); hp_headers_free(headers); return -1;
    }
    r->sapi_response_headers = headers;
    r->sapi_response_status = status;
    r->sapi_headers_accepted = true;
    /* Do not defer replacement of an already visible public header set. */
    bool notify = (!defer || status == 101 || r->headers_ready) && sapi_response_publish_locked(r);
    pthread_mutex_unlock(&r->mutex);
    if (notify) hp_request_notify(r);
    return 0;
}
int hp_sapi_response_publish(hp_request_t *r) {
    pthread_mutex_lock(&r->mutex);
    bool notify = sapi_response_publish_locked(r);
    bool failed = r->sapi_headers_accepted && !r->sapi_headers_published;
    pthread_mutex_unlock(&r->mutex);
    if (notify) hp_request_notify(r);
    return failed ? -1 : 0;
}
ssize_t hp_sapi_response_write(hp_request_t *r, const void *data, size_t len) {
    if (!len) return hp_response_write(r, data, len);
    pthread_mutex_lock(&r->mutex);
    bool pending = r->sapi_headers_accepted && !r->sapi_headers_published;
    /* One ordinary transport quantum only. Never reserve or wait in order to
     * combine headers, and never interfere with the initial large-write lease. */
    bool eligible = pending && len <= 16384 && len <= r->output_limit &&
        !r->cancelled && !r->response_finished && !r->headers_taken &&
        !r->output_bytes && !r->output_inflight && !r->output_head &&
        !r->output_consumer_active && !r->info_head &&
        !r->response_lease_owner_set && !r->response_lease_call_active &&
        !r->response_lease_fallback_active;
    hp_chunk_allocation_t *allocation = NULL;
    if (eligible && hp_now_ns() >= r->deadline_ns) {
        timeout_locked(r);
        eligible = false;
    }
    if (eligible) {
        pthread_mutex_lock(&budget_mutex);
        bool room = !atomic_load_explicit(&r->budget_stopped, memory_order_seq_cst) && budget_room_locked(len);
        if (room) buffered += len;
        pthread_mutex_unlock(&budget_mutex);
        if (room) {
            allocation = malloc(sizeof(*allocation) + sizeof(hp_chunk_t) + len);
            if (!allocation) budget_release(len);
        }
    }
    if (allocation) {
        atomic_init(&allocation->refs, 1); allocation->bytes = len;
        allocation->owner = NULL; allocation->release = NULL;
        hp_chunk_t *p = (hp_chunk_t *)(allocation + 1);
        char *payload = (char *)(p + 1);
        memcpy(payload, data, len);
        *p = (hp_chunk_t){ .len = len, .data = payload, .allocation = allocation };
        r->output_head = r->output_tail = p;
        r->output_bytes += len;
        r->state = HP_RESPONDING;
        /* The ordinary small-write path spends this generation's initial
         * lease attempt too; do not reopen it after this chunk is drained. */
        if (r->response_lease_ops) r->response_lease_attempted = true;
        (void)sapi_response_publish_locked(r);
        pthread_mutex_unlock(&r->mutex);
        /* The first notification observes both the immutable final headers
         * and the already-owned body; H2O still owns filters and all framing. */
        hp_request_notify(r);
        return (ssize_t)len;
    }
    bool notify = sapi_response_publish_locked(r);
    bool cancelled = r->cancelled;
    pthread_mutex_unlock(&r->mutex);
    /* Progress comes before any request/global capacity wait, allocation
     * fallback, other-producer wait or large lease handshake in the bridge. */
    if (notify || cancelled) hp_request_notify(r);
    return hp_response_write(r, data, len);
}
void hp_response_flush_notify(hp_request_t *r) {
    pthread_mutex_lock(&r->mutex);
    bool published = sapi_response_publish_locked(r);
    r->output_flush_requested = true;
    /* The lease already flushed headers, and every direct CALL completed its
     * own synchronous write. An additional owning-loop wake has no work. */
    bool leased = r->response_lease && !r->response_lease_return;
    bool notify = published || (!r->output_consumer_active && !leased);
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
    /* SAPI success already froze this request's final set. A public final
     * must not replace it while its wire publication is still pending. */
    if ((!info && r->sapi_headers_accepted) || r->headers_taken) {
        pthread_mutex_unlock(&r->mutex); hp_headers_free(copy); return -1;
    }
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
    /* Cancellation/queue rejection can pre-set response_finished before
     * this first publication; track this operation separately. A repeated
     * runtime/SAPI finish has no new state for the loop to consume. */
    if (r->response_finish_published) { pthread_mutex_unlock(&r->mutex); return; }
    (void)sapi_response_publish_locked(r);
    if (!r->sapi_headers_published) sapi_response_discard_locked(r);
    r->response_finish_published = true;
    r->response_finished = true; r->headers_ready = true;
    response_lease_stop_locked(r, false);
    hp_php_metrics_request_end_locked(&r->php_metrics, hp_now_ns());
    stop_budget_locked(r);
    /* This is application input EOF, distinct from the owning generator's
     * actual transport EOF. Cached PHP stream bytes remain PHP-owned. */
    hp_chunk_t *body = r->body_head;
    if (r->body_read_pin && !r->body_read_pin_detached) {
        if (body != r->body_read_pin) abort();
        body = body->next;
        r->body_read_pin->next = NULL;
        r->body_read_pin_detached = true;
    }
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
