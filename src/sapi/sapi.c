/* Native FrankenPHP-compatible SAPI for H2O.
 * Worker lifecycle portions adapted from php/frankenphp
 * 4384c506d219cd2f8750edfceae57d3db8eb54f3.
 * Copyright (c) 2022-present Kévin Dunglas, MIT; see ../php/LICENSE.frankenphp.
 * Zend execution and bailout recovery remain entirely inside this C unit. */
#include "h2o_php.h"
#include "environment.h"
#include "parallel_host.h"
#include "../observability/log.h"
#include <php.h>
#include <SAPI.h>
#include <zend_API.h>
#include <php_main.h>
#include <php_ini.h>
#include <php_output.h>
#include <php_variables.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_interfaces.h>
#include <Zend/zend_smart_str.h>
#include <Zend/zend_signal.h>
#include <Zend/zend_virtual_cwd.h>
#include <ext/spl/spl_exceptions.h>
#include <ext/standard/basic_functions.h>
#include <ext/standard/php_fopen_wrappers.h>
#include <ext/standard/info.h>
#include <ext/session/php_session.h>
#include <ext/json/php_json.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#ifndef ZTS
#error "h2o-php requires the installed ZTS PHP runtime"
#endif
#if PHP_VERSION_ID < 80500 || PHP_VERSION_ID >= 80700
#error "This SAPI lifecycle port requires PHP 8.5 or 8.6 headers and library"
#endif

static _Thread_local hp_request_t *current_request;
static _Thread_local hp_worker_group_t *current_group;
static _Thread_local hp_request_t dummy_request;
static _Thread_local bool worker_bootstrapped;
static _Thread_local bool partial_active;
static _Thread_local bool http_finished;
static _Thread_local bool sapi_final_headers_sent;
static _Thread_local bool sapi_headers_finishing;
static _Thread_local unsigned worker_requests;
typedef struct hp_php_owner {
    atomic_uint refs;
    void *token; /* slots_mutex protects token and slot registry membership */
    pthread_mutex_t output_mutex;
} hp_php_owner_t;
#define HP_LEASE_MAGIC UINT64_C(0x48504c4541534531)
typedef struct hp_php_lease {
    uint64_t magic;
    atomic_uint refs;
    atomic_bool child_attached;
    atomic_bool spawn_pending;
    hp_php_owner_t *owner;
    hp_request_t *request;
    hp_header_t *env;
    char *script, *root;
    sigset_t parent_mask;
    bool have_parent_mask;
} hp_php_lease_t;
static _Thread_local hp_php_owner_t *local_owner;
static _Thread_local hp_php_lease_t *local_lease;
static _Thread_local bool local_auxiliary;
static atomic_uint auxiliary_threads;
static sapi_module_struct hp_sapi_module;
static char *configured_ini;
static bool engine_started;
static bool default_ignore_user_abort;
/* Startup-only explicit filter.default gate, matching the reference SAPI. */
static bool filter_server_variables;

#include "../php/frankenphp_arginfo.h"

typedef struct hp_php_slot {
    struct hp_php_slot *next;
    hp_request_t *request;
    void *worker_token;
    hp_php_owner_t *owner;
    bool registered;
    bool auxiliary, busy;
    pthread_t thread;
    zend_atomic_bool *interrupt, *timed_out;
} hp_php_slot_t;
static pthread_mutex_t slots_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t slots_cond = PTHREAD_COND_INITIALIZER;
static hp_php_slot_t *slots;
static _Thread_local hp_php_slot_t local_slot;
static bool auxiliary_fork_gate;
static pthread_t auxiliary_fork_owner;
static unsigned pending_spawns;

static void wait_auxiliary_gate(void)
{
    while (auxiliary_fork_gate && !pthread_equal(auxiliary_fork_owner, pthread_self()))
        pthread_cond_wait(&slots_cond, &slots_mutex);
}

void hp_sapi_interrupt_request(hp_request_t *request)
{
    if (!request || hp_process_is_fork_child()) return;
    pthread_mutex_lock(&slots_mutex);
    for (hp_php_slot_t *slot = slots; slot; slot = slot->next) {
        if (request == slot->request) {
            zend_atomic_bool_store(slot->timed_out, true);
            zend_atomic_bool_store(slot->interrupt, true);
        }
    }
    pthread_mutex_unlock(&slots_mutex);
}

void hp_sapi_set_worker_token(void *token)
{
    if (hp_process_is_fork_child()) return;
    pthread_mutex_lock(&slots_mutex);
    local_slot.worker_token = token;
    if (local_owner) local_owner->token = token;
    pthread_mutex_unlock(&slots_mutex);
}

void hp_sapi_interrupt_worker(void *token)
{
    if (!token || hp_process_is_fork_child()) return;
    pthread_mutex_lock(&slots_mutex);
    for (hp_php_slot_t *slot = slots; slot; slot = slot->next) {
        if ((slot->owner ? slot->owner->token : slot->worker_token) == token) {
            zend_atomic_bool_store(slot->timed_out, true);
            zend_atomic_bool_store(slot->interrupt, true);
        }
    }
    pthread_mutex_unlock(&slots_mutex);
}

static void publish_request(hp_request_t *request)
{
    pthread_mutex_lock(&slots_mutex);
    local_slot.request = request;
    pthread_mutex_unlock(&slots_mutex);
}

static void initialize_opcache_scalars(void);
static void frankenphp_reset_super_globals(void);
static void install_context(hp_request_t *);
static void register_parallel_hooks(void);
static void unregister_slot(void);

static hp_php_owner_t *owner_new(void)
{
    hp_php_owner_t *owner = calloc(1, sizeof(*owner));
    if (!owner) zend_error_noreturn(E_ERROR, "Cannot allocate native PHP context");
    atomic_init(&owner->refs, 1);
    if (pthread_mutex_init(&owner->output_mutex, NULL)) abort();
    return owner;
}
static void owner_retain(hp_php_owner_t *owner) { atomic_fetch_add_explicit(&owner->refs, 1, memory_order_relaxed); }
static void owner_release(hp_php_owner_t *owner)
{
    if (owner && atomic_fetch_sub_explicit(&owner->refs, 1, memory_order_acq_rel) == 1) {
        pthread_mutex_destroy(&owner->output_mutex); free(owner);
    }
}
static void lease_retain(hp_php_lease_t *lease) { atomic_fetch_add_explicit(&lease->refs, 1, memory_order_relaxed); }
static void lease_release(void *opaque)
{
    hp_php_lease_t *lease = opaque;
    if (!lease || atomic_fetch_sub_explicit(&lease->refs, 1, memory_order_acq_rel) != 1) return;
    if (lease->request) hp_request_release(lease->request);
    hp_headers_free(lease->env); free(lease->script); free(lease->root);
    owner_release(lease->owner); free(lease);
}
static void *lease_capture(void)
{
    if (hp_process_is_fork_child()) return NULL;
    pthread_mutex_lock(&slots_mutex);
    if (!(local_auxiliary && local_slot.busy)) wait_auxiliary_gate();
    pthread_mutex_unlock(&slots_mutex);
    if (!local_owner) local_owner = owner_new();
    hp_php_lease_t *lease = calloc(1, sizeof(*lease));
    if (!lease) zend_error_noreturn(E_ERROR, "Cannot allocate parallel request lease");
    lease->magic = HP_LEASE_MAGIC; atomic_init(&lease->refs, 1); atomic_init(&lease->child_attached, false); atomic_init(&lease->spawn_pending, false);
    lease->owner = local_owner; owner_retain(lease->owner);
    if (current_request && current_request != &dummy_request) {
        lease->request = current_request; hp_request_retain(lease->request);
    }
    /* FrankenPHP's sandbox/prepared/worker env tables belong to the calling
     * OS thread. A parallel interpreter starts from process env, not a parent
     * PHP overlay; its own bootstrap and task changes remain persistent. */
    lease->script = strdup(current_request && current_request->script ? current_request->script : "");
    lease->root = strdup(current_request && current_request->document_root ? current_request->document_root : "");
    if (!lease->script || !lease->root) { lease_release(lease); zend_error_noreturn(E_ERROR, "Cannot copy parallel request metadata"); }
    return lease;
}
static int pending_output(void)
{
    php_output_handler **handlers = zend_stack_base(&OG(handlers));
    for (int i = 0; i < zend_stack_count(&OG(handlers)); ++i)
        if (handlers[i]->buffer.used) return 1;
    return 0;
}
/* newWorkerDummyContext uses the basename URI and a nil split list, which
 * defaults to .php independently of the HTTP server's configured delimiters.
 * The first embedded occurrence rejects this delimiter, as in splitPos. */
