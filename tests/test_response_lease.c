/* Standalone deterministic bridge contract regression; no H2O/PHP runtime.
 * Compile with src/bridge/bridge.c and -pthread. The transport stub accepts
 * caller memory only during write and models owner-loop grant/return. */
#include "h2o_php.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hp_runtime_config_t hp_config = { .max_buffered_body = 4096, .max_buffered_response = 262144,
    .max_buffered_global = 1048576, .request_timeout_ms = 2000 };
void hp_php_metrics_request_cancel_locked(hp_php_metric_request_t *r, uint64_t now) { (void)r; (void)now; }
void hp_php_metrics_request_end_locked(hp_php_metric_request_t *r, uint64_t now) { (void)r; (void)now; }

enum { META = 512, LARGE = 65536, CAPTURE = 262144 };
typedef struct fixture {
    hp_request_t *r;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool grant, block, entered, allow_exit, revoked;
    bool refuse, fail, timeout, finish_in_write, cancel_in_write, reject, detach_at_entry;
    bool block_notification, notification_entered, notification_exit;
    unsigned acquisitions, writes, releases, revokes, notifications;
    size_t peak_credit, captured;
    char bytes[CAPTURE];
} fixture_t;

static void capture(fixture_t *f, const void *data, size_t length) {
    pthread_mutex_lock(&f->mutex);
    assert(length <= CAPTURE - f->captured);
    memcpy(f->bytes + f->captured, data, length);
    f->captured += length;
    pthread_mutex_unlock(&f->mutex);
}
static void *acquire(void *data) {
    fixture_t *f = data;
    ++f->acquisitions;
    /* The whole caller span and metadata are charged BEFORE allocation. */
    assert(f->r->output_inflight == LARGE + META);
    assert(hp_bridge_buffered_bytes() >= LARGE + META);
    return f->grant && !f->reject ? f : NULL;
}
static int write_token(void *token, const void *data, size_t length, uint64_t deadline_ns,
                       size_t *written, int *error) {
    fixture_t *f = token;
    assert(deadline_ns <= f->r->deadline_ns);
    pthread_mutex_lock(&f->r->mutex);
    assert(f->r->response_lease_call_active && f->r->response_lease_borrowed == length);
    assert(f->r->output_inflight == length + META);
    pthread_mutex_unlock(&f->r->mutex);
    assert(hp_bridge_buffered_bytes() == length + META);
    if (f->detach_at_entry) {
        /* Owner return raced the gap after bridge unlock and before worker
         * API entry. H2O detached its reference but must not free ours yet. */
        hp_response_lease_returned(f->r, f);
        assert(!f->releases && hp_bridge_buffered_bytes() == length + META);
        *written = 0; *error = 0;
        return 1;
    }
    pthread_mutex_lock(&f->mutex);
    ++f->writes;
    f->peak_credit = hp_bridge_buffered_bytes();
    f->entered = true;
    pthread_cond_broadcast(&f->cond);
    while (f->block && !f->allow_exit && !f->revoked) pthread_cond_wait(&f->cond, &f->mutex);
    bool revoked = f->revoked;
    pthread_mutex_unlock(&f->mutex);
    *written = 0; *error = 0;
    if (f->refuse) return 1;
    if (f->fail || f->timeout || revoked) {
        /* Partial wire output is terminal and must not enter owned fallback. */
        capture(f, data, 37);
        *written = 37; *error = f->timeout ? ETIMEDOUT : ECANCELED;
        return -1;
    }
    capture(f, data, length);
    *written = length;
    if (f->finish_in_write) hp_response_finish(f->r);
    if (f->cancel_in_write) hp_request_cancel(f->r);
    return 0;
}
static void revoke_token(void *token) {
    fixture_t *f = token;
    pthread_mutex_lock(&f->mutex);
    ++f->revokes; f->revoked = true;
    pthread_cond_broadcast(&f->cond);
    pthread_mutex_unlock(&f->mutex);
}
static void release_token(void *token) {
    fixture_t *f = token;
    /* Metadata credit cannot disappear before the token/cancelFD is freed. */
    assert(hp_bridge_buffered_bytes() >= META);
    ++f->releases;
}
static const hp_response_lease_ops_t ops = {
    .charge = META, .write = write_token, .revoke = revoke_token, .release = release_token
};
static void drain(fixture_t *f) {
    bool finished;
    for (;;) {
        hp_chunk_t *chunks = hp_response_pop_batch(f->r, 16, 262144, &finished);
        if (!chunks) break;
        for (hp_chunk_t *p = chunks; p; p = p->next) {
            assert(p->len <= 16384);
            capture(f, p->data + p->offset, p->len - p->offset);
        }
        hp_response_release_chunks(f->r, chunks);
    }
}
/* The real receiver runs only on its owning loop. The stub serializes all
 * receiver work with this separate mutex while worker calls may overlap. */
