/* Detached input returns are independent of request lifetime. No consumer
 * thread frees the storage or releases its global capacity before origin drain. */
#include "../src/bridge/bridge.c"
#include <assert.h>

hp_runtime_config_t hp_config = { .max_buffered_body = 1048576,
    .max_buffered_response = 16384, .max_buffered_global = 8388608,
    .request_timeout_ms = 2000 };
void hp_php_metrics_request_cancel_locked(hp_php_metric_request_t *r, uint64_t now) { (void)r; (void)now; }
void hp_php_metrics_request_end_locked(hp_php_metric_request_t *r, uint64_t now) { (void)r; (void)now; }

static pthread_t origin;
static unsigned published, released;
static char out[1048576];
struct owner { char *data; size_t charged; struct owner *next; };
static struct owner *pending;
static void return_owner(void *p, size_t capacity) {
    struct owner *o = p;
    /* A stand-in for the existing receiver's MPSC publication. The test joins
     * its sole publisher before origin drain, so no queue lock is necessary. */
    o->charged = capacity; o->next = pending; pending = o; ++published;
}
static struct owner *owner(void) {
    struct owner *o = calloc(1, sizeof(*o)); assert(o);
    o->data = malloc(2097152); assert(o->data); memset(o->data, 'Q', 1048576); return o;
}
static void drain_origin(void) {
    assert(pthread_equal(pthread_self(), origin));
    while (pending) {
        struct owner *o = pending; pending = o->next;
        /* Actual handler uses h2o_buffer_dispose here on the origin loop. */
        free(o->data); ++released;
        hp_request_body_release_owned_capacity(o->charged); free(o);
    }
}
static void run_thread(void *(*fn)(void *), hp_request_t *r) {
    pthread_t thread; assert(!pthread_create(&thread, NULL, fn, r));
    assert(!pthread_join(thread, NULL));
}
static void *read_rest(void *p) {
    assert(!pthread_equal(pthread_self(), origin));
    hp_request_t *r = p;
    assert(hp_request_body_read(r, out + 113, sizeof(out) - 113) == sizeof(out) - 113);
    assert(hp_request_body_read(r, out, 1) == 0);
    hp_request_release(r); return NULL;
}
static void partial_and_eof(void) {
    hp_request_t *r = hp_request_new(); struct owner *o = owner();
    assert(hp_request_body_push_owned(r, o, o->data, 1048576, 2097152, return_owner, true) == 1);
    assert(r->body_head->data == o->data && hp_bridge_buffered_bytes() == 2097152);
    assert(hp_request_body_read(r, out, 113) == 113 && !published);
    assert(hp_bridge_buffered_bytes() == 2097152);
    run_thread(read_rest, r); /* Even request destruction cannot free owner. */
    for (size_t i = 0; i < sizeof(out); ++i) assert(out[i] == 'Q');
    assert(published == 1 && !released && pending == o && o->data[0] == 'Q');
    assert(hp_bridge_buffered_bytes() == 2097152);
    /* Pending returns retain their full charge and cannot admit excess input. */
    hp_config.max_buffered_global = 2097152;
    hp_request_t *blocked = hp_request_new();
    assert(hp_request_body_push(blocked, "x", 1, true) == -1);
    hp_request_release(blocked);
    drain_origin(); assert(released == 1 && hp_bridge_buffered_bytes() == 0);
    hp_config.max_buffered_global = 8388608;
}
static void *cancel_owner(void *p) { hp_request_cancel(p); hp_request_release(p); return NULL; }
static void *finish_owner(void *p) { hp_response_finish(p); hp_request_release(p); return NULL; }
static void *destroy_owner(void *p) { hp_request_release(p); return NULL; }
static void cancel_and_finish(void) {
    void *(*actions[])(void *) = { cancel_owner, finish_owner, destroy_owner };
    for (size_t i = 0; i < sizeof(actions) / sizeof(*actions); ++i) {
        hp_request_t *r = hp_request_new(); struct owner *o = owner(); unsigned before = released;
        assert(hp_request_body_push_owned(r, o, o->data, 1048576, 2097152, return_owner, true) == 1);
        run_thread(actions[i], r);
        assert(released == before && pending == o && hp_bridge_buffered_bytes() == 2097152);
        drain_origin(); assert(released == before + 1 && hp_bridge_buffered_bytes() == 0);
    }
    hp_request_t *r = hp_request_new(); hp_response_finish(r); struct owner *o = owner();
    assert(hp_request_body_push_owned(r, o, o->data, 1048576, 2097152, return_owner, true) == 1);
    assert(pending == o && o->charged == 0 && hp_bridge_buffered_bytes() == 0);
    hp_request_release(r); drain_origin();
}
static void budget_fallback(void) {
    for (unsigned i = 0; i != 2; ++i) {
        hp_config.max_buffered_global = i ? 2097152 : 1048576 + 16384;
        hp_request_t *r = hp_request_new(); struct owner *o = owner(); unsigned before = published;
        assert(hp_request_body_push_owned(r, o, o->data, 1048576, 2097152, return_owner, true) == 0);
        assert(published == before && !r->body_head && hp_bridge_buffered_bytes() == 0);
        assert(hp_request_body_push(r, o->data, 1048576, true) == 0);
        free(o->data); free(o); /* Fallback retains origin ownership. */
        assert(hp_response_write(r, "ok", 2) == 2); /* Unread input leaves output room. */
        assert(hp_request_body_read(r, out, sizeof(out)) == sizeof(out));
        hp_request_release(r); assert(hp_bridge_buffered_bytes() == 0);
    }
}
static void shutdown_pending(void) {
    hp_config.max_buffered_global = 8388608;
    hp_request_t *r = hp_request_new(); struct owner *o = owner();
    assert(hp_request_body_push_owned(r, o, o->data, 1048576, 2097152, return_owner, true) == 1);
    run_thread(finish_owner, r);
    hp_bridge_cleanup(); /* Controller cleanup can precede receiver drain. */
    assert(hp_bridge_buffered_bytes() == 2097152 && pending == o);
    drain_origin(); assert(hp_bridge_buffered_bytes() == 0);
}
static void *read_nonfinal(void *p) {
    hp_request_t *r = p;
    assert(hp_request_body_read(r, out + 113, sizeof(out) - 113) == sizeof(out) - 113);
    assert(!r->body_eof); /* Exhausting a fragment must not manufacture EOF. */
    return NULL;
}
static void nonfinal_fragments(void) {
    hp_config.max_buffered_global = 8388608;
    hp_request_t *r = hp_request_new();
    struct owner *first = owner(), *second = owner();
    unsigned before = published;
    assert(hp_request_body_push_owned(r, first, first->data, sizeof(out), 2097152, return_owner, false) == 1);
    assert(hp_request_body_read(r, out, 113) == 113 && published == before);
    run_thread(read_nonfinal, r);
    assert(published == before + 1 && pending == first && !r->body_eof);
    assert(hp_bridge_buffered_bytes() == 2097152);
    assert(hp_request_body_push_owned(r, second, second->data, sizeof(out), 2097152, return_owner, false) == 1);
    assert(hp_bridge_buffered_bytes() == 4194304);
    assert(hp_request_body_read(r, out, 113) == 113);
    run_thread(read_nonfinal, r);
    assert(published == before + 2 && pending == second && second->next == first);
    assert(hp_bridge_buffered_bytes() == 4194304);
    assert(hp_request_body_push(r, "tail", 4, true) == 0);
    assert(hp_request_body_read(r, out, 4) == 4 && memcmp(out, "tail", 4) == 0);
    assert(hp_request_body_read(r, out, 1) == 0);
    hp_request_release(r); drain_origin(); assert(hp_bridge_buffered_bytes() == 0);

    r = hp_request_new(); first = owner(); before = published;
    assert(hp_request_body_push_owned(r, first, first->data, sizeof(out), 2097152, return_owner, false) == 1);
    assert(hp_request_body_read(r, out, sizeof(out)) == sizeof(out));
    r->deadline_ns = hp_now_ns() - 1;
    assert(hp_request_body_read(r, out, 1) == -1 && r->state == HP_TIMED_OUT);
    assert(published == before + 1 && pending == first && hp_bridge_buffered_bytes() == 2097152);
    hp_request_release(r); drain_origin(); assert(hp_bridge_buffered_bytes() == 0);

    /* A refused nonfinal adoption leaves EOF false through the exact-length
     * fallback, and outstanding returns still reserve their full capacity. */
    hp_config.max_buffered_global = sizeof(out) + 16384;
    r = hp_request_new(); first = owner(); before = published;
    assert(hp_request_body_push_owned(r, first, first->data, sizeof(out), 2097152, return_owner, false) == 0);
    assert(published == before && !r->body_eof && hp_bridge_buffered_bytes() == 0);
    assert(hp_request_body_push(r, first->data, sizeof(out), false) == 0);
    free(first->data); free(first);
    assert(hp_request_body_read(r, out, sizeof(out)) == sizeof(out) && !r->body_eof);
    assert(hp_request_body_push(r, "tail", 4, true) == 0);
    assert(hp_request_body_read(r, out, 4) == 4 && memcmp(out, "tail", 4) == 0);
    assert(hp_request_body_read(r, out, 1) == 0);
    hp_request_release(r); assert(hp_bridge_buffered_bytes() == 0);
    hp_config.max_buffered_global = 8388608;
    void *(*actions[])(void *) = { cancel_owner, finish_owner, destroy_owner };
    for (size_t i = 0; i != sizeof(actions) / sizeof(*actions); ++i) {
        r = hp_request_new(); first = owner(); before = published;
        assert(hp_request_body_push_owned(r, first, first->data, sizeof(out), 2097152, return_owner, false) == 1);
        assert(hp_request_body_read(r, out, 113) == 113);
        run_thread(actions[i], r);
        assert(published == before + 1 && pending == first && hp_bridge_buffered_bytes() == 2097152);
        drain_origin(); assert(hp_bridge_buffered_bytes() == 0);
    }
}
int main(void) {
    origin = pthread_self(); partial_and_eof(); cancel_and_finish(); budget_fallback(); nonfinal_fragments(); shutdown_pending();
    assert(!pending && released == published);
    puts("PASS: deferred origin return, request-independent lifetime, partial/nonfinal/EOF, cancel/finish, pressure/fallback, shutdown charge");
}