static void set_worker_dummy_cgi_split(hp_request_t *request)
{
    const char *name = request->uri;
    size_t length = strlen(name);
    request->document_uri_set = true;
    request->document_uri = request->path_info = NULL;
    for (size_t i = 0; i + 4 <= length; ++i) {
        if (name[i] != '.' || (name[i + 1] != 'p' && name[i + 1] != 'P') ||
            (name[i + 2] != 'h' && name[i + 2] != 'H') || (name[i + 3] != 'p' && name[i + 3] != 'P')) continue;
        if (i + 4 == length) {
            request->document_uri = request->uri;
            request->path_info = request->uri + length;
        }
        break;
    }
}
static void bind_lease(hp_php_lease_t *lease)
{
    if (lease == local_lease) return;
    lease_retain(lease);
    hp_php_lease_t *old = local_lease; local_lease = lease;
    if (current_request && current_request != &dummy_request)
        (void)hp_sapi_response_publish(current_request);
    current_request = lease->request;
    if (!current_request) {
        memset(&dummy_request, 0, sizeof(dummy_request));
        const char *slash = strrchr(lease->script, '/');
        dummy_request.method = "GET";
        dummy_request.uri = (char *)(slash ? slash + 1 : lease->script);
        dummy_request.script_name = (char *)(slash ? slash : "/");
        if (slash) set_worker_dummy_cgi_split(&dummy_request);
        dummy_request.query = ""; dummy_request.script = lease->script; dummy_request.document_root = lease->root;
        dummy_request.content_length = SIZE_MAX;
        current_request = &dummy_request;
    }
    publish_request(lease->request);
    http_finished = false;
    sapi_final_headers_sent = false;
    sapi_headers_finishing = false;
    /* The extension creates one full PHP request per Runtime. Leave its fresh
     * SG metadata and superglobals intact across tasks; only output/callback
     * ownership follows the per-task lease. No parent's Zend data is borrowed. */
    SG(request_info).no_headers = true;
    SG(request_info).headers_only = false;
    lease_release(old);
}
static void parallel_task_enter(void *opaque)
{
    if (hp_process_is_fork_child() || !opaque) return;
    hp_php_lease_t *lease = opaque;
    if (!local_auxiliary || lease->magic != HP_LEASE_MAGIC) return;
    if (local_lease && local_lease->request != lease->request) {
        php_output_flush_all();
        if (pending_output()) {
            zend_throw_exception(spl_ce_RuntimeException,
                "Cannot switch parallel HTTP request: an output buffer from the previous task cannot be flushed", 0);
            return;
        }
    }
    bind_lease(lease);
}
static void parallel_task_leave(void *opaque)
{
    (void)opaque;
    if (!hp_process_is_fork_child() && local_auxiliary) {
        php_output_flush_all();
        if (current_request && current_request != &dummy_request)
            (void)hp_sapi_response_publish(current_request);
    }
    /* Keep the lease through idle time and shutdown hooks. A next task first
     * flushes old buffers before changing generation, never into a new reply. */
}
static int parallel_native_output(void) { return !hp_process_is_fork_child() && local_slot.registered; }
static void parallel_thread_idle(void *opaque)
{
    (void)opaque;
    if (!local_auxiliary || hp_process_is_fork_child()) return;
    pthread_mutex_lock(&slots_mutex);
    local_slot.busy = false; pthread_cond_broadcast(&slots_cond);
    wait_auxiliary_gate();
    pthread_mutex_unlock(&slots_mutex);
}
static void parallel_thread_busy(void *opaque)
{
    (void)opaque;
    if (!local_auxiliary || hp_process_is_fork_child()) return;
    pthread_mutex_lock(&slots_mutex);
    wait_auxiliary_gate(); local_slot.busy = true;
    pthread_mutex_unlock(&slots_mutex);
}
static void *parallel_spawn_prepare(void *opaque)
{
    if (!opaque || hp_process_is_fork_child()) return NULL;
    hp_php_lease_t *lease = opaque;
    pthread_mutex_lock(&slots_mutex);
    wait_auxiliary_gate();
    atomic_store(&lease->spawn_pending, true); ++pending_spawns;
    pthread_mutex_unlock(&slots_mutex);
    hp_pcntl_spawn_prepare(&lease->parent_mask);
    lease->have_parent_mask = true;
    return lease;
}
static void parallel_spawn_parent(void *opaque, int created)
{
    if (!opaque || hp_process_is_fork_child()) return;
    hp_php_lease_t *lease = opaque;
    if (lease->have_parent_mask) hp_pcntl_spawn_restore(&lease->parent_mask);
    if (created) return;
    pthread_mutex_lock(&slots_mutex);
    if (atomic_exchange(&lease->spawn_pending, false)) --pending_spawns;
    pthread_cond_broadcast(&slots_cond); pthread_mutex_unlock(&slots_mutex);
}

static void parallel_thread_ready(void *opaque)
{
    hp_php_lease_t *lease = opaque;
    if (local_auxiliary && !hp_process_is_fork_child()) {
        if (lease && lease->have_parent_mask) hp_pcntl_spawn_restore(&lease->parent_mask);
        hp_pcntl_context_enter();
    }
}
static void parallel_thread_done(void *opaque)
{
    (void)opaque;
    hp_php_lease_t *base = opaque;
    if (hp_process_is_fork_child()) return;
    pthread_mutex_lock(&slots_mutex);
    if (base && atomic_exchange(&base->spawn_pending, false)) --pending_spawns;
    pthread_cond_broadcast(&slots_cond); pthread_mutex_unlock(&slots_mutex);
    if (!local_auxiliary) return;
    /* parallel has already freed its own TSRM context. Only C ownership remains. */
    if (local_lease && local_lease->request)
        (void)hp_sapi_response_publish(local_lease->request);
    lease_release(local_lease); local_lease = NULL; current_request = NULL;
    owner_release(local_owner); local_owner = NULL; local_auxiliary = false;
}
static void parallel_thread_stopping(void *opaque)
{
    (void)opaque;
    /* TSRM's virtual-CWD constructor allocates before the first RINIT, while
     * its destructor only frees the realpath cache. This is also needed when
     * request startup/shutdown bailed before ordinary CWD deactivation. */
    virtual_cwd_deactivate();
    if (!local_auxiliary || hp_process_is_fork_child()) return;
    /* A shutdown callback can bail before sapi_deactivate. Remove all native
     * references to Zend TLS before parallel frees that interpreter. Its PHP
     * heap may already be gone; only clear environment pointers here. */
    hp_pcntl_context_leave();
    if (local_slot.registered) unregister_slot();
    hp_env_after_shutdown();
    if (current_request && current_request != &dummy_request)
        (void)hp_sapi_response_publish(current_request);
    current_request = NULL; SG(server_context) = NULL;
}
static void parallel_thread_joined(void *opaque)
{
    hp_php_lease_t *lease = opaque;
    if (hp_process_is_fork_child() || !lease) return;
    if (atomic_exchange_explicit(&lease->child_attached, false, memory_order_acq_rel)) {
        atomic_fetch_sub_explicit(&auxiliary_threads, 1, memory_order_release);
        pthread_mutex_lock(&slots_mutex); pthread_cond_broadcast(&slots_cond); pthread_mutex_unlock(&slots_mutex);
    }
}
int hp_sapi_fork_prepare(uint64_t timeout_ms)
{
    uint64_t now = hp_now_ns();
    uint64_t end = timeout_ms > (UINT64_MAX - now) / 1000000 ? UINT64_MAX : now + timeout_ms * 1000000;
    pthread_mutex_lock(&slots_mutex);
    if (auxiliary_fork_gate) { pthread_mutex_unlock(&slots_mutex); return -1; }
    auxiliary_fork_gate = true; auxiliary_fork_owner = pthread_self();
    for (;;) {
        bool busy = pending_spawns != 0;
        for (hp_php_slot_t *slot = slots; slot; slot = slot->next)
            if (slot->auxiliary && slot->busy && !pthread_equal(slot->thread, pthread_self())) busy = true;
        if (!busy) { pthread_mutex_unlock(&slots_mutex); return 0; }
        now = hp_now_ns();
        if (now >= end) {
            auxiliary_fork_gate = false; pthread_cond_broadcast(&slots_cond);
            pthread_mutex_unlock(&slots_mutex); return -1;
        }
        uint64_t remaining = end - now;
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += (time_t)(remaining / 1000000000);
        until.tv_nsec += (long)(remaining % 1000000000);
        if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&slots_cond, &slots_mutex, &until);
    }
}
void hp_sapi_fork_parent(void)
{
    pthread_mutex_lock(&slots_mutex); auxiliary_fork_gate = false;
    pthread_cond_broadcast(&slots_cond); pthread_mutex_unlock(&slots_mutex);
}
void hp_sapi_fork_child(void)
{
    /* No other pthread survives fork. Never acquire inherited host locks. */
    slots_mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    slots_cond = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
    slots = local_slot.registered ? &local_slot : NULL; local_slot.next = NULL;
    auxiliary_fork_gate = false; pending_spawns = 0;
    atomic_store(&auxiliary_threads, 0);
}

unsigned hp_sapi_aux_threads(void) { return atomic_load_explicit(&auxiliary_threads, memory_order_acquire); }
void *hp_sapi_worker_token(void) { return local_owner ? local_owner->token : local_slot.worker_token; }
int64_t hp_sapi_memory_limit(void) { return PG(memory_limit); }

static bool is_real_request(void) { return current_request && current_request != &dummy_request; }

/* HTTP/1.1 is the only protocol that can carry a final, header-only 101 here:
 * HTTP/1.0 clients must not receive 1xx, and HTTP/2 and HTTP/3 prohibit 101.
 * Uses only the request's copied protocol metadata; PHP threads never touch
 * the H2O request or connection. */
static bool http11_request(void)
{
    return is_real_request() && current_request->protocol && !strcmp(current_request->protocol, "HTTP/1.1");
}

static void log_message(const char *message, int severity)
{
    if (hp_process_is_fork_child()) {
        (void)write(STDERR_FILENO, message, strlen(message)); (void)write(STDERR_FILENO, "\n", 1); return;
    }
    hp_log_message("php.engine", message, severity, is_real_request() ? current_request->id : 0);
}

