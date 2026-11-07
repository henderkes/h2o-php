/* Standalone ownership/credit regression: compile with bridge.c and -pthread. */
#include "h2o_php.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

hp_runtime_config_t hp_config = { .max_buffered_body = 4096, .max_buffered_response = 32768,
    .max_buffered_global = 40960, .request_timeout_ms = 2000 };
void hp_php_metrics_request_cancel_locked(hp_php_metric_request_t *r, uint64_t now) { (void)r; (void)now; }
void hp_php_metrics_request_end_locked(hp_php_metric_request_t *r, uint64_t now) { (void)r; (void)now; }

static void partial_batch(void) {
    char source[32768]; memset(source, 'A', 16384); memset(source + 16384, 'B', 16384);
    hp_request_t *r = hp_request_new(); assert(r);
    assert(hp_response_write(r, source, sizeof(source)) == sizeof(source));
    memset(source, 'X', sizeof(source)); /* PHP can immediately reuse its buffer. */
    bool finished;
    hp_chunk_t *first = hp_response_pop_batch(r, 1, 16384, &finished);
    assert(first && !first->next && first->len == 16384 && !finished);
    for (size_t i = 0; i < first->len; ++i) assert(first->data[i] == 'A');
    hp_response_release_chunks(r, first);
    /* A partial H2O batch must not release credit for payload still allocated. */
    assert(hp_bridge_buffered_bytes() == 32768 && r->output_inflight == 16384 && r->output_bytes == 16384);
    hp_chunk_t *second = hp_response_pop_batch(r, 1, 16384, &finished);
    assert(second && !second->next && second->len == 16384);
    for (size_t i = 0; i < second->len; ++i) assert(second->data[i] == 'B');
    hp_response_release_chunks(r, second);
    assert(hp_bridge_buffered_bytes() == 0 && r->output_inflight == 0 && r->output_bytes == 0);
    hp_request_release(r);
}

static void cancellation_cleanup(void) {
    char source[32768]; memset(source, 'C', sizeof(source));
    hp_request_t *r = hp_request_new(); assert(r);
    assert(hp_request_body_push(r, "body", 4, false) == 0);
    assert(hp_response_write(r, source, sizeof(source)) == sizeof(source));
    bool finished;
    hp_chunk_t *first = hp_response_pop_batch(r, 1, 16384, &finished);
    hp_request_cancel(r);
    hp_response_release_chunks(r, first); /* The second slice remains queued. */
    assert(hp_bridge_buffered_bytes() == 32772);
    hp_request_release(r); /* Reclaims queued slice and unread body once. */
    assert(hp_bridge_buffered_bytes() == 0);
}

static size_t consumed, retained, first_chunk, notifications;
static void drain(hp_request_t *r) {
    assert(hp_bridge_buffered_bytes() <= hp_config.max_buffered_global);
    bool finished; hp_chunk_t *chunks = hp_response_pop_batch(r, 16, 262144, &finished);
    if (!chunks) { assert(r->cancelled); return; }
    assert(!finished);
    if (!notifications++) first_chunk = chunks->len;
    for (hp_chunk_t *p = chunks; p; p = p->next) {
        assert(p->len <= 16384);
        for (size_t i = 0; i < p->len; ++i) assert(p->data[i] == 'D');
        consumed += p->len;
    }
    hp_response_release_chunks(r, chunks);
    assert(hp_bridge_buffered_bytes() == retained); /* Unread input is retained. */
    (void)hp_response_pop_batch(r, 16, 262144, &finished); /* Consumer is idle. */
}
static void unread_input_progress(void) {
    char source[65536]; memset(source, 'D', sizeof(source));
    const size_t budgets[] = {16384, 24576};
    const size_t bodies[] = {4096, 3073};
    for (size_t b = 0; b < sizeof(budgets) / sizeof(*budgets); ++b) {
        for (size_t i = 0; i < sizeof(bodies) / sizeof(*bodies); ++i) {
            hp_config.max_buffered_global = budgets[b];
            hp_request_t *r = hp_request_new(); assert(r);
            retained = bodies[i]; consumed = first_chunk = notifications = 0;
            /* Two independently allocated chunks retain the same full input
             * capacity even when their boundary is not an output boundary. */
            size_t prefix = i ? 113 : 2047;
            assert(hp_request_body_push(r, source, prefix, false) == 0);
            assert(hp_request_body_push(r, source + prefix, retained - prefix, false) == 0);
            assert(r->body_head->capacity == prefix);
            assert(r->body_tail->capacity == retained - prefix);
            assert(hp_bridge_buffered_bytes() == retained);
            r->notify = drain;
            assert(hp_response_write(r, source, sizeof(source)) == sizeof(source));
            assert(consumed == sizeof(source) && notifications > 1);
            size_t available = budgets[b] - retained;
            assert(first_chunk == (available < 16384 ? available : 16384));
            assert(r->body_bytes == retained && !r->body_eof);
            r->notify = NULL;
            char input[4096];
            assert(hp_request_body_read(r, input, retained) == (ssize_t)retained);
            for (size_t j = 0; j < retained; ++j) assert(input[j] == 'D');
            hp_request_release(r);
            assert(hp_bridge_buffered_bytes() == 0);
        }
    }
}

static bool finish_under_pressure;
static void stop_reduced_batch(hp_request_t *r) {
    bool finished;
    hp_chunk_t *chunks = hp_response_pop_batch(r, 1, 16384, &finished);
    assert(chunks && !chunks->next && chunks->len == 12288 && !finished);
    assert(hp_bridge_buffered_bytes() == 16384 && r->output_inflight == 12288);
    r->notify = NULL;
    if (finish_under_pressure) hp_response_finish(r); else hp_request_cancel(r);
    hp_response_release_chunks(r, chunks);
    assert(hp_bridge_buffered_bytes() == (finish_under_pressure ? 0 : 4096));
}
static void reduced_batch_terminal_cleanup(void) {
    char source[65536]; memset(source, 'E', sizeof(source));
    hp_config.max_buffered_global = 16384;
    for (unsigned i = 0; i < 2; ++i) {
        hp_request_t *r = hp_request_new(); assert(r);
        assert(hp_request_body_push(r, source, 4096, false) == 0);
        finish_under_pressure = i;
        r->notify = stop_reduced_batch;
        ssize_t written = hp_response_write(r, source, sizeof(source));
        assert(written == (i ? (ssize_t)sizeof(source) : -1));
        hp_request_release(r);
        assert(hp_bridge_buffered_bytes() == 0);
    }
}

int main(void) {
    partial_batch(); cancellation_cleanup(); unread_input_progress(); reduced_batch_terminal_cleanup();
    puts("PASS: partial H2O batch ownership, cancellation cleanup, 16/24 KiB unread input progress, reduced-batch finish/cancel");
    return 0;
}
