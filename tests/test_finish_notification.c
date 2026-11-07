/* Terminal publication must remain useful once and idempotent after delivery. */
#include "h2o_php.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

hp_runtime_config_t hp_config = { .max_buffered_body = 4096, .max_buffered_response = 32768,
    .max_buffered_global = 65536, .request_timeout_ms = 2000 };
static atomic_uint notifications, metric_ends;
void hp_php_metrics_request_cancel_locked(hp_php_metric_request_t *r, uint64_t now) { (void)r; (void)now; }
void hp_php_metrics_request_end_locked(hp_php_metric_request_t *r, uint64_t now) {
    (void)r; (void)now; atomic_fetch_add(&metric_ends, 1);
}
static void notify(hp_request_t *r) {
    atomic_fetch_add(&notifications, 1);
    /* Simulate the loop consuming each message before the next finish call. */
    atomic_store(&r->notification_pending, false);
}
static hp_request_t *request(void) {
    atomic_store(&notifications, 0); atomic_store(&metric_ends, 0);
    hp_request_t *r = hp_request_new(); assert(r); r->notify = notify;
    return r;
}
static void *finish_thread(void *arg) { hp_response_finish(arg); return NULL; }

static void ordinary_response(void) {
    hp_request_t *r = request();
    assert(hp_request_body_push(r, "unread", 6, false) == 0);
    assert(hp_response_headers(r, 200, NULL, false) == 0);
    assert(atomic_load(&notifications) == 1); /* The header wake is preserved. */
    assert(hp_response_write(r, "body", 4) == 4);
    assert(atomic_load(&notifications) == 2); /* First-byte delivery is preserved. */
    hp_response_finish(r);
    assert(atomic_load(&notifications) == 3 && atomic_load(&metric_ends) == 1);
    assert(r->body_eof && r->response_finished && !r->body_head && r->body_bytes == 0);
    assert(hp_bridge_buffered_bytes() == 4);
    bool finished;
    hp_chunk_t *chunks = hp_response_pop_batch(r, 16, 262144, &finished);
    assert(chunks && chunks->len == 4 && !memcmp(chunks->data, "body", 4) && finished);
    hp_response_release_chunks(r, chunks);
    hp_response_finish(r); hp_response_finish(r);
    assert(atomic_load(&notifications) == 3 && atomic_load(&metric_ends) == 1);
    assert(hp_bridge_buffered_bytes() == 0);
    hp_request_release(r);
}
static void prepublished_terminal_state(void) {
    hp_request_t *r = request();
    pthread_mutex_lock(&r->mutex);
    /* take()/reject_queued() pre-set these before their first finish call. */
    r->response_finished = r->headers_ready = r->cancelled = true;
    pthread_mutex_unlock(&r->mutex);
    hp_response_finish(r);
    assert(atomic_load(&notifications) == 1 && atomic_load(&metric_ends) == 1 && r->body_eof);
    hp_response_finish(r);
    assert(atomic_load(&notifications) == 1 && atomic_load(&metric_ends) == 1);
    hp_request_release(r);
}
static void informational_and_flush(void) {
    hp_request_t *r = request();
    assert(hp_response_headers(r, 103, NULL, true) == 0);
    hp_response_flush_notify(r);
    assert(atomic_load(&notifications) == 2 && r->output_flush_requested);
    assert(hp_response_headers(r, 200, NULL, false) == 0);
    assert(hp_response_write(r, "stream", 6) == 6);
    bool finished;
    hp_chunk_t *chunks = hp_response_pop_batch(r, 16, 262144, &finished);
    assert(chunks && !finished); hp_response_release_chunks(r, chunks);
    (void)hp_response_pop_batch(r, 16, 262144, &finished); /* Re-arm an idle consumer. */
    hp_response_flush_notify(r);
    assert(atomic_load(&notifications) == 5);
    hp_response_finish(r); hp_response_finish(r);
    assert(atomic_load(&notifications) == 6 && atomic_load(&metric_ends) == 1);
    hp_request_release(r);
}
static void concurrent_finish(void) {
    hp_request_t *r = request();
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; ++i) assert(pthread_create(&threads[i], NULL, finish_thread, r) == 0);
    for (unsigned i = 0; i < 8; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert(atomic_load(&notifications) == 1 && atomic_load(&metric_ends) == 1);
    hp_request_release(r);
}
typedef struct { hp_request_t *request; pthread_barrier_t *start; ssize_t result; } blocked_t;
static void *blocked_read(void *arg) {
    blocked_t *wait = arg; char byte;
    pthread_barrier_wait(wait->start);
    wait->result = hp_request_body_read(wait->request, &byte, 1);
    return NULL;
}
static void *blocked_write(void *arg) {
    blocked_t *wait = arg;
    pthread_barrier_wait(wait->start);
    wait->result = hp_response_write(wait->request, "x", 1);
    return NULL;
}
static void terminal_waiters(void) {
    hp_request_t *r = request();
    char full[32768]; memset(full, 'F', sizeof(full));
    assert(hp_response_write(r, full, sizeof(full)) == sizeof(full));
    pthread_barrier_t start; assert(pthread_barrier_init(&start, NULL, 3) == 0);
    blocked_t read = { r, &start, -2 }, write = { r, &start, -2 };
    pthread_t reader, writer;
    assert(pthread_create(&reader, NULL, blocked_read, &read) == 0);
    assert(pthread_create(&writer, NULL, blocked_write, &write) == 0);
    pthread_barrier_wait(&start);
    hp_response_finish(r); hp_response_finish(r);
    assert(pthread_join(reader, NULL) == 0 && pthread_join(writer, NULL) == 0);
    assert(read.result == 0 && write.result == 1);
    assert(atomic_load(&notifications) == 2 && atomic_load(&metric_ends) == 1);
    pthread_barrier_destroy(&start);
    hp_request_release(r); assert(hp_bridge_buffered_bytes() == 0);
}
int main(void) {
    ordinary_response(); prepublished_terminal_state(); informational_and_flush(); concurrent_finish(); terminal_waiters();
    puts("PASS: one terminal publication, first-byte/headers/flush/info, pre-set terminal state, concurrent finish, input/output waiters");
}