static int publish_headers(sapi_headers_struct *headers)
{
    if (hp_process_is_fork_child() || !is_real_request() || SG(request_info).no_headers || http_finished)
        return SAPI_HEADER_SENT_SUCCESSFULLY;
    hp_header_t *list = NULL;
    zend_llist_position position;
    sapi_header_struct *entry = zend_llist_get_first_ex(&headers->headers, &position);
    for (; entry; entry = zend_llist_get_next_ex(&headers->headers, &position)) {
        char *colon = memchr(entry->header, ':', entry->header_len);
        if (!colon) continue;
        size_t name_len = (size_t)(colon - entry->header);
        while (name_len && (entry->header[name_len - 1] == ' ' || entry->header[name_len - 1] == '\t')) --name_len;
        if (!name_len) continue;
        char *name = strndup(entry->header, name_len);
        const char *value = colon + 1;
        const char *end = entry->header + entry->header_len;
        while (value < end && (*value == ' ' || *value == '\t')) ++value;
        char *copy = strndup(value, (size_t)(end - value));
        if (!name || !copy || hp_header_add(&list, name, copy)) {
            free(name); free(copy); hp_headers_free(list);
            return SAPI_HEADER_SEND_FAILED;
        }
        free(name); free(copy);
    }
    int status = headers->http_response_code ? headers->http_response_code : 200;
    bool final_101 = status == 101 && http11_request();
    if (status == 101 && !final_101) {
        /* A PHP request has no native socket-upgrade bridge. In particular,
         * H2O's informational API excludes 101 and asserts if given it, and
         * HTTP/1.0, HTTP/2 and HTTP/3 cannot carry it either. */
        log_message("PHP protocol upgrades are not supported; sending HTTP 500", 4);
        status = headers->http_response_code = 500;
    }
    if (status < 100 || status > 999) {
        log_message("Invalid response status code; sending HTTP 500", 4);
        status = 500;
    }
    /* 101 is never informational: on HTTP/1.1 it is committed as the final,
     * bodyless header set; the bridge rejects it on every other protocol. */
    bool info = status >= 100 && status < 200 && status != 101;
    int result;
    if (info) {
        result = hp_response_headers(current_request, status, list, true);
        hp_headers_free(list);
    } else {
        /* Native success accepts an owned immutable system-buffer snapshot.
         * Explicit progress boundaries and conservative bodyless/reproxy cases
         * publish immediately; only an actual later ub_write may compound. */
        bool defer = !sapi_headers_finishing && status != 101 && status != 204 && status != 304 &&
            !SG(request_info).headers_only &&
            !(current_request->method && !strcmp(current_request->method, "HEAD")) &&
            !hp_header_get(list, "x-reproxy-url");
        result = hp_sapi_response_headers(current_request, status, list, defer);
    }
    if (!result && !info) sapi_final_headers_sent = true;
    return result ? SAPI_HEADER_SEND_FAILED : SAPI_HEADER_SENT_SUCCESSFULLY;
}

static int send_headers(sapi_headers_struct *headers)
{
    /* PHP may reenter its native callback after releasing the header callable:
     * a callable destructor can flush the same final headers before this call. */
    if (sapi_final_headers_sent) return SAPI_HEADER_SENT_SUCCESSFULLY;
    return publish_headers(headers);
}

static size_t write_output(const char *data, size_t length)
{
    if (hp_process_is_fork_child() || !is_real_request() || http_finished) return length;
    if (local_owner && pthread_mutex_trylock(&local_owner->output_mutex)) {
        /* A competing producer may be blocked on capacity. Its progress must
         * not depend on this thread first acquiring producer serialization. */
        (void)hp_sapi_response_publish(current_request);
        pthread_mutex_lock(&local_owner->output_mutex);
    }
    ssize_t written = hp_sapi_response_write(current_request, data, length);
    if (local_owner) pthread_mutex_unlock(&local_owner->output_mutex);
    if (written < 0) {
        php_handle_aborted_connection();
        return 0;
    }
    return (size_t)written;
}

static void flush_output(void *server_context)
{
    (void)server_context;
    if (hp_process_is_fork_child() || !is_real_request()) return;
    if (!http_finished) sapi_send_headers();
    /* Core short-circuits when SG(headers_sent) is already true. The native
     * system buffer still has to publish on every explicit flush boundary. */
    (void)hp_sapi_response_publish(current_request);
    hp_response_flush_notify(current_request);
}

static size_t read_post(char *buffer, size_t count)
{
    if (hp_process_is_fork_child() || !is_real_request() || http_finished) return 0;
    size_t total = 0;
    /* PHP treats any short SAPI read as EOF. Assemble until its desired size
     * or a real EOF, while the bridge applies upload backpressure. */
    while (total < count) {
        ssize_t got = hp_request_body_read(current_request, buffer + total, count - total);
        if (got <= 0) break;
        total += (size_t)got;
    }
    return total;
}

/* Keep PHP's public default reader and its 16KiB logical reads. When a full
 * contiguous native block already exists, pass that immutable block directly
 * to the same PHP temp stream instead of copying through a stack buffer.
 * Registered POST readers and replaced SAPI read hooks retain PHP's path. */
static void read_default_post(void)
{
    if (hp_process_is_fork_child() || !is_real_request() || http_finished ||
        sapi_module.read_post != read_post ||
        strcmp(SG(request_info).request_method, "POST") || SG(request_info).post_entry) {
        php_default_post_reader();
        return;
    }
    zend_long post_max_size = REQUEST_PARSE_BODY_OPTION_GET(post_max_size, SG(post_max_size));
    if (post_max_size > 0 && SG(request_info).content_length > post_max_size) {
        php_error_docref(NULL, E_WARNING, "POST Content-Length of " ZEND_LONG_FMT " bytes exceeds the limit of " ZEND_LONG_FMT " bytes",
                        SG(request_info).content_length, post_max_size);
        return;
    }
    SG(request_info).request_body = php_stream_temp_create_ex(TEMP_STREAM_DEFAULT, SAPI_POST_BLOCK_SIZE, PG(upload_tmp_dir));
    for (;;) {
        char buffer[SAPI_POST_BLOCK_SIZE];
        hp_sapi_body_view_t view = {0};
        size_t read_bytes;
        ssize_t written = 0;
        if (hp_sapi_request_body_borrow(current_request, SAPI_POST_BLOCK_SIZE, &view)) {
            read_bytes = view.len;
            SG(read_post_bytes) += read_bytes;
            if (read_bytes < SAPI_POST_BLOCK_SIZE) SG(post_read) = true;
            /* PHP stream writes can bail out (including temporary-file error
             * handlers). The entire logical read has happened in either case. */
            zend_try {
                written = php_stream_write(SG(request_info).request_body, view.data, read_bytes);
            } zend_catch {
                hp_sapi_request_body_release(&view);
                zend_bailout();
            } zend_end_try();
            hp_sapi_request_body_release(&view);
        } else {
            read_bytes = sapi_read_post_block(buffer, SAPI_POST_BLOCK_SIZE);
            if (read_bytes > 0)
                written = php_stream_write(SG(request_info).request_body, buffer, read_bytes);
        }
        if (read_bytes > 0 && written != (ssize_t)read_bytes) {
            php_stream_truncate_set_size(SG(request_info).request_body, 0);
            php_error_docref(NULL, E_WARNING, "POST data can't be buffered; all data discarded");
            break;
        }
        if (post_max_size > 0 && SG(read_post_bytes) > post_max_size) {
            php_error_docref(NULL, E_WARNING, "Actual POST length does not match Content-Length, and exceeds " ZEND_LONG_FMT " bytes", post_max_size);
            break;
        }
        if (read_bytes < SAPI_POST_BLOCK_SIZE) break;
    }
    php_stream_rewind(SG(request_info).request_body);
    if (SG(request_info).request_body != NULL) {
        /* The temp stream forwards this public option to its inner stream.
         * Replay remains valid if the optional buffering hint is rejected. */
        (void)php_stream_set_option(SG(request_info).request_body, PHP_STREAM_OPTION_READ_BUFFER,
                                    PHP_STREAM_BUFFER_NONE, NULL);
    }
}

static char *read_cookies(void) { return current_request ? current_request->cookie : NULL; }

static void regvar(zval *target, const char *name, const char *value)
{
    size_t length = value ? strlen(value) : 0;
    if (!filter_server_variables || !value || !sapi_module.input_filter) {
        php_register_variable_safe(name, value ? value : "", length, target);
        return;
    }
    /* An input filter may replace or free its mutable PHP-heap input. Never
     * hand it request-owned or immutable environment bytes. PARSE_SERVER can
     * register raw/filtered values itself and return false; do not overwrite
     * that result with the unfiltered original. Bailouts stay inside C. */
    char *filtered = estrndup(value, length);
    size_t filtered_length = length;
    if (sapi_module.input_filter(PARSE_SERVER, name, &filtered, length, &filtered_length))
        php_register_variable_safe(name, filtered, filtered_length, target);
    if (filtered) efree(filtered);
}

/* POSIX sanitizedPathJoin metadata: root is already a cleaned absolute
 * configured path. Clean the decoded suffix without decoding it again, clamp
 * .. at root, and preserve upstream's final-slash rule. No filesystem access. */
static zend_string *cgi_path_translated(const char *root, const char *path_info)
{
    if (!root || !*root) root = ".";
    size_t root_len = strlen(root), info_len = strlen(path_info);
    while (root_len > 1 && root[root_len - 1] == '/') --root_len;
    if (root_len == 1 && root[0] == '.') root_len = 0;
    zend_string *result = zend_string_alloc(root_len + info_len + 2, 0);
    char *dst = ZSTR_VAL(result);
    memcpy(dst, root, root_len);
    size_t length = root_len;
    for (const char *at = path_info; *at;) {
        while (*at == '/') ++at;
        if (!*at) break;
        const char *part = at;
        while (*at && *at != '/') ++at;
        size_t part_len = (size_t)(at - part);
        if (part_len == 1 && part[0] == '.') continue;
        if (part_len == 2 && part[0] == '.' && part[1] == '.') {
            if (length > root_len) {
                while (length > root_len && dst[length - 1] != '/') --length;
                if (length > root_len) --length;
            }
            continue;
        }
        if (length && dst[length - 1] != '/') dst[length++] = '/';
        memcpy(dst + length, part, part_len); length += part_len;
    }
    if (!length) dst[length++] = '.';
    if (info_len > 1 && path_info[info_len - 1] == '/') dst[length++] = '/';
    dst[length] = '\0'; ZSTR_LEN(result) = length;
    return result;
}