static pthread_mutex_t receiver_mutex = PTHREAD_MUTEX_INITIALIZER;
static void receive(hp_request_t *r) {
    fixture_t *f = r->notify_data;
    pthread_mutex_lock(&receiver_mutex);
    ++f->notifications;
    pthread_mutex_lock(&f->mutex);
    if (f->block_notification) {
        f->notification_entered = true;
        pthread_cond_broadcast(&f->cond);
        while (!f->notification_exit) pthread_cond_wait(&f->cond, &f->mutex);
    }
    pthread_mutex_unlock(&f->mutex);
    if (f->grant && hp_response_lease_requested(r)) hp_response_lease_acquire(r, acquire, f);
    bool returning, active;
    void *token = hp_response_lease_get(r, &returning, &active);
    if (token && returning && !active) hp_response_lease_returned(r, token);
    token = hp_response_lease_get(r, &returning, &active);
    if (!token && !active) drain(f);
    pthread_mutex_unlock(&receiver_mutex);
}
static void init(fixture_t *f) {
    memset(f, 0, sizeof(*f));
    pthread_mutex_init(&f->mutex, NULL); pthread_cond_init(&f->cond, NULL);
    f->r = hp_request_new(); assert(f->r);
    f->r->notify = receive; f->r->notify_data = f;
    f->r->response_lease_ops = &ops;
    assert(hp_response_headers(f->r, 200, NULL, false) == 0);
}
static void dispose(fixture_t *f) {
    hp_response_finish(f->r); hp_response_finish(f->r);
    receive(f->r);
    assert(!f->r->response_lease && !f->r->response_lease_borrowed && !f->r->response_lease_charge);
    assert(!f->r->output_bytes && !f->r->output_inflight && hp_bridge_buffered_bytes() == 0);
    hp_request_release(f->r);
    pthread_cond_destroy(&f->cond); pthread_mutex_destroy(&f->mutex);
}
static void normal_borrow(void) {
    fixture_t f; init(&f); f.grant = true;
    char source[LARGE]; memset(source, 'A', sizeof(source));
    assert(hp_response_write(f.r, source, sizeof(source)) == sizeof(source));
    memset(source, 'X', sizeof(source));
    assert(f.acquisitions == 1 && f.writes == 1 && f.captured == LARGE);
    assert(f.r->state == HP_RESPONDING);
    for (size_t i = 0; i < LARGE; ++i) assert(f.bytes[i] == 'A');
    assert(hp_bridge_buffered_bytes() == META && f.r->output_inflight == META);
    unsigned notifications = f.notifications;
    hp_response_flush_notify(f.r);
    assert(f.notifications == notifications); /* Direct data is already flushed. */
    memset(source, 'B', sizeof(source));
    assert(hp_response_write(f.r, source, sizeof(source)) == sizeof(source));
    assert(f.acquisitions == 1 && f.writes == 2 && f.captured == 2 * LARGE);
    dispose(&f); assert(f.releases == 1);
}
static void irreversible_late_grant(void) {
    fixture_t f; init(&f);
    char source[LARGE]; memset(source, 'L', sizeof(source));
    assert(hp_response_write(f.r, source, sizeof(source)) == sizeof(source));
    assert(f.r->response_lease_attempted && !f.r->response_lease_requested && f.captured == LARGE);
    /* The owning loop resumes only after the caller returned and reused its
     * buffer: a late grant callback must not even execute. No sleeps/races. */
    memset(source, 'Z', sizeof(source)); f.grant = true;
    hp_response_lease_acquire(f.r, acquire, &f);
    assert(!f.acquisitions && !f.writes && !f.r->response_lease);
    assert(hp_response_write(f.r, source, sizeof(source)) == sizeof(source));
    assert(!f.acquisitions && !f.writes && f.captured == 2 * LARGE);
    for (size_t i = 0; i < LARGE; ++i) assert(f.bytes[i] == 'L');
    dispose(&f); assert(!f.releases);
}
typedef struct writer { fixture_t *f; const void *bytes; size_t length; ssize_t result; } writer_t;
static void *writer(void *data) {
    writer_t *w = data;
    w->result = hp_response_write(w->f->r, w->bytes, w->length);
    return NULL;
}
static void entered(fixture_t *f) {
    pthread_mutex_lock(&f->mutex);
    while (!f->entered) pthread_cond_wait(&f->cond, &f->mutex);
    pthread_mutex_unlock(&f->mutex);
}
static void auxiliary_yield(void) {
    fixture_t f; init(&f); f.grant = f.block = true;
    char source[LARGE]; memset(source, 'D', sizeof(source));
    writer_t first = { &f, source, LARGE, -2 }, auxiliary = { &f, "aux", 3, -2 };
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, writer, &first)); entered(&f);
    assert(!pthread_create(&b, NULL, writer, &auxiliary));
    pthread_mutex_lock(&f.r->mutex);
    while (!f.r->response_lease_return) pthread_cond_wait(&f.r->output_cond, &f.r->mutex);
    assert(f.r->response_lease_call_active && !f.r->output_bytes);
    pthread_mutex_unlock(&f.r->mutex);
    pthread_mutex_lock(&f.mutex); f.allow_exit = true; pthread_cond_broadcast(&f.cond); pthread_mutex_unlock(&f.mutex);
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    assert(first.result == LARGE && auxiliary.result == 3);
    receive(f.r);
    assert(f.captured == LARGE + 3 && !memcmp(f.bytes + LARGE, "aux", 3));
    for (size_t i = 0; i < LARGE; ++i) assert(f.bytes[i] == 'D');
    assert(f.writes == 1 && f.releases == 1 && !f.revokes);
    dispose(&f);
}
static void handshake_auxiliary_yield(unsigned terminal) {
    fixture_t f; init(&f); f.grant = f.block_notification = true;
    char source[LARGE]; memset(source, 'I', sizeof(source));
    writer_t first = { &f, source, LARGE, -2 }, auxiliary = { &f, "aux", 3, -2 };
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, writer, &first));
    pthread_mutex_lock(&f.mutex);
    while (!f.notification_entered) pthread_cond_wait(&f.cond, &f.mutex);
    pthread_mutex_unlock(&f.mutex);
    assert(!pthread_create(&b, NULL, writer, &auxiliary));
    pthread_mutex_lock(&f.r->mutex);
    while (!f.r->response_lease_return) pthread_cond_wait(&f.r->output_cond, &f.r->mutex);
    if (terminal == 2) f.r->deadline_ns = hp_now_ns() - 1;
    pthread_mutex_unlock(&f.r->mutex);
    if (terminal == 1) hp_request_cancel(f.r);
    pthread_mutex_lock(&f.mutex); f.notification_exit = true; pthread_cond_broadcast(&f.cond); pthread_mutex_unlock(&f.mutex);
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    assert(!f.acquisitions && !f.writes && !f.releases);
    if (terminal) {
        assert(first.result == -1 && auxiliary.result == -1 && !f.captured);
        if (terminal == 2) assert(f.r->state == HP_TIMED_OUT);
    } else {
        assert(first.result == LARGE && auxiliary.result == 3 && f.captured == LARGE + 3);
        for (size_t i = 0; i < LARGE; ++i) assert(f.bytes[i] == 'I');
        assert(!memcmp(f.bytes + LARGE, "aux", 3));
    }
    assert(!f.r->response_lease_fallback_active);
    dispose(&f);
}
static void cancelled_active_call(void) {
    fixture_t f; init(&f); f.grant = f.block = true;
    char source[LARGE]; memset(source, 'C', sizeof(source));
    writer_t w = { &f, source, LARGE, -2 }; pthread_t thread;
    assert(!pthread_create(&thread, NULL, writer, &w)); entered(&f);
    assert(hp_bridge_buffered_bytes() == LARGE + META);
    hp_request_cancel(f.r);
    assert(!pthread_join(thread, NULL)); receive(f.r);
    assert(w.result == -1 && f.captured == 37 && !f.r->output_head);
    assert(f.revokes && f.releases == 1);
    dispose(&f);
}
static void failures_and_finish(void) {
    char source[LARGE]; memset(source, 'F', sizeof(source));
    fixture_t f; init(&f); f.grant = f.fail = true;
    assert(hp_response_write(f.r, source, sizeof(source)) == -1);
    assert(f.captured == 37 && !f.r->output_head); dispose(&f);
    init(&f); f.grant = f.timeout = true;
    assert(hp_response_write(f.r, source, sizeof(source)) == -1);
    assert(f.r->state == HP_TIMED_OUT && f.captured == 37); dispose(&f);
    init(&f); f.grant = f.refuse = true;
    assert(hp_response_write(f.r, source, sizeof(source)) == sizeof(source));
    assert(f.writes == 1 && f.releases == 1 && f.captured == LARGE); dispose(&f);
    init(&f); f.grant = f.finish_in_write = true; f.r->state = HP_COMPLETED;
    assert(hp_response_write(f.r, source, sizeof(source)) == sizeof(source));
    assert(f.r->response_finish_published && f.r->state == HP_COMPLETED && f.releases == 1 && f.captured == LARGE); dispose(&f);
    init(&f); f.grant = f.cancel_in_write = true;
    assert(hp_response_write(f.r, source, sizeof(source)) == sizeof(source));
    assert(f.r->cancelled && f.r->state == HP_CANCELLED && f.releases == 1 && f.captured == LARGE); dispose(&f);
    init(&f); f.grant = f.reject = true;
    assert(hp_response_write(f.r, source, sizeof(source)) == sizeof(source));
    assert(f.acquisitions == 1 && !f.writes && !f.releases && f.captured == LARGE); dispose(&f);
    init(&f); f.grant = f.detach_at_entry = true;
    assert(hp_response_write(f.r, source, sizeof(source)) == sizeof(source));
    assert(f.releases == 1 && f.captured == LARGE); dispose(&f);
}
static void cancelled_handshake_and_deadline(void) {
    char source[LARGE]; memset(source, 'H', sizeof(source));
    fixture_t f; init(&f); f.grant = f.block_notification = true;
    writer_t w = { &f, source, LARGE, -2 }; pthread_t thread;
    assert(!pthread_create(&thread, NULL, writer, &w));
    pthread_mutex_lock(&f.mutex);
    while (!f.notification_entered) pthread_cond_wait(&f.cond, &f.mutex);
    pthread_mutex_unlock(&f.mutex);
    hp_request_cancel(f.r);
    pthread_mutex_lock(&f.mutex); f.notification_exit = true; pthread_cond_broadcast(&f.cond); pthread_mutex_unlock(&f.mutex);
    assert(!pthread_join(thread, NULL));
    assert(w.result == -1 && !f.acquisitions && !f.writes && !f.captured);
    dispose(&f);
    init(&f); f.grant = true; f.r->deadline_ns = hp_now_ns() - 1;
    assert(hp_response_write(f.r, source, sizeof(source)) == -1);
    assert(f.r->state == HP_TIMED_OUT && !f.acquisitions && !f.writes); dispose(&f);
}
static void shutdown_yield(void) {
    fixture_t f; init(&f); f.grant = f.block = true;
    char source[LARGE]; memset(source, 'S', sizeof(source));
    writer_t w = { &f, source, LARGE, -2 }; pthread_t thread;
    assert(!pthread_create(&thread, NULL, writer, &w)); entered(&f);
    hp_response_lease_yield(f.r);
    pthread_mutex_lock(&f.mutex); f.allow_exit = true; pthread_cond_broadcast(&f.cond); pthread_mutex_unlock(&f.mutex);
    assert(!pthread_join(thread, NULL));
    assert(w.result == LARGE && f.releases == 1 && !f.revokes);
    assert(hp_response_write(f.r, "tail", 4) == 4);
    assert(f.writes == 1 && f.captured == LARGE + 4);
    dispose(&f);
}
static void tight_budget_owned_progress(void) {
    char source[LARGE]; memset(source, 'P', sizeof(source));
    size_t original_output = hp_config.max_buffered_response, original_global = hp_config.max_buffered_global;
    const size_t limits[] = { 16384, 24576, LARGE, 262144 };
    for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
        hp_config.max_buffered_response = limits[i];
        hp_config.max_buffered_global = i == 3 ? 24576 : 1048576;
        fixture_t f; init(&f); f.grant = true;
        assert(hp_response_write(f.r, source, sizeof(source)) == sizeof(source));
        assert(!f.acquisitions && !f.writes && f.captured == LARGE);
        dispose(&f);
    }
    hp_config.max_buffered_response = original_output; hp_config.max_buffered_global = original_global;
    fixture_t f; init(&f); f.grant = true;
    assert(hp_response_write(f.r, "small", 5) == 5);
    assert(hp_response_write(f.r, source, sizeof(source)) == sizeof(source));
    assert(!f.acquisitions && !f.writes && f.captured == LARGE + 5); dispose(&f);
}
int main(void) {
    normal_borrow(); irreversible_late_grant(); auxiliary_yield(); cancelled_active_call();
    handshake_auxiliary_yield(0); handshake_auxiliary_yield(1); handshake_auxiliary_yield(2);
    failures_and_finish(); cancelled_handshake_and_deadline(); shutdown_yield(); tight_budget_owned_progress();
    puts("PASS: full CALL/metadata credit, reuse/flush, irreversible late grant, pre-entry pin, active/handshake auxiliary order, cancel/deadline wakes, terminal errors, finish, shutdown, tight-budget owned progress");
    return 0;
}
