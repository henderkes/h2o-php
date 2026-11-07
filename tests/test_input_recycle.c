/* Standalone input ownership/budget regression; includes the bridge so idle
 * capacity can be checked without adding a production diagnostics API. */
#include "../src/bridge/bridge.c"
#include <assert.h>

hp_runtime_config_t hp_config = { .max_buffered_body = 32 * 1024 * 1024,
    .max_buffered_response = 4 * 1024 * 1024, .max_buffered_global = 64 * 1024 * 1024,
    .request_timeout_ms = 2000 };
void hp_php_metrics_request_cancel_locked(hp_php_metric_request_t *r, uint64_t now) { (void)r; (void)now; }
void hp_php_metrics_request_end_locked(hp_php_metric_request_t *r, uint64_t now) { (void)r; (void)now; }

static char data[16 * 1024 * 1024], out[1024 * 1024];
static void check_budget(void) {
    assert(buffered + input_recycled_bytes <= hp_config.max_buffered_global);
    assert(input_recycled_bytes <= INPUT_RECYCLE_LIMIT);
    assert(input_recycled_bytes <= hp_config.max_buffered_global / 4);
}
static void discard_idle(void) {
    assert(buffered == 0);
    while (input_recycled) input_evict_one();
    assert(input_recycled_bytes == 0);
}
static void partial_read_reuse(void) {
    hp_request_t *r = hp_request_new();
    size_t len = 700000;
    assert(hp_request_body_push(r, data, len, true) == 0);
    hp_chunk_t *first = r->body_head;
    assert(first->capacity == 1048576 && hp_bridge_buffered_bytes() == 1048576);
    memset(data, 'B', len); /* The queued input is an independent copy. */
    assert(hp_request_body_read(r, out, 113) == 113);
    assert(out[0] == 'A' && out[112] == 'A');
    assert(hp_bridge_buffered_bytes() == 1048576 && input_recycled_bytes == 0);
    assert(hp_request_body_read(r, out, sizeof(out)) == len - 113);
    for (size_t i = 0; i < len - 113; ++i) assert(out[i] == 'A');
    assert(hp_request_body_read(r, out, 1) == 0);
    assert(hp_bridge_buffered_bytes() == 0 && input_recycled_bytes == 1048576);
    hp_request_release(r);
    r = hp_request_new();
    assert(hp_request_body_push(r, data, 800000, false) == 0);
    assert(r->body_head == first && hp_bridge_buffered_bytes() == 1048576);
    assert(r->body_bytes == 800000);
    hp_request_cancel(r); hp_request_release(r);
    assert(hp_bridge_buffered_bytes() == 0); check_budget();
}
static void bounded_and_pressure(void) {
    hp_request_t *requests[12];
    for (size_t i = 0; i < 12; ++i) {
        requests[i] = hp_request_new();
        assert(hp_request_body_push(requests[i], data, 1048576, true) == 0);
    }
    for (size_t i = 0; i < 12; ++i) hp_request_release(requests[i]);
    assert(input_recycled_bytes == INPUT_RECYCLE_LIMIT && buffered == 0); check_budget();
    discard_idle();
    hp_config.max_buffered_global = 4 * 1048576;
    hp_request_t *r = hp_request_new();
    assert(hp_request_body_push(r, data, 1048576, true) == 0);
    hp_request_release(r);
    assert(input_recycled_bytes == 1048576 && hp_bridge_buffered_bytes() == 0);
    r = hp_request_new();
    /* Idle input capacity cannot block output admission or consume live credit. */
    assert(hp_response_write(r, data, 3 * 1048576 + 524288) == 3 * 1048576 + 524288);
    assert(input_recycled_bytes == 0); check_budget();
    hp_request_release(r);
    /* Input also evicts idle capacity when its exact length fills the budget. */
    r = hp_request_new();
    assert(hp_request_body_push(r, data, 1048576, true) == 0); hp_request_release(r);
    r = hp_request_new();
    assert(hp_request_body_push(r, data, 4 * 1048576, true) == 0);
    assert(input_recycled_bytes == 0 && buffered == 4 * 1048576); check_budget();
    hp_request_release(r);
}
static void tight_nonfinal_rounding(void) {
    discard_idle();
    size_t body_limit = hp_config.max_buffered_body;
    size_t output_limit = hp_config.max_buffered_response;
    hp_config.max_buffered_body = 1048576;
    hp_config.max_buffered_response = 16384;
    hp_config.max_buffered_global = 1048576 + 16384;
    hp_request_t *r = hp_request_new();
    assert(hp_request_body_push(r, data, 786432, false) == 0);
    assert(hp_request_body_read(r, out, 113) == 113);
    /* A partial read keeps the first allocation live. Optional rounding of
     * that earlier prefix must not consume the tail's required capacity. */
    assert(hp_request_body_push(r, data + 786432, 262144, true) == 0);
    assert(hp_bridge_buffered_bytes() == 1048576);
    assert(hp_response_write(r, "ok", 2) == 2);
    assert(hp_request_body_read(r, out + 113, sizeof(out) - 113) == sizeof(out) - 113);
    assert(memcmp(out, data, sizeof(out)) == 0);
    assert(hp_request_body_read(r, out, 1) == 0);
    hp_request_release(r);
    assert(hp_bridge_buffered_bytes() == 0); check_budget(); discard_idle();
    hp_config.max_buffered_body = body_limit;
    hp_config.max_buffered_response = output_limit;
    hp_config.max_buffered_global = 64 * 1048576;
}
static void small_budget_and_cleanup(void) {
    hp_config.max_buffered_global = 300000;
    hp_request_t *r = hp_request_new();
    /* Exact allocation remains possible when the rounded size cannot fit. */
    assert(hp_request_body_push(r, data, 280000, true) == 0);
    assert(r->body_head->capacity == 280000 && buffered == 280000);
    hp_response_finish(r); hp_request_release(r);
    assert(buffered == 0 && input_recycled_bytes == 0);
    hp_config.max_buffered_global = 16384;
    r = hp_request_new();
    assert(hp_request_body_push(r, data, 4096, true) == 0);
    assert(hp_response_write(r, data, 4096) == 4096);
    hp_request_release(r); assert(buffered == 0); check_budget();
    hp_config.max_buffered_global = 64 * 1048576;
    r = hp_request_new(); assert(hp_request_body_push(r, data, 1048576, true) == 0); hp_request_release(r);
    assert(input_recycled_bytes == 1048576);
    hp_bridge_cleanup(); assert(input_recycled_bytes == 0 && !input_recycled);
    r = hp_request_new(); assert(hp_request_body_push(r, data, 1048576, true) == 0); hp_request_release(r);
    assert(buffered == 0 && input_recycled_bytes == 0);
}
int main(void) {
    memset(data, 'A', sizeof(data));
    partial_read_reuse(); bounded_and_pressure(); tight_nonfinal_rounding(); small_budget_and_cleanup();
    puts("PASS: input copy/partial read/EOF, bounded reuse, capacity credit, input/output eviction, tight nonfinal tail, cancellation, cleanup");
}