static void register_variables(zval *target)
{
    hp_request_t *r = current_request;
    hp_env_server_import(target);
    if (!r) return;
    regvar(target, "SERVER_SOFTWARE", "H2O PHP/FrankenPHP-compatible");
    regvar(target, "GATEWAY_INTERFACE", "CGI/1.1");
    regvar(target, "SERVER_PROTOCOL", r->protocol ? r->protocol : "HTTP/1.1");
    regvar(target, "SERVER_NAME", r->server_name);
    regvar(target, "SERVER_PORT", r->server_port);
    regvar(target, "REMOTE_ADDR", r->remote_addr);
    regvar(target, "REMOTE_HOST", r->remote_addr);
    regvar(target, "REMOTE_PORT", r->remote_port);
    regvar(target, "DOCUMENT_ROOT", r->document_root);
    regvar(target, "SCRIPT_FILENAME", r->script);
    regvar(target, "SCRIPT_NAME", r->script_name);
    regvar(target, "PATH_INFO", r->path_info);
    const char *script_name = r->script_name;
    if (r->path_info && *r->path_info) {
        /* Pinned upstream CGI metadata uses SCRIPT_NAME + PATH_INFO.
         * Explicit-worker PATH_INFO retains decoded URL dot segments;
         * PATH_TRANSLATED cleans them lexically without filesystem access. */
        zend_string *self = strpprintf(0, "%s%s", script_name ? script_name : "", r->path_info);
        regvar(target, "PHP_SELF", ZSTR_VAL(self));
        zend_string_release(self);
        zend_string *translated = cgi_path_translated(r->document_root, r->path_info);
        regvar(target, "PATH_TRANSLATED", ZSTR_VAL(translated));
        zend_string_release(translated);
    } else regvar(target, "PHP_SELF", script_name);
    regvar(target, "REQUEST_URI", r->uri);
    regvar(target, "DOCUMENT_URI", r->document_uri_set ? r->document_uri : r->script_name);
    /* Parallel only inherits SG(server_context), not the parent's request
     * info. Its method is absent and its query is empty at fresh RINIT. */
    if (SG(request_info).request_method) regvar(target, "REQUEST_METHOD", SG(request_info).request_method);
    regvar(target, "QUERY_STRING", SG(request_info).query_string);
    /* Trusted NULL values must exist in SERVER without entering the raw
     * INPUT_SERVER filter table, matching upstream request-info registration. */
    const char *content_type = SG(request_info).content_type;
    regvar(target, "CONTENT_TYPE", content_type && *content_type ? content_type : NULL);
    regvar(target, "REQUEST_SCHEME", r->tls ? "https" : "http");
    if (r->tls) regvar(target, "HTTPS", "on");
    /* CGI uses the header value, not SG's numeric body-framing length.
     * A dummy GET has numeric length zero but no Content-Length header. */
    regvar(target, "CONTENT_LENGTH", hp_header_get(r->headers, "content-length"));
    if (SG(request_info).auth_user) {
        regvar(target, "AUTH_TYPE", "Basic");
        regvar(target, "PHP_AUTH_USER", SG(request_info).auth_user);
        regvar(target, "PHP_AUTH_PW", SG(request_info).auth_password);
        regvar(target, "REMOTE_USER", SG(request_info).auth_user);
    }
    /* Join duplicates by their original case-insensitive field name before
     * PHP normalizes names. Distinct fields such as X-A and X_A must not be
     * concatenated merely because both map to HTTP_X_A. */
    HashTable header_values;
    zend_hash_init(&header_values, 8, NULL, ZVAL_PTR_DTOR, 0);
    for (hp_header_t *h = r->headers; h; h = h->next) {
        zend_string *original = zend_string_init(h->name, strlen(h->name), 0);
        zend_string *key = zend_string_tolower(original);
        zend_string_release(original);
        zval joined;
        zval *existing = zend_hash_find(&header_values, key);
        if (existing) ZVAL_STR(&joined, strpprintf(0, "%s, %s", Z_STRVAL_P(existing), h->value));
        else ZVAL_STRING(&joined, h->value);
        zend_hash_update(&header_values, key, &joined);
        zend_string_release(key);
    }
    zend_string *field;
    zval *value;
    ZEND_HASH_FOREACH_STR_KEY_VAL(&header_values, field, value) {
        size_t len = ZSTR_LEN(field);
        char *name = emalloc(len + 6);
        memcpy(name, "HTTP_", 5);
        for (size_t i = 0; i < len; ++i) {
            unsigned char c = (unsigned char)ZSTR_VAL(field)[i];
            name[i + 5] = c == '-' ? '_' : (c >= 'a' && c <= 'z' ? c - 32 : c);
        }
        name[len + 5] = 0;
        regvar(target, name, Z_STRVAL_P(value));
        efree(name);
    } ZEND_HASH_FOREACH_END();
    zend_hash_destroy(&header_values);
    /* Preserve existing copy-last precedence, but overlays are raw hash
     * entries in the reference, not CGI input. Normalizing here creates an
     * extra HP_ODD alias for HP.ODD; filtering here would alter configured
     * environment values and incorrectly populate INPUT_SERVER. */
    if (!local_auxiliary) for (hp_header_t *h = r->env; h; h = h->next) {
        zval value;
        ZVAL_STRING(&value, h->value ? h->value : "");
        zend_hash_str_update(Z_ARRVAL_P(target), h->name, strlen(h->name), &value);
    }
}

static zend_result request_time(double *time)
{
    if (!current_request) return FAILURE;
    *time = current_request->request_time;
    return SUCCESS;
}

static void install_context(hp_request_t *request)
{
    if (current_request && current_request != &dummy_request && current_request != request)
        (void)hp_sapi_response_publish(current_request);
    current_request = request;
    publish_request(request);
    http_finished = false;
    sapi_final_headers_sent = false;
    sapi_headers_finishing = false;
    SG(server_context) = local_owner;
    SG(sapi_headers).http_response_code = 200;
    SG(request_info).request_method = request->method ? request->method : "GET";
    SG(request_info).query_string = request->query;
    SG(request_info).cookie_data = request->cookie;
    SG(request_info).content_length = request->content_length == SIZE_MAX ? -1 :
        (request->content_length > LONG_MAX ? LONG_MAX : (zend_long)request->content_length);
    SG(request_info).path_translated = request->script;
    SG(request_info).request_uri = request->uri;
    SG(request_info).content_type = request->content_type;
    SG(request_info).headers_only = request->method && !strcmp(request->method, "HEAD");
    SG(request_info).no_headers = false;
    SG(global_request_time) = request->request_time;
    PG(ignore_user_abort) = current_group ? true : default_ignore_user_abort;
    for (hp_header_t *h = request->env; h; h = h->next) hp_env_set(h->name, h->value);
    if (current_group) hp_env_set("FRANKENPHP_WORKER", "1");
    const char *authorization = hp_header_get(request->headers, "authorization");
    php_handle_auth_data(authorization);
}

static void clear_context(void)
{
    if (!hp_process_is_fork_child() && is_real_request())
        (void)hp_sapi_response_publish(current_request);
    publish_request(NULL);
    SG(request_info).request_method = NULL;
    SG(request_info).query_string = NULL;
    SG(request_info).cookie_data = NULL;
    SG(request_info).path_translated = NULL;
    SG(request_info).request_uri = NULL;
    SG(request_info).content_type = NULL;
}

static void finish_http(void)
{
    if (hp_process_is_fork_child() || !is_real_request() || http_finished) return;
    sapi_send_headers();
    hp_response_finish(current_request);
    http_finished = true;
}

static void frankenphp_reset_super_globals() {
  zend_try {
    /* only $_FILES needs to be flushed explicitly
     * $_GET, $_POST, $_COOKIE and $_SERVER are flushed on reimport
     * $_ENV is not flushed
     * for more info see: php_startup_auto_globals()
     */
    zval *files = &PG(http_globals)[TRACK_VARS_FILES];
    zval_ptr_dtor_nogc(files);
    memset(files, 0, sizeof(*files));

    /* $_SESSION must be explicitly deleted from the symbol table.
     * Unlike other superglobals, $_SESSION is stored in EG(symbol_table)
     * with a reference to PS(http_session_vars). The session RSHUTDOWN
     * only decrements the refcount but doesn't remove it from the symbol
     * table, causing data to leak between requests. */
    zend_hash_str_del(&EG(symbol_table), "_SESSION", sizeof("_SESSION") - 1);
  }
  zend_end_try();

  zend_auto_global *auto_global;
  zend_string *_env = ZSTR_KNOWN(ZEND_STR_AUTOGLOBAL_ENV);
  zend_string *_server = ZSTR_KNOWN(ZEND_STR_AUTOGLOBAL_SERVER);
  ZEND_HASH_MAP_FOREACH_PTR(CG(auto_globals), auto_global) {
    if (auto_global->name == _env) {
      /* skip $_ENV */
    } else if (auto_global->name == _server) {
      /* always reimport $_SERVER */
      auto_global->armed = auto_global->auto_global_callback(auto_global->name);
    } else if (auto_global->jit) {
      /* JIT globals ($_REQUEST, $GLOBALS) need special handling:
       * - $GLOBALS will always be handled by the application, we skip it
       * For $_REQUEST:
       * - If in symbol_table: re-initialize with current request data
       * - If not: do nothing, it may be armed by jit later */
      if (auto_global->name == ZSTR_KNOWN(ZEND_STR_AUTOGLOBAL_REQUEST) &&
          zend_hash_exists(&EG(symbol_table), auto_global->name)) {
        auto_global->armed =
            auto_global->auto_global_callback(auto_global->name);
      }
    } else if (auto_global->auto_global_callback) {
      /* $_GET, $_POST, $_COOKIE, $_FILES are reimported here */
      auto_global->armed = auto_global->auto_global_callback(auto_global->name);
    } else {
      /* $_SESSION will land here (not an http_global) */
      auto_global->armed = 0;
    }
  }
  ZEND_HASH_FOREACH_END();
}

