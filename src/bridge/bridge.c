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
size_t hp_bridge_buffered_bytes(void) {
    pthread_mutex_lock(&budget_mutex); size_t n = buffered; pthread_mutex_unlock(&budget_mutex); return n;
}
hp_request_t *hp_request_new(void) {
    hp_request_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    atomic_init(&r->refs, 1); atomic_init(&r->notification_pending, false);
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
    if (r->state != HP_COMPLETED && r->state != HP_TIMED_OUT) r->state = HP_CANCELLED;
    pthread_cond_broadcast(&r->body_cond); pthread_cond_broadcast(&r->output_cond);
    pthread_mutex_unlock(&r->mutex);
    pthread_mutex_lock(&budget_mutex); pthread_cond_broadcast(&budget_cond); pthread_mutex_unlock(&budget_mutex);
}
static void timeout_locked(hp_request_t *r) {
    r->cancelled = true; r->state = HP_TIMED_OUT;
    pthread_cond_broadcast(&r->body_cond); pthread_cond_broadcast(&r->output_cond);
}
int hp_request_body_push(hp_request_t *r, const void *data, size_t len, bool eof) {
    pthread_mutex_lock(&r->mutex);
    if (r->cancelled || r->body_eof || len > r->body_limit || r->body_bytes > r->body_limit - len) {
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
ssize_t hp_request_body_read(hp_request_t *r, void *out, size_t len) {
    if (!len) return 0;
    pthread_mutex_lock(&r->mutex);
    while (!r->body_head && !r->body_eof && !r->cancelled) {
        struct timespec ts = deadline(r);
        if (pthread_cond_timedwait(&r->body_cond, &r->mutex, &ts) == ETIMEDOUT) timeout_locked(r);
    }
    if (r->cancelled) { pthread_mutex_unlock(&r->mutex); hp_request_notify(r); return -1; }
    size_t n = 0;
    while (len && r->body_head) {
        hp_chunk_t *p = r->body_head; size_t take = p->len - p->offset;
        if (take > len) take = len;
        memcpy((char *)out + n, p->data + p->offset, take); p->offset += take;
        n += take; len -= take; r->body_bytes -= take;
        if (p->offset == p->len) { r->body_head = p->next; if (!r->body_head) r->body_tail = NULL; hp_chunk_free(p); }
    }
    pthread_mutex_unlock(&r->mutex); hp_request_notify(r); return (ssize_t)n;
}
ssize_t hp_response_write(hp_request_t *r, const void *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t n = len - sent; if (n > 16384) n = 16384; if (n > r->output_limit) n = r->output_limit;
        if (!n) return -1;
        pthread_mutex_lock(&r->mutex);
        while (!r->cancelled && !r->response_finished && r->output_bytes > r->output_limit - n) {
            struct timespec ts = deadline(r);
            if (pthread_cond_timedwait(&r->output_cond, &r->mutex, &ts) == ETIMEDOUT) timeout_locked(r);
        }
        if (r->response_finished) { pthread_mutex_unlock(&r->mutex); return (ssize_t)len; }
        if (r->cancelled) { pthread_mutex_unlock(&r->mutex); hp_request_notify(r); return -1; }
        pthread_mutex_unlock(&r->mutex);
        /* Global budget waits use a condition, without holding request locks. */
        pthread_mutex_lock(&budget_mutex);
        while (buffered > hp_config.max_buffered_global - n) {
            struct timespec realtime; clock_gettime(CLOCK_REALTIME, &realtime); realtime.tv_sec++;
            pthread_cond_timedwait(&budget_cond, &budget_mutex, &realtime);
            pthread_mutex_unlock(&budget_mutex);
            pthread_mutex_lock(&r->mutex); bool stop = r->cancelled || r->response_finished;
            if (hp_now_ns() >= r->deadline_ns) { timeout_locked(r); stop = true; }
            pthread_mutex_unlock(&r->mutex);
            if (stop) { hp_request_notify(r); return -1; }
            pthread_mutex_lock(&budget_mutex);
        }
        buffered += n; pthread_mutex_unlock(&budget_mutex);
        hp_chunk_t *p = malloc(sizeof(*p) + n);
        if (!p) { budget_release(n); return -1; }
        p->next = NULL; p->len = n; p->offset = 0; memcpy(p->data, (const char *)data + sent, n);
        pthread_mutex_lock(&r->mutex);
        if (r->cancelled || r->response_finished) { pthread_mutex_unlock(&r->mutex); hp_chunk_free(p); return -1; }
        if (r->output_tail) r->output_tail->next = p; else r->output_head = p;
        r->output_tail = p; r->output_bytes += n; r->state = HP_RESPONDING;
        pthread_mutex_unlock(&r->mutex); sent += n; hp_request_notify(r);
    }
    return (ssize_t)sent;
}
int hp_response_headers(hp_request_t *r, int status, const hp_header_t *headers, bool info) {
    /* HTTP 101 requires a protocol upgrade, not an informational response. */
    if (info && (status < 100 || status >= 200 || status == 101)) return -1;
    hp_header_t *copy = hp_headers_clone(headers);
    if (headers && !copy) return -1;
    pthread_mutex_lock(&r->mutex);
    if (r->cancelled || r->response_finished || r->headers_taken) { pthread_mutex_unlock(&r->mutex); hp_headers_free(copy); return -1; }
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
    pthread_cond_broadcast(&r->output_cond); pthread_mutex_unlock(&r->mutex); hp_request_notify(r);
}
hp_chunk_t *hp_response_pop(hp_request_t *r) {
    pthread_mutex_lock(&r->mutex); hp_chunk_t *p = r->output_head;
    if (p) { r->output_head = p->next; if (!r->output_head) r->output_tail = NULL; p->next = NULL; r->output_bytes -= p->len; }
    pthread_cond_broadcast(&r->output_cond); pthread_mutex_unlock(&r->mutex); return p;
}