/* Mirrors php_stream_input_t from ext/standard/php_fopen_wrapper.c */
typedef struct {
  php_stream *body;
  zend_off_t position;
} frankenphp_stream_input;

/* Matches the body by address: userland may already have closed and freed it.
 * A php://input stream kept by the application reads it through a raw
 * pointer, the body then lives as long as the worker script. */
static void frankenphp_release_request_body(php_stream *request_body) {
  zend_resource *body_res = NULL;
  zend_resource *res;
  ZEND_HASH_FOREACH_PTR(&EG(regular_list), res) {
    if (res->type != php_file_le_stream()) {
      continue;
    }
    php_stream *stream = (php_stream *)res->ptr;
    if (stream == request_body) {
      body_res = res;
    } else if (stream->wrapper == &php_stream_php_wrapper &&
               strcmp(stream->ops->label, "Input") == 0 &&
               ((frankenphp_stream_input *)stream->abstract)->body ==
                   request_body) {
      return;
    }
  }
  ZEND_HASH_FOREACH_END();

  if (body_res != NULL) {
    zend_list_delete(body_res);
  }
}

/* The installed session extension may be shared. Access its registered ZTS
 * globals through the public module registry rather than linking ps_globals_id.
 * This preserves user save handlers just as upstream's built-in session path. */
static void reset_session_state(void)
{
    zend_module_entry *module = zend_hash_str_find_ptr(&module_registry, "session", sizeof("session") - 1);
    if (!module || !module->globals_id_ptr || module->globals_size != sizeof(php_ps_globals)) return;
    php_ps_globals *ps = ts_resource_ex(*module->globals_id_ptr, NULL);
    if (ps->session_status == php_session_active) {
        typedef zend_result (*flush_fn)(int);
        flush_fn flush = (flush_fn)dlsym(module->handle ? module->handle : RTLD_DEFAULT, "php_session_flush");
        if (flush) { zend_try { flush(1); } zend_end_try(); }
    }
    if (!Z_ISUNDEF(ps->http_session_vars)) {
        zval_ptr_dtor(&ps->http_session_vars);
        ZVAL_UNDEF(&ps->http_session_vars);
    }
    if ((ps->mod_data || ps->mod_user_implemented) && ps->mod) {
        zend_try { ps->mod->s_close(&ps->mod_data); } zend_end_try();
    }
    if (ps->id) { zend_string_release_ex(ps->id, 0); ps->id = NULL; }
    if (ps->session_vars) { zend_string_release_ex(ps->session_vars, 0); ps->session_vars = NULL; }
    if (ps->session_started_filename) {
        zend_string_release(ps->session_started_filename);
        ps->session_started_filename = NULL;
        ps->session_started_lineno = 0;
    }
    ps->session_status = php_session_none;
    ps->in_save_handler = ps->set_handler = ps->mod_user_is_open = false;
    ps->mod_data = NULL;
    ps->define_sid = true;
}

static void reset_url_scanner(url_adapt_state_ex_t *ctx)
{
    smart_str_free(&ctx->result); smart_str_free(&ctx->buf);
    smart_str_free(&ctx->tag); smart_str_free(&ctx->arg);
    smart_str_free(&ctx->attr_val); smart_str_free(&ctx->form_app);
    smart_str_free(&ctx->url_app);
    ctx->active = ctx->tag_type = ctx->attr_type = 0;
}

static void reload_module(bool startup)
{
    zend_module_entry *module = zend_hash_str_find_ptr(&module_registry, "filter", sizeof("filter") - 1);
    if (!module) return;
    if (startup && module->request_startup_func) module->request_startup_func(module->type, module->module_number);
    if (!startup && module->request_shutdown_func) module->request_shutdown_func(module->type, module->module_number);
}

/* Partial lifecycle: do not deactivate Zend, destruct persistent userland
 * objects, run shutdown functions, or free the worker's request heap. */
static void worker_request_shutdown(void)
{
    zend_try { php_output_end_all(); } zend_end_try();
    reload_module(false);
    reset_session_state();
    zend_try { php_output_deactivate(); } zend_end_try();
    finish_http();
    reset_url_scanner(&BG(url_adapt_session_ex));
    reset_url_scanner(&BG(url_adapt_output_ex));
    php_stream *body = SG(request_info).request_body;
    zend_try { sapi_deactivate(); } zend_end_try();
    SG(request_info).content_type_dup = NULL;
    SG(request_info).current_user = NULL;
#if PHP_VERSION_ID < 80600
    SG(request_info).current_user_length = 0;
#endif
    if (body) frankenphp_release_request_body(body);
    clear_context();
    hp_env_request_clear();
    zend_set_memory_limit(PG(memory_limit));
    partial_active = false;
}

static int worker_request_startup(hp_request_t *request)
{
    int result = SUCCESS;
    zval error_handler;
    ZVAL_COPY_VALUE(&error_handler, &EG(user_error_handler));
    ZVAL_UNDEF(&EG(user_error_handler));
    install_context(request);
    zend_try {
        php_output_activate();
        PG(header_is_being_sent) = false;
        PG(connection_status) = PHP_CONNECTION_NORMAL;
        sapi_activate();
#ifdef ZEND_MAX_EXECUTION_TIMERS
        zend_set_timeout(PG(max_input_time) == -1 ? EG(timeout_seconds) : PG(max_input_time), 1);
#endif
        if (PG(expose_php)) sapi_add_header(SAPI_PHP_VERSION_HEADER, sizeof(SAPI_PHP_VERSION_HEADER) - 1, 1);
#if PHP_VERSION_ID >= 80600
        const char *output_handler = PG(output_handler) ? ZSTR_VAL(PG(output_handler)) : NULL;
#else
        const char *output_handler = PG(output_handler);
#endif
        if (output_handler && output_handler[0]) {
            zval handler;
            ZVAL_STRING(&handler, output_handler);
            php_output_start_user(&handler, 0, PHP_OUTPUT_HANDLER_STDFLAGS);
            zval_ptr_dtor(&handler);
        } else if (PG(output_buffering)) {
            php_output_start_user(NULL, PG(output_buffering) > 1 ? PG(output_buffering) : 0, PHP_OUTPUT_HANDLER_STDFLAGS);
        } else if (PG(implicit_flush)) php_output_set_implicit_flush(1);
        frankenphp_reset_super_globals();
        reload_module(true);
    } zend_catch { result = FAILURE; } zend_end_try();
    ZVAL_COPY_VALUE(&EG(user_error_handler), &error_handler);
    SG(sapi_started) = 1;
    partial_active = true;
    return result;
}

PHP_FUNCTION(frankenphp_handle_request)
{
    zend_fcall_info fci;
    zend_fcall_info_cache fcc;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_FUNC(fci, fcc)
    ZEND_PARSE_PARAMETERS_END();
    if (hp_process_is_fork_child()) RETURN_FALSE;
    if (!current_group) {
        zend_throw_exception(spl_ce_RuntimeException, "frankenphp_handle_request() called while not in worker mode", 0);
        RETURN_THROWS();
    }
#ifdef ZEND_MAX_EXECUTION_TIMERS
    zend_unset_timeout();
#endif
    if (!worker_bootstrapped) {
        hp_env_bootstrapped();
        worker_request_shutdown();
        worker_bootstrapped = true;
        current_request = NULL;
        hp_worker_ready(current_group);
    }
    hp_request_t *request = hp_worker_should_restart(current_group) ? NULL : hp_worker_next(current_group);
    if (worker_request_startup(request ? request : &dummy_request) == FAILURE) zend_bailout();
    if (!request) RETURN_FALSE;
#ifdef ZEND_MAX_EXECUTION_TIMERS
    if (PG(max_input_time) != -1)
        zend_set_timeout(zend_ini_long("max_execution_time", sizeof("max_execution_time") - 1, 0), 0);
#endif
    zval retval;
    ZVAL_UNDEF(&retval);
    fci.size = sizeof(fci);
    fci.retval = &retval;
    fci.params = NULL;
    fci.param_count = 0;
    zend_call_function(&fci, &fcc);
    if (EG(exception)) {
        if (zend_is_unwind_exit(EG(exception)) || zend_is_graceful_exit(EG(exception))) zend_bailout();
        zend_exception_error(EG(exception), E_ERROR);
    }
    worker_request_shutdown();
    finish_http();
    hp_runtime_request_done(request);
    current_request = NULL;
    ++worker_requests;
    if (!Z_ISUNDEF(retval)) zval_ptr_dtor(&retval);
    RETURN_TRUE;
}

PHP_FUNCTION(frankenphp_finish_request)
{
    ZEND_PARSE_PARAMETERS_NONE();
    if (hp_process_is_fork_child() || http_finished || !is_real_request()) RETURN_FALSE;
    php_output_end_all();
    finish_http();
    RETURN_TRUE;
}

PHP_FUNCTION(headers_send)
{
    zend_long status = 200;
    ZEND_PARSE_PARAMETERS_START(0, 1)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(status)
    ZEND_PARSE_PARAMETERS_END();
    /* On a real HTTP/1.1 request, 101 is sent as a final bodyless header set and
     * the previous status is restored below (return 1, headers_sent() stays
     * false). On HTTP/1.0, HTTP/2 or HTTP/3 the upgrade stays unsupported; in
     * CLI/bootstrap there is no socket, so the call is a no-op like any other. */
    if (status == 101 && is_real_request() && !http11_request()) {
        zend_argument_value_error(1, "must not be 101 because PHP protocol upgrades are not supported");
        RETURN_THROWS();
    }
    int previous = SG(sapi_headers).http_response_code;
    SG(sapi_headers).http_response_code = (int)status;
    if (status >= 100 && status < 200) {
        int result = publish_headers(&SG(sapi_headers));
        SG(sapi_headers).http_response_code = previous;
        RETURN_LONG(result);
    }
    int result = sapi_send_headers();
    /* headers_send(final) is an explicit publication boundary even if core
     * previously accepted the snapshot and now skips its native callback. */
    if (!hp_process_is_fork_child() && is_real_request() &&
        hp_sapi_response_publish(current_request)) result = FAILURE;
    RETURN_LONG(result);
}

PHP_FUNCTION(frankenphp_request_headers)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    if (current_request) for (hp_header_t *h = current_request->headers; h; h = h->next) {
        size_t len = strlen(h->name);
        /* net/http keeps Host outside request.Header. Preserve the internal
         * Host entry for CGI metadata; omit it only from this public array. */
        if (len == 4 && !zend_binary_strcasecmp(h->name, len, "host", 4)) continue;
        zend_string *name = zend_string_init(h->name, len, 0);
        bool upper = true;
        for (size_t i = 0; i < len; ++i) {
            unsigned char c = (unsigned char)ZSTR_VAL(name)[i];
            if (c >= 'a' && c <= 'z' && upper) c -= 32;
            else if (c >= 'A' && c <= 'Z' && !upper) c += 32;
            ZSTR_VAL(name)[i] = (char)c;
            upper = c == '-';
        }
        zval val;
        zval *existing = zend_hash_find(Z_ARRVAL_P(return_value), name);
        if (existing && Z_TYPE_P(existing) == IS_STRING)
            ZVAL_STR(&val, strpprintf(0, "%s, %s", Z_STRVAL_P(existing), h->value));
        else ZVAL_STRING(&val, h->value);
        zend_hash_update(Z_ARRVAL_P(return_value), name, &val);
        zend_string_release(name);
    }
}

PHP_FUNCTION(frankenphp_response_headers)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    zend_llist_position pos;
    sapi_header_struct *h = zend_llist_get_first_ex(&SG(sapi_headers).headers, &pos);
    for (; h; h = zend_llist_get_next_ex(&SG(sapi_headers).headers, &pos)) {
        char *colon = memchr(h->header, ':', h->header_len);
        if (!colon) continue;
        size_t len = (size_t)(colon - h->header);
        while (len && (h->header[len - 1] == ' ' || h->header[len - 1] == '\t')) --len;
        const char *value = colon + 1;
        const char *end = h->header + h->header_len;
        while (value < end && (*value == ' ' || *value == '\t')) ++value;
        if (len) add_assoc_stringl_ex(return_value, h->header, len, value, (size_t)(end - value));
    }
}

static bool valid_log_context(zval *value, unsigned depth)
{
    ZVAL_DEREF(value);
    switch (Z_TYPE_P(value)) {
    case IS_NULL: case IS_FALSE: case IS_TRUE: case IS_LONG: case IS_DOUBLE: case IS_STRING:
        return true;
    case IS_ARRAY: {
        HashTable *array = Z_ARRVAL_P(value);
        if (GC_IS_RECURSIVE(array) || depth > 512) {
            zend_throw_exception(spl_ce_RuntimeException, "Failed to log message: converting attrs: cannot convert a recursive array", 0);
            return false;
        }
        GC_TRY_PROTECT_RECURSION(array);
        zval *element;
        bool valid = true;
        ZEND_HASH_FOREACH_VAL(array, element) {
            if (!valid_log_context(element, depth + 1)) { valid = false; break; }
        } ZEND_HASH_FOREACH_END();
        GC_TRY_UNPROTECT_RECURSION(array);
        return valid;
    }
    default:
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "Failed to log message: converting attrs: unsupported zval type %d", Z_TYPE_P(value));
        return false;
    }
}

PHP_FUNCTION(frankenphp_log)
{
    zend_string *message;
    zend_long level = 0;
    zval *context = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_STR(message)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(level)
        Z_PARAM_ARRAY(context)
    ZEND_PARSE_PARAMETERS_END();
    if (hp_process_is_fork_child()) {
        (void)write(STDERR_FILENO, ZSTR_VAL(message), ZSTR_LEN(message));
        (void)write(STDERR_FILENO, "\n", 1); return;
    }
    if (context && !valid_log_context(context, 0)) RETURN_THROWS();
    zval record, msg;
    array_init(&record);
    add_assoc_string(&record, "component", "php.application");
    ZVAL_STR_COPY(&msg, message);
    add_assoc_zval(&record, "message", &msg);
    add_assoc_long(&record, "level", level);
    add_assoc_long(&record, "request_id", is_real_request() ? (zend_long)current_request->id : 0);
    if (context) { Z_TRY_ADDREF_P(context); add_assoc_zval(&record, "context", context); }
    else { zval empty; array_init(&empty); add_assoc_zval(&record, "context", &empty); }
    smart_str json = {0};
    int result = php_json_encode(&json, &record, PHP_JSON_UNESCAPED_SLASHES | PHP_JSON_INVALID_UTF8_SUBSTITUTE);
    zval_ptr_dtor(&record);
    if (result == FAILURE) {
        smart_str_free(&json);
        zend_throw_exception(spl_ce_RuntimeException, "Failed to log message: converting attrs to JSON", 0);
        RETURN_THROWS();
    }
    smart_str_0(&json);
    hp_log_json(json.s ? ZSTR_VAL(json.s) : "", json.s ? ZSTR_LEN(json.s) : 0);
    smart_str_free(&json);
}

/* Reboot long-lived workers after a safe shared OPcache reset. The original
 * extension handler acquires OPcache's own shared-memory restart lock. */
static zif_handler original_opcache_reset;
PHP_FUNCTION(hp_opcache_reset)
{
    if (original_opcache_reset) original_opcache_reset(INTERNAL_FUNCTION_PARAM_PASSTHRU);
    else RETVAL_FALSE;
    if (Z_TYPE_P(return_value) == IS_TRUE) hp_runtime_restart_workers();
}

static void override_opcache(void)
{
    zend_function *fn = zend_hash_str_find_ptr(CG(function_table), "opcache_reset", sizeof("opcache_reset") - 1);
    if (fn && fn->type == ZEND_INTERNAL_FUNCTION && fn->internal_function.handler != ZEND_FN(hp_opcache_reset)) {
        original_opcache_reset = fn->internal_function.handler;
        fn->internal_function.handler = ZEND_FN(hp_opcache_reset);
    }
}

static PHP_MINIT_FUNCTION(frankenphp)
{
    register_frankenphp_symbols(module_number);
    if (hp_pcntl_install() != 0) return FAILURE;
    register_parallel_hooks();
    hp_env_override_functions();
    override_opcache();
    return SUCCESS;
}

static PHP_RSHUTDOWN_FUNCTION(frankenphp)
{
    /* PHP drains normal output before module RSHUTDOWN. Publish its accepted
     * snapshot now; later header-only sends must also precede SAPI teardown. */
    sapi_headers_finishing = true;
    if (!hp_process_is_fork_child() && is_real_request())
        (void)hp_sapi_response_publish(current_request);
    return SUCCESS;
}

static PHP_MINFO_FUNCTION(frankenphp)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "FrankenPHP compatibility", "native H2O PHP runtime");
    php_info_print_table_row(2, "Reference revision", "4384c506d219cd2f8750edfceae57d3db8eb54f3");
    php_info_print_table_row(2, "Thread safety", "ZTS / dedicated pthreads");
    php_info_print_table_end();
}

static zend_module_entry frankenphp_module = {
    STANDARD_MODULE_HEADER,
    "frankenphp", ext_functions,
    PHP_MINIT(frankenphp), NULL, NULL, PHP_RSHUTDOWN(frankenphp),
    PHP_MINFO(frankenphp), "h2o-native-0.1",
    STANDARD_MODULE_PROPERTIES
};

/* PHP 8.5's two OPcache sizing update handlers reject refresh after SHM
 * startup, including zend_new_thread_end_handler's initialization of a new
 * ZTS thread. Preserve the controller's validated scalar caches using Zend's
 * public INI entry address metadata; no private accelerator layout is used. */
static const char *opcache_scalar_names[] = {"opcache.memory_consumption", "opcache.interned_strings_buffer"};
static zend_long opcache_scalar_values[2];
static bool opcache_scalar_present[2];

static zend_long *opcache_scalar_address(size_t index)
{
    const char *name = opcache_scalar_names[index];
    zend_ini_entry *entry = zend_hash_str_find_ptr(EG(ini_directives), name, strlen(name));
    if (!entry || !entry->mh_arg2) return NULL;
    return (zend_long *)((char *)ts_resource(*((int *)entry->mh_arg2)) + (size_t)entry->mh_arg1);
}

static void capture_opcache_scalars(void)
{
    for (size_t i = 0; i < 2; ++i) {
        zend_long *address = opcache_scalar_address(i);
        opcache_scalar_present[i] = address != NULL;
        if (address) opcache_scalar_values[i] = *address;
    }
}

static void initialize_opcache_scalars(void)
{
    for (size_t i = 0; i < 2; ++i) if (opcache_scalar_present[i]) {
        zend_long *address = opcache_scalar_address(i);
        if (address) *address = opcache_scalar_values[i];
    }
}

static int startup(sapi_module_struct *module)
{
    if (hp_env_startup()) return FAILURE;
    php_import_environment_variables = hp_env_import;
    return php_module_startup(module, &frankenphp_module);
}

static bool (*parallel_is_worker)(void);
/* Static PHP extensions have no dlopen handle or dynamic symbol table. Their
 * module entry keeps php_parallel.c linked, so weak direct references resolve
 * without imposing a parallel dependency on the shared development profile. */
extern int php_parallel_set_host_hooks(const hp_parallel_host_hooks_t *) __attribute__((weak));
extern bool php_parallel_is_parallel_worker_thread(void) __attribute__((weak));
static void register_slot_locked(void)
{
    local_slot.interrupt = &EG(vm_interrupt);
    local_slot.timed_out = &EG(timed_out);
    local_slot.request = NULL; local_slot.owner = local_owner;
    local_slot.thread = pthread_self(); local_slot.auxiliary = local_auxiliary; local_slot.busy = true;
    local_slot.next = slots; slots = &local_slot; local_slot.registered = true;
}
static void register_slot(void)
{
    pthread_mutex_lock(&slots_mutex); register_slot_locked(); pthread_mutex_unlock(&slots_mutex);
}
static void unregister_slot(void)
{
    pthread_mutex_lock(&slots_mutex);
    hp_php_slot_t **link = &slots;
    while (*link && *link != &local_slot) link = &(*link)->next;
    if (*link) *link = local_slot.next;
    local_slot.request = NULL; local_slot.worker_token = NULL; local_slot.owner = NULL;
    local_slot.interrupt = local_slot.timed_out = NULL; local_slot.next = NULL; local_slot.registered = false;
    pthread_mutex_unlock(&slots_mutex);
}
static int activate(void)
{
    if (hp_process_is_fork_child() || local_slot.registered || !parallel_is_worker || !parallel_is_worker()) return SUCCESS;
    hp_php_lease_t *lease = SG(server_context);
    if (!lease || lease->magic != HP_LEASE_MAGIC) return SUCCESS;
    local_auxiliary = true; local_owner = lease->owner; owner_retain(local_owner);
    initialize_opcache_scalars();
    pthread_mutex_lock(&slots_mutex);
    /* A pthread already counted as pending must finish bootstrap so the fork
     * gate can observe it reaching idle; newly requested spawns stay frozen. */
    if (!atomic_load(&lease->spawn_pending)) wait_auxiliary_gate();
    if (atomic_exchange(&lease->spawn_pending, false)) --pending_spawns;
    register_slot_locked();
    atomic_fetch_add_explicit(&auxiliary_threads, 1, memory_order_acq_rel);
    atomic_store_explicit(&lease->child_attached, true, memory_order_release);
    pthread_cond_broadcast(&slots_cond); pthread_mutex_unlock(&slots_mutex);
    bind_lease(lease);
    return SUCCESS;
}
static int deactivate(void)
{
    /* Native-only cleanup fallback, including auxiliary interpreter teardown. */
    if (!hp_process_is_fork_child() && is_real_request())
        (void)hp_sapi_response_publish(current_request);
    if (!hp_process_is_fork_child() && local_auxiliary) {
        hp_pcntl_context_leave();
        hp_env_script_reset();
        clear_context();
        unregister_slot();
    }
    return SUCCESS;
}
static void register_parallel_hooks(void)
{
    zend_module_entry *module = zend_hash_str_find_ptr(&module_registry, "parallel", sizeof("parallel") - 1);
    if (!module) return;
    hp_parallel_set_host_hooks_t install = NULL;
    parallel_is_worker = NULL;
    if (!module->handle) {
        install = php_parallel_set_host_hooks;
        parallel_is_worker = php_parallel_is_parallel_worker_thread;
    }
    if (!install || !parallel_is_worker) {
        void *handle = module->handle ? module->handle : RTLD_DEFAULT;
        install = (hp_parallel_set_host_hooks_t)dlsym(handle, "php_parallel_set_host_hooks");
        parallel_is_worker = (bool (*)(void))dlsym(handle, "php_parallel_is_parallel_worker_thread");
    }
    if (!install || !parallel_is_worker) {
        zend_error_noreturn(E_CORE_ERROR, "h2o-php requires parallel built with its versioned native host integration patch");
    }
    const hp_parallel_host_hooks_t hooks = {
        .abi = HP_PARALLEL_HOST_ABI, .size = sizeof(hooks),
        .capture = lease_capture, .release = lease_release,
        .enter = parallel_task_enter, .leave = parallel_task_leave,
        .thread_ready = parallel_thread_ready, .thread_stopping = parallel_thread_stopping,
        .thread_done = parallel_thread_done,
        .thread_joined = parallel_thread_joined, .native_output = parallel_native_output,
        .thread_idle = parallel_thread_idle, .thread_busy = parallel_thread_busy,
        .spawn_prepare = parallel_spawn_prepare, .spawn_parent = parallel_spawn_parent,
    };
    if (install(&hooks) != SUCCESS) zend_error_noreturn(E_CORE_ERROR, "Incompatible parallel host integration ABI");
}

static sapi_module_struct hp_sapi_module = {
    "frankenphp", "H2O Native FrankenPHP-compatible PHP",
    startup, php_module_shutdown_wrapper,
    activate, deactivate,
    write_output, flush_output, NULL, hp_env_get,
    php_error,
    NULL, send_headers, NULL,
    read_post, read_cookies,
    register_variables, log_message, request_time, NULL,
    STANDARD_SAPI_MODULE_PROPERTIES
};

#if PHP_VERSION_ID >= 80400 && defined(ZTS)
static void (*previous_opcache_restart_hook)(int);
static void php_metrics_opcache_restart_hook(int reason)
{
    /* Own cold callback skips the pre-wrapper libc child-handler interval too.
     * No native mutex remains held when an arbitrary previous Zend hook runs;
     * any previous hook bailout therefore stays inside the existing C Zend
     * execution/startup boundary. Do not swallow or emulate that bailout. */
    if (hp_php_metrics_owner_process()) {
        hp_php_metrics_opcache_restart(reason);
        const char *names[] = {"oom", "hash", "manual"};
        const char *name = reason >= 0 && reason < 3 ? names[reason] : "unknown";
        char record[640];
        int n = snprintf(record, sizeof(record),
            "{\"component\":\"php.opcache\",\"severity\":4,\"reason\":\"%s\",\"message\":\"opcache restart scheduled, caching stops until the next request start carries it out while other threads may still reference the old memory: raise opcache.memory_consumption, opcache.max_accelerated_files or opcache.max_wasted_percentage\"}", name);
        if (n > 0 && (size_t)n < sizeof(record)) hp_log_json(record, (size_t)n);
    }
    /* Preserve prior callback order/semantics, including in a fork child.
     * Third-party hook fork-safety is not invented by this collector. */
    if (previous_opcache_restart_hook) previous_opcache_restart_hook(reason);
}
static void install_php_metrics_opcache_hook(void)
{
    previous_opcache_restart_hook = zend_accel_schedule_restart_hook;
    zend_accel_schedule_restart_hook = php_metrics_opcache_restart_hook;
    hp_php_metrics_opcache_available(true);
}
static void restore_php_metrics_opcache_hook(void)
{
    if (zend_accel_schedule_restart_hook == php_metrics_opcache_restart_hook)
        zend_accel_schedule_restart_hook = previous_opcache_restart_hook;
    previous_opcache_restart_hook = NULL;
}
#else
static void install_php_metrics_opcache_hook(void) { hp_php_metrics_opcache_available(false); }
static void restore_php_metrics_opcache_hook(void) {}
#endif

int hp_sapi_init(const char *ini, unsigned max_threads)
{
    if (hp_log_open(hp_config.log_path)) return -1;
    if (hp_env_init()) { hp_log_close(); return -1; }
    if (!php_tsrm_startup_ex((int)max_threads + 1)) { hp_env_shutdown(); hp_log_close(); return -1; }
    zend_signal_startup();
    sapi_startup(&hp_sapi_module);
    /* Match the reference SAPI: installed PHP INI supplies defaults, with
     * explicit server overrides applied last. ZTS builds without independent
     * execution timers require the same timeout fallback as FrankenPHP. */
#ifdef ZEND_MAX_EXECUTION_TIMERS
    const char *defaults = "";
#else
    const char *defaults = "max_execution_time=0\nmax_input_time=-1\n";
#endif
    size_t len = strlen(defaults) + (ini ? strlen(ini) : 0) + 1;
    configured_ini = malloc(len);
    if (!configured_ini) { sapi_shutdown(); tsrm_shutdown(); hp_env_shutdown(); hp_log_close(); return -1; }
    snprintf(configured_ini, len, "%s%s", defaults, ini ? ini : "");
    hp_sapi_module.ini_entries = configured_ini;
    if (hp_sapi_module.startup(&hp_sapi_module) == FAILURE) {
        sapi_shutdown(); tsrm_shutdown(); hp_env_shutdown();
        free(configured_ini); configured_ini = NULL;
        hp_sapi_module.ini_entries = NULL;
        hp_log_close();
        return -1;
    }
    /* Extensions may install their own reader during module startup. A
     * symbolically bound shared PHP stores its implementation address, while
     * this executable's direct reference can name its canonical PLT entry.
     * RTLD_NEXT skips that executable entry; direct equality also covers PHP
     * linked statically or with ordinary interposition. */
    void (*shared_default_post_reader)(void) =
        (void (*)(void))dlsym(RTLD_NEXT, "php_default_post_reader");
    if (sapi_module.default_post_reader == php_default_post_reader ||
        (shared_default_post_reader && sapi_module.default_post_reader == shared_default_post_reader))
        sapi_register_default_post_reader(read_default_post);
    install_php_metrics_opcache_hook();
    /* The installed default unsafe_raw alone must not enable PARSE_SERVER:
     * the pinned SAPI calls this hook only for an explicit config entry. */
    char *configured_filter = NULL;
    (void)cfg_get_string("filter.default", &configured_filter);
    filter_server_variables = configured_filter != NULL;
    override_opcache();
    register_parallel_hooks();
    if (hp_pcntl_install() != 0) zend_error_noreturn(E_CORE_ERROR, "Cannot install the required PCNTL runtime integration");
    hp_pcntl_finalize();
    default_ignore_user_abort = PG(ignore_user_abort);
    capture_opcache_scalars();
    engine_started = true;
    return 0;
}

void hp_sapi_shutdown(void)
{
    if (!engine_started) return;
    /* Execution parents normally join children during their full RSHUTDOWN.
     * Never destroy extension/TSRM globals while any child remains joinable. */
    pthread_mutex_lock(&slots_mutex);
    while (atomic_load_explicit(&auxiliary_threads, memory_order_acquire)) {
        for (hp_php_slot_t *slot = slots; slot; slot = slot->next) {
            if (slot->owner && slot->interrupt) {
                zend_atomic_bool_store(slot->timed_out, true);
                zend_atomic_bool_store(slot->interrupt, true);
            }
        }
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_nsec += 10000000;
        if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&slots_cond, &slots_mutex, &until);
    }
    pthread_mutex_unlock(&slots_mutex);
    hp_sapi_module.shutdown(&hp_sapi_module);
    restore_php_metrics_opcache_hook();
    sapi_shutdown();
    tsrm_shutdown();
    hp_env_shutdown();
    free(configured_ini); configured_ini = NULL;
    hp_sapi_module.ini_entries = NULL;
    engine_started = false;
    owner_release(local_owner); local_owner = NULL;
    hp_log_close();
}

void hp_sapi_thread_init(void)
{
    (void)ts_resource(0);
    local_owner = owner_new();
    initialize_opcache_scalars(); register_slot();
}

void hp_sapi_thread_shutdown(void)
{
    if (!hp_process_is_fork_child() && is_real_request())
        (void)hp_sapi_response_publish(current_request);
    hp_pcntl_context_leave();
    unregister_slot();
    /* An execution thread can remain unused for its whole lifetime. */
    virtual_cwd_deactivate();
    ts_free_thread();
    owner_release(local_owner); local_owner = NULL;
}

/* OPcache timestamps have second resolution. Filesystem watch uses
 * nanoseconds, so a normal generation reboot alone can execute stale same-
 * second bytecode. Invalidate watched files through the installed extension's
 * public native function before compiling the new worker. */
static bool invalidate_cached_file(zend_function *function, const char *path)
{
    bool bailed = false;
    zend_try {
        zval args[2], result;
        ZVAL_STRING(&args[0], path);
        ZVAL_TRUE(&args[1]);
        ZVAL_UNDEF(&result);
        zend_call_known_function(function, NULL, NULL, &result, 2, args, NULL);
        zval_ptr_dtor(&args[0]);
        if (!Z_ISUNDEF(result)) zval_ptr_dtor(&result);
    } zend_catch { bailed = true; } zend_end_try();
    return !bailed;
}

static bool invalidate_watch_tree(zend_function *function, const char *path, unsigned depth)
{
    if (depth > 32) return true;
    struct stat st;
    if (lstat(path, &st)) return true;
    if (S_ISREG(st.st_mode)) return invalidate_cached_file(function, path);
    if (!S_ISDIR(st.st_mode)) return true; /* never recurse through symlinks */
    DIR *directory = opendir(path);
    if (!directory) return true;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char *child;
        if (asprintf(&child, "%s/%s", path, entry->d_name) < 0) { ok = false; break; }
        ok = invalidate_watch_tree(function, child, depth + 1);
        free(child);
        if (!ok) break;
    }
    closedir(directory);
    return ok;
}

static bool invalidate_watch_pattern(zend_function *function, const char *pattern)
{
    const char *magic = strpbrk(pattern, "*?[{");
    if (!magic) return invalidate_watch_tree(function, pattern, 0);
    const char *slash = magic;
    while (slash > pattern && slash[-1] != '/') --slash;
    size_t length = (size_t)(slash - pattern);
    if (length >= PATH_MAX) return false;
    char root[PATH_MAX];
    if (length) {
        memcpy(root, pattern, length); root[length] = '\0';
        if (length > 1) root[length - 1] = '\0';
    } else strcpy(root, ".");
    /* Invalidation may conservatively cover sibling code under the literal
     * prefix. This also covers recursive ** and brace watches without feeding
     * a glob to lstat or relying on shell glob expansion semantics. */
    return invalidate_watch_tree(function, root, 0);
}

static void invalidate_watched_code(void)
{
    if (!current_group) return;
    const hp_worker_config_t *config = hp_worker_group_config(current_group);
    if (!config->num_watch) return;
    zend_function *function = zend_hash_str_find_ptr(EG(function_table), "opcache_invalidate", sizeof("opcache_invalidate") - 1);
    if (!function || function->type != ZEND_INTERNAL_FUNCTION) return;
    bool ok = invalidate_cached_file(function, config->file);
    for (size_t i = 0; i < config->num_watch && ok; ++i)
        ok = invalidate_watch_pattern(function, config->watch[i]);
    if (!ok) zend_bailout(); /* directory handles/native path buffers are closed */
}

static hp_execution_result_t execute_script(hp_request_t *request, const char *file)
{
    volatile int status = 0;
    volatile hp_thread_health_t health = HP_THREAD_HEALTHY;
    volatile hp_execution_phase_t phase = HP_PHASE_STARTUP;
    volatile hp_execution_phase_t failure_phase = HP_PHASE_NONE;
    volatile bool startup_attempted = false;
    install_context(request);
    zend_first_try {
        startup_attempted = true;
        if (php_request_startup() == FAILURE) zend_bailout();
        hp_pcntl_context_enter();
        phase = HP_PHASE_EXECUTION;
        partial_active = true;
        invalidate_watched_code();
        zend_file_handle handle;
        zend_stream_init_filename(&handle, file);
        handle.primary_script = true;
        EG(exit_status) = 0;
        if (zend_stream_open(&handle) == SUCCESS) php_execute_script(&handle);
        else {
            SG(sapi_headers).http_response_code = errno == EACCES ? 403 : 404;
            PHPWRITE("No input file specified.\n", sizeof("No input file specified.\n") - 1);
            EG(exit_status) = 255;
        }
        if (hp_process_is_fork_child()) _exit(EG(exit_status));
        zend_destroy_file_handle(&handle);
        status = EG(exit_status);
    } zend_catch {
        if (hp_process_is_fork_child()) _exit(EG(exit_status) ? EG(exit_status) : 255);
        status = 255; health = HP_THREAD_UNHEALTHY; failure_phase = phase;
    } zend_end_try();
    if (hp_process_is_fork_child()) _exit(status);
    if (startup_attempted) {
        /* If a callback exited/fataled, full shutdown cleans its active partial
         * request as well as the persistent script. PHP catches normal exit. */
        phase = HP_PHASE_SHUTDOWN;
        if (is_real_request()) (void)hp_sapi_response_publish(current_request);
        /* Failed startup may skip module RSHUTDOWN entirely. */
        if (failure_phase == HP_PHASE_STARTUP) sapi_headers_finishing = true;
        zend_try { hp_env_script_reset(); php_request_shutdown(NULL); }
        zend_catch { status = 255; health = HP_THREAD_UNHEALTHY; failure_phase = HP_PHASE_SHUTDOWN; }
        zend_end_try();
        hp_pcntl_context_leave();
        hp_env_after_shutdown();
    }
    if (is_real_request()) {
        (void)hp_sapi_response_publish(current_request);
        pthread_mutex_lock(&current_request->mutex);
        bool headers_ready = current_request->headers_ready || current_request->sapi_headers_accepted;
        pthread_mutex_unlock(&current_request->mutex);
        if (status && !headers_ready) hp_response_headers(current_request, 500, NULL, false);
        /* PHP shutdown has already flushed all output and sent headers. */
        hp_response_finish(current_request);
        if (current_group) hp_runtime_request_done(current_request);
    }
    clear_context();
    current_request = NULL;
    SG(server_context) = NULL;
    partial_active = false;
    return (hp_execution_result_t){.exit_status = status, .health = health, .phase = failure_phase};
}

hp_execution_result_t hp_sapi_execute(hp_request_t *request)
{
    current_group = NULL;
    return execute_script(request, request->script);
}

hp_execution_result_t hp_sapi_worker(hp_worker_group_t *group, const char *file)
{
    current_group = group;
    worker_bootstrapped = false;
    worker_requests = 0;
    memset(&dummy_request, 0, sizeof(dummy_request));
    /* newWorkerDummyContext uses basename(file) as URI, with the worker's
     * own directory as document root and /basename as SCRIPT_NAME. Config
     * resolves worker files to absolute canonical paths before this call. */
    const char *slash = strrchr(file, '/');
    size_t root_length = slash ? (size_t)(slash - file) : 0;
    char worker_root[PATH_MAX];
    if (!slash || root_length >= sizeof(worker_root)) {
        current_group = NULL;
        return (hp_execution_result_t){.exit_status = 255, .health = HP_THREAD_UNHEALTHY, .phase = HP_PHASE_STARTUP};
    }
    if (!root_length) root_length = 1;
    memcpy(worker_root, file, root_length);
    worker_root[root_length] = '\0';
    dummy_request.method = "GET";
    dummy_request.uri = (char *)(slash + 1);
    dummy_request.script_name = (char *)slash;
    set_worker_dummy_cgi_split(&dummy_request);
    dummy_request.query = "";
    dummy_request.script = (char *)file;
    const hp_worker_config_t *config = hp_worker_group_config(group);
    dummy_request.document_root = worker_root;
    dummy_request.env = config->env;
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    dummy_request.request_time = wall.tv_sec + wall.tv_nsec / 1e9;
    hp_execution_result_t result = execute_script(&dummy_request, file);
    current_group = NULL;
    if (!worker_bootstrapped) {
        result.exit_status = 255;
        if (!worker_bootstrapped && result.phase == HP_PHASE_NONE) result.phase = HP_PHASE_STARTUP;
    }
    return result;
}
