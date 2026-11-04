#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "php_handler.h"
#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <netdb.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

typedef struct {
    h2o_handler_t super;
    hp_route_config_t config;
    H2O_VECTOR(char *) static_roots;
} php_handler_t;
typedef struct {
    h2o_multithread_receiver_t receiver;
    h2o_linklist_t generators;
    uint64_t drain_deadline_ns;
    atomic_size_t pending_messages;
} php_context_t;
typedef struct {
    h2o_multithread_message_t super;
    hp_request_t *request;
} php_message_t;
typedef struct {
    h2o_generator_t super;
    h2o_req_t *req;
    hp_request_t *native;
    hp_chunk_t *sending;
    h2o_timer_t timeout, body_timeout, response_timeout;
    uint64_t body_timeout_ms, response_timeout_ms;
    h2o_linklist_t context_link;
    size_t input_offset;
    int abort_status;
    unsigned started : 1, headers_sent : 1, in_flight : 1, finalized : 1, input_waiting : 1, input_eof : 1, aborted : 1;
} php_generator_t;

static void pump_output(php_generator_t *g);
static void pump_input(php_generator_t *g);
static void arm_body_timeout(php_generator_t *g);
static void arm_response_timeout(php_generator_t *g);

static char *copy_iov(h2o_iovec_t v) { return strndup(v.base ? v.base : "", v.len); }
static char *join_cookie_fields(const hp_header_t *headers)
{
    size_t size = 1, count = 0;
    for (const hp_header_t *h = headers; h; h = h->next) {
        if (strcasecmp(h->name, "cookie") != 0) continue;
        size += strlen(h->value) + (count++ ? 2 : 0);
    }
    char *joined = h2o_mem_alloc(size), *at = joined;
    count = 0;
    for (const hp_header_t *h = headers; h; h = h->next) {
        if (strcasecmp(h->name, "cookie") != 0) continue;
        if (count++) { memcpy(at, "; ", 2); at += 2; }
        size_t length = strlen(h->value);
        memcpy(at, h->value, length); at += length;
    }
    *at = '\0';
    return joined;
}
static void notify_request(hp_request_t *r)
{
    if (atomic_exchange_explicit(&r->notification_pending, true, memory_order_acq_rel))
        return;
    php_message_t *m = calloc(1, sizeof(*m));
    if (!m) abort();
    hp_request_retain(r);
    m->request = r;
    php_context_t *context = H2O_STRUCT_FROM_MEMBER(php_context_t, receiver, (h2o_multithread_receiver_t *)r->notify_data);
    atomic_fetch_add_explicit(&context->pending_messages, 1, memory_order_relaxed);
    h2o_multithread_send_message((h2o_multithread_receiver_t *)r->notify_data, &m->super);
}
static void receive_messages(h2o_multithread_receiver_t *receiver, h2o_linklist_t *messages)
{
    php_context_t *context = H2O_STRUCT_FROM_MEMBER(php_context_t, receiver, receiver);
    while (!h2o_linklist_is_empty(messages)) {
        php_message_t *m = H2O_STRUCT_FROM_MEMBER(php_message_t, super.link, messages->next);
        h2o_linklist_unlink(&m->super.link);
        hp_request_t *r = m->request;
        atomic_store_explicit(&r->notification_pending, false, memory_order_release);
        php_generator_t *g = r->network_data;
        if (g) {
            pump_input(g);
            /* Input resumption may synchronously dispose the H2O request. */
            g = r->network_data;
            if (g) pump_output(g);
        }
        hp_request_release(r);
        free(m);
        atomic_fetch_sub_explicit(&context->pending_messages, 1, memory_order_release);
    }
}
static void context_init(h2o_handler_t *handler, h2o_context_t *ctx)
{
    php_context_t *p = calloc(1, sizeof(*p));
    if (!p) abort();
    atomic_init(&p->pending_messages, 0);
    h2o_linklist_init_anchor(&p->generators);
    h2o_multithread_register_receiver(ctx->queue, &p->receiver, receive_messages);
    h2o_context_set_handler_context(ctx, handler, p);
}
static void context_dispose(h2o_handler_t *handler, h2o_context_t *ctx)
{
    php_context_t *p = h2o_context_get_handler_context(ctx, handler);
    h2o_multithread_unregister_receiver(ctx->queue, &p->receiver);
    free(p);
}
static void close_generator(void *data)
{
    php_generator_t *g = data;
    if (h2o_linklist_is_linked(&g->context_link)) h2o_linklist_unlink(&g->context_link);
    if (h2o_timer_is_linked(&g->timeout)) h2o_timer_unlink(&g->timeout);
    if (h2o_timer_is_linked(&g->body_timeout)) h2o_timer_unlink(&g->body_timeout);
    if (h2o_timer_is_linked(&g->response_timeout)) h2o_timer_unlink(&g->response_timeout);
    if (g->sending) { hp_response_release_chunks(g->native, g->sending); g->sending = NULL; }
    if (g->native) {
        hp_request_t *r = g->native;
        r->network_data = NULL;
        pthread_mutex_lock(&r->mutex);
        bool completed_response = g->finalized && r->response_finished;
        if (completed_response) {
            r->body_eof = true;
            pthread_cond_broadcast(&r->body_cond);
        }
        pthread_mutex_unlock(&r->mutex);
        if (!completed_response) hp_request_cancel(r);
        hp_request_release(r);
        g->native = NULL;
    }
    g->req = NULL;
}
static void stop_generator(h2o_generator_t *generator, h2o_req_t *req)
{
    (void)req;
    close_generator(generator);
}
static void proceed_generator(h2o_generator_t *generator, h2o_req_t *req)
{
    (void)req;
    php_generator_t *g = (void *)generator;
    if (h2o_timer_is_linked(&g->response_timeout)) h2o_timer_unlink(&g->response_timeout);
    if (g->sending) { hp_response_release_chunks(g->native, g->sending); g->sending = NULL; }
    g->in_flight = 0;
    pump_output(g);
}
static const char *reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK"; case 201: return "Created"; case 204: return "No Content";
    case 301: return "Moved Permanently"; case 302: return "Found"; case 304: return "Not Modified";
    case 400: return "Bad Request"; case 408: return "Request Timeout"; case 403: return "Forbidden"; case 404: return "Not Found";
    case 413: return "Content Too Large"; case 429: return "Too Many Requests";
    case 500: return "Internal Server Error"; case 503: return "Service Unavailable"; case 504: return "Gateway Timeout";
    case 101: return "Switching Protocols";
    case 103: return "Early Hints"; default: return "Response";
    }
}
static void apply_headers(h2o_req_t *req, int status, hp_header_t *headers)
{
    req->res.status = status;
    req->res.reason = reason_phrase(status);
    req->res.content_length = SIZE_MAX;
    for (hp_header_t *h = headers; h; h = h->next) {
        size_t n = strlen(h->name);
        char *name = h2o_mem_alloc_pool(&req->pool, char, n + 1);
        for (size_t i = 0; i < n; ++i) name[i] = (char)tolower((unsigned char)h->name[i]);
        name[n] = 0;
        if (!strcmp(name, "content-length")) {
            /* A final 101 is header-only: never frame it with Content-Length. */
            if (status == 101) continue;
            char *end; errno = 0;
            unsigned long long value = strtoull(h->value, &end, 10);
            if (!errno && !*end && end != h->value && value < SIZE_MAX) req->res.content_length = (size_t)value;
            continue;
        }
        if (!strcmp(name, "connection") || !strcmp(name, "transfer-encoding") || !strcmp(name, "keep-alive")) continue;
        h2o_iovec_t value = h2o_strdup(&req->pool, h->value, SIZE_MAX);
        h2o_add_header_by_str(&req->pool, &req->res.headers, name, n, 1, NULL, value.base, value.len);
    }
}
static void abort_response(php_generator_t *g, int status)
{
    if (!g->native || !g->req || g->aborted) return;
    g->aborted = 1;
    if (h2o_timer_is_linked(&g->timeout)) h2o_timer_unlink(&g->timeout);
    if (h2o_timer_is_linked(&g->body_timeout)) h2o_timer_unlink(&g->body_timeout);
    if (h2o_timer_is_linked(&g->response_timeout)) h2o_timer_unlink(&g->response_timeout);
    h2o_req_t *req = g->req;
    if (!g->started && !g->finalized && !req->conn->ctx->shutdown_requested) {
        g->finalized = 1;
        h2o_send_error_generic(req, status, reason_phrase(status), "PHP request cancelled or timed out\n",
                               H2O_SEND_ERROR_HTTP1_CLOSE_CONNECTION);
    } else if (req->conn->callbacks->abort_request) {
        /* Protocol code owns pending sends and can cancel them without waiting
         * for a slow peer. This call can synchronously destroy g and req. */
        req->conn->callbacks->abort_request(req);
    } else if (!g->in_flight && !g->finalized) {
        g->finalized = 1;
        h2o_send(req, NULL, 0, H2O_SEND_STATE_ERROR);
    }
}
static void schedule_abort(php_generator_t *g, int status)
{
    if (!g->native || !g->req || g->aborted) return;
    if (!g->abort_status) g->abort_status = status;
    /* Input/proceed callbacks can have protocol frames above them that still
     * use the request. Tear down transport only from an independent timer. */
    if (h2o_timer_is_linked(&g->timeout)) h2o_timer_unlink(&g->timeout);
    if (h2o_timer_is_linked(&g->body_timeout)) h2o_timer_unlink(&g->body_timeout);
    if (h2o_timer_is_linked(&g->response_timeout)) h2o_timer_unlink(&g->response_timeout);
    h2o_timer_link(g->req->conn->ctx->loop, 0, &g->timeout);
}
static void on_body_timeout(h2o_timer_t *timer)
{
    php_generator_t *g = H2O_STRUCT_FROM_MEMBER(php_generator_t, body_timeout, timer);
    if (!g->native || !g->req || g->input_eof || g->aborted) return;
    pthread_mutex_lock(&g->native->mutex); g->native->state = HP_TIMED_OUT; pthread_mutex_unlock(&g->native->mutex);
    hp_request_cancel(g->native); schedule_abort(g, 408);
}
static void on_response_timeout(h2o_timer_t *timer)
{
    php_generator_t *g = H2O_STRUCT_FROM_MEMBER(php_generator_t, response_timeout, timer);
    if (!g->native || !g->req || g->aborted) return;
    pthread_mutex_lock(&g->native->mutex); g->native->state = HP_TIMED_OUT; pthread_mutex_unlock(&g->native->mutex);
    hp_request_cancel(g->native); schedule_abort(g, 504);
}
static void arm_body_timeout(php_generator_t *g)
{
    if (h2o_timer_is_linked(&g->body_timeout)) h2o_timer_unlink(&g->body_timeout);
    if (g->body_timeout_ms && !g->input_eof && !g->aborted)
        h2o_timer_link(g->req->conn->ctx->loop, g->body_timeout_ms, &g->body_timeout);
}
static void arm_response_timeout(php_generator_t *g)
{
    if (h2o_timer_is_linked(&g->response_timeout)) h2o_timer_unlink(&g->response_timeout);
    if (g->response_timeout_ms && !g->aborted)
        h2o_timer_link(g->req->conn->ctx->loop, g->response_timeout_ms, &g->response_timeout);
}
static void pump_output(php_generator_t *g)
{
    if (!g->native || g->aborted) return;
    hp_request_t *r = g->native;
    pthread_mutex_lock(&r->mutex);
    bool cancelled = r->cancelled;
    int status = r->status >= 500 ? r->status : 504;
    pthread_mutex_unlock(&r->mutex);
    if (cancelled) { schedule_abort(g, status); return; }
    if (g->finalized || g->in_flight) return;
    for (;;) {
        pthread_mutex_lock(&r->mutex);
        hp_info_t *info = r->info_head;
        if (info) {
            r->info_head = info->next;
            if (!r->info_head) r->info_tail = NULL;
        }
        pthread_mutex_unlock(&r->mutex);
        if (!info) break;
        /* H2O's informational API excludes 101 (it asserts); the bridge never
         * queues a 101 info entry — this only guards the call site. */
        if (!g->started && info->status != 101) {
            apply_headers(g->req, info->status, info->headers);
            h2o_send_informational(g->req);
        }
        hp_headers_free(info->headers);
        free(info);
    }
    if (!g->started) {
        pthread_mutex_lock(&r->mutex);
        bool ready = r->headers_ready;
        if (ready) {
            apply_headers(g->req, r->status, r->response_headers);
            r->headers_taken = true;
        }
        pthread_mutex_unlock(&r->mutex);
        if (!ready) return;
        g->started = 1;
        h2o_start_response(g->req, &g->super);
    }
    /* Send only what is already available, without waiting for another PHP
     * write or flush. Queued/reserved plus in-flight payload stays within the
     * configured per-request limit; global credit lasts through completion. */
    enum { MAX_SEND_CHUNKS = 16, MAX_SEND_BYTES = 262144 };
    size_t batch_bytes = r->output_limit < MAX_SEND_BYTES ? r->output_limit : MAX_SEND_BYTES;
    bool finished;
    hp_chunk_t *chunks = hp_response_pop_batch(r, MAX_SEND_CHUNKS, batch_bytes, &finished);
    bool no_body = g->req->method.len == 4 && !memcmp(g->req->method.base, "HEAD", 4);
    no_body |= g->req->res.status == 204 || g->req->res.status == 304;
    no_body |= g->req->res.status == 101; /* final 101 is bodyless on every pump */
    if (no_body) {
        while (chunks) {
            hp_response_release_chunks(r, chunks);
            chunks = hp_response_pop_batch(r, MAX_SEND_CHUNKS, batch_bytes, &finished);
        }
    }
    if (!chunks && !finished) {
        /* start_response only installs filters. An explicit first flush must
         * send headers even without payload; empty sends after that are not
         * valid progress in H2O. The empty pop already published idle, so a
         * flush racing this read either appears here or queues another wake. */
        if (g->headers_sent) return;
        pthread_mutex_lock(&r->mutex);
        bool flush_requested = r->output_flush_requested;
        pthread_mutex_unlock(&r->mutex);
        if (!flush_requested) return;
    }
    if (finished) {
        /* Keep the deadline armed until the network request is disposed: a
         * final send can itself remain blocked by a non-reading peer. */
        g->finalized = 1;
        /* An early final response does not mean the upload has reached EOF.
         * Keep its idle deadline while the owning loop drains unread input. */
        if (g->input_eof && h2o_timer_is_linked(&g->body_timeout)) h2o_timer_unlink(&g->body_timeout);
    }
    h2o_iovec_t vec[MAX_SEND_CHUNKS];
    size_t count = 0;
    for (hp_chunk_t *p = chunks; p; p = p->next)
        vec[count++] = h2o_iovec_init(p->data + p->offset, p->len - p->offset);
    g->sending = chunks;
    g->in_flight = !finished;
    arm_response_timeout(g);
    g->headers_sent = 1;
    h2o_send(g->req, count ? vec : NULL, count, finished ? H2O_SEND_STATE_FINAL : H2O_SEND_STATE_IN_PROGRESS);
    /* Do not touch g: h2o_send can synchronously dispose its request pool. */
}
static void pump_input(php_generator_t *g)
{
    if (!g->native || !g->input_waiting || g->aborted) return;
    hp_request_t *r = g->native;
Retry:
    pthread_mutex_lock(&r->mutex);
    size_t room = r->body_bytes < r->body_limit ? r->body_limit - r->body_bytes : 0;
    bool cancelled = r->cancelled, discard = r->response_finished;
    r->body_producer_blocked = false;
    pthread_mutex_unlock(&r->mutex);
    if (cancelled) return;
    /* The pinned H2O fork refreshes this view when its HTTP/2 body buffer
     * grows. Only this owning event loop reads it; PHP sees copied chunks. */
    size_t left = g->req->entity.len - g->input_offset;
    /* PHP has relinquished its body after finish. Consume the complete
     * current H2O lease without allocating or copying a discarded prefix. */
    size_t n = discard ? left : (left < room ? left : room);
    bool eof = g->input_eof && n == left;
    if (n || eof) {
        if (hp_request_body_push(r, g->req->entity.base ? g->req->entity.base + g->input_offset : "", n, eof) != 0) {
            hp_request_cancel(r);
            schedule_abort(g, 503);
            return;
        }
        g->input_offset += n;
    }
    if (g->input_offset != g->req->entity.len || (!discard && !g->input_eof && room <= n)) {
        /* Arm after the copy under the reader's mutex. If PHP already
         * drained below low-water, retry here rather than lose that wake. */
        if (hp_request_body_pause(r)) return;
        goto Retry;
    }
    g->input_waiting = 0;
    g->input_offset = 0;
    arm_body_timeout(g);
    /* Proceed may synchronously deliver EOF and dispose this request pool,
     * particularly after the response is final. Do not access g afterwards. */
    if (g->req->proceed_req) g->req->proceed_req(g->req, NULL);
}
static int write_request(void *data, int is_end_stream)
{
    php_generator_t *g = data;
    if (!g->native || g->aborted || (g->finalized && !g->started)) return -1;
    /* The client cannot make progress while H2O waits for our body lease.
     * Re-arm only when that prefix has been consumed and reads resume. */
    if (h2o_timer_is_linked(&g->body_timeout)) h2o_timer_unlink(&g->body_timeout);
    g->input_waiting = 1;
    g->input_offset = 0;
    g->input_eof = is_end_stream;
    if (is_end_stream) g->req->write_req.cb = NULL;
    pump_input(g);
    return 0;
}
static void on_timeout(h2o_timer_t *timer)
{
    php_generator_t *g = H2O_STRUCT_FROM_MEMBER(php_generator_t, timeout, timer);
    if (!g->native || !g->req) return;
    if (g->aborted) {
        /* An earlier HTTP/2 reset can still await a blocked connection write.
         * Revisit it at process drain so the last stream can close transport. */
        if (g->req->conn->ctx->shutdown_requested && g->req->conn->callbacks->abort_request)
            g->req->conn->callbacks->abort_request(g->req);
        return;
    }
    pthread_mutex_lock(&g->native->mutex);
    if (!g->abort_status) g->native->state = HP_TIMED_OUT;
    int status = g->abort_status ? g->abort_status : 504;
    pthread_mutex_unlock(&g->native->mutex);
    hp_request_cancel(g->native);
    abort_response(g, status);
}
static void set_address(h2o_req_t *req, hp_request_t *r, int peer)
{
    struct sockaddr_storage ss;
    socklen_t n = peer ? req->conn->callbacks->get_peername(req->conn, (void *)&ss) : req->conn->callbacks->get_sockname(req->conn, (void *)&ss);
    char host[NI_MAXHOST], port[16];
    if (!n || h2o_socket_getnumerichost((void *)&ss, n, host) == SIZE_MAX) strcpy(host, "");
    int value = n ? h2o_socket_getport((void *)&ss) : 0;
    snprintf(port, sizeof(port), "%d", value < 0 ? 0 : value);
    if (peer) { r->remote_addr = strdup(host); r->remote_port = strdup(port); }
    else r->server_port = strdup(port);
}
static int inside_root(const char *root, const char *path)
{
    size_t n = strlen(root);
    return !strncmp(root, path, n) && (n == 1 || path[n] == '/' || !path[n]);
}
static size_t mount_prefix(h2o_req_t *req)
{
    size_t n = req->pathconf->path.len;
    if (n > req->path_normalized.len) n = req->path_normalized.len;
    if (n && req->pathconf->path.base[n - 1] == '/') --n;
    return n;
}
/* Delimiters use ASCII folding only, matching FrankenPHP splitPos. Never
 * apply locale or Unicode equivalence to a script delimiter. */
static bool ascii_equal_case_insensitive(const char *a, const char *b, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 128 || y >= 128) return false;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return true;
}
static bool script_suffix(const hp_route_config_t *c, const char *path)
{
    size_t len = strlen(path);
    size_t count = c->num_split_paths ? c->num_split_paths : 1;
    for (size_t i = 0; i < count; ++i) {
        const char *part = c->num_split_paths ? c->split_paths[i] : ".php";
        size_t n = strlen(part);
        if (n <= len && ascii_equal_case_insensitive(path + len - n, part, n)) return true;
    }
    return false;
}
/* Keep no-delimiter distinct from a delimiter ending at the path boundary:
 * Go's unset pathInfo is NULL, while an empty suffix retains its backing data. */
static size_t script_path_split(const hp_route_config_t *c, h2o_iovec_t path, size_t prefix)
{
    if (!c->split_path) return SIZE_MAX;
    size_t count = c->num_split_paths ? c->num_split_paths : 1;
    for (size_t part = 0; part < count; ++part) {
        const char *delimiter = c->num_split_paths ? c->split_paths[part] : ".php";
        size_t n = strlen(delimiter);
        if (n == 0 || prefix > path.len || n > path.len - prefix) continue;
        for (size_t i = prefix; i <= path.len - n; ++i) {
            if (!ascii_equal_case_insensitive(path.base + i, delimiter, n)) continue;
            size_t end = i + n;
            /* Like upstream splitPos, an embedded first occurrence rejects
             * this delimiter entirely, even if a later occurrence fits. */
            if (end != path.len && path.base[end] != '/') break;
            return end;
        }
    }
    return SIZE_MAX;
}
static size_t script_path_length(const hp_route_config_t *c, h2o_iovec_t path, size_t prefix)
{
    size_t split = script_path_split(c, path, prefix);
    return split == SIZE_MAX ? path.len : split;
}
static int php_path(const hp_route_config_t *c, h2o_iovec_t path)
{
    if (script_path_length(c, path, 0) < path.len) return 1;
    char *copy = strndup(path.base, path.len);
    bool result = copy && script_suffix(c, copy);
    free(copy); return result;
}
static int resolve_php_script(const hp_route_config_t *c, const char *root, const char *candidate, char *resolved)
{
    if (!realpath(candidate, resolved) || !inside_root(root, resolved)) return 0;
    struct stat st;
    return !stat(resolved, &st) && S_ISREG(st.st_mode) && script_suffix(c, resolved);
}
static int worker_static_file(php_handler_t *handler, h2o_req_t *req)
{
    if (!handler->static_roots.size || req->filereq || req->path_normalized_has_null_char || php_path(&handler->config, req->path_normalized))
        return 0;
    size_t prefix_len = mount_prefix(req);
    if (req->path_normalized.len - prefix_len > INT_MAX) return 0;
    for (size_t i = 0; i < handler->static_roots.size; ++i) {
        char root[PATH_MAX], candidate[PATH_MAX], resolved[PATH_MAX];
        if (!realpath(handler->static_roots.entries[i], root)) continue;
        int len = snprintf(candidate, sizeof(candidate), "%s/%.*s", root,
                           (int)(req->path_normalized.len - prefix_len), req->path_normalized.base + prefix_len);
        if (len < 0 || len >= (int)sizeof(candidate) || !realpath(candidate, resolved) || !inside_root(root, resolved)) continue;
        if (script_suffix(&handler->config, resolved)) continue;
        struct stat st;
        if (!stat(resolved, &st) && S_ISREG(st.st_mode)) return 1;
    }
    return 0;
}
typedef enum { HP_CGI_PATH_VALID, HP_CGI_PATH_BAD_ESCAPE, HP_CGI_PATH_NUL } hp_cgi_path_error_t;
static int ascii_hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static hp_cgi_path_error_t cgi_path_error(h2o_req_t *req)
{
    /* H2O normalizes any path containing '%' or NUL with norm_indexes.
     * Ordinary paths need no additional scan. Query bytes are not path bytes. */
    if (!req->norm_indexes && !req->path_normalized_has_null_char) return HP_CGI_PATH_VALID;
    const char *query = memchr(req->path.base, '?', req->path.len);
    size_t length = query ? (size_t)(query - req->path.base) : req->path.len;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)req->path.base[i];
        if (!c) return HP_CGI_PATH_NUL;
        if (c != '%') continue;
        if (length - i < 3) return HP_CGI_PATH_BAD_ESCAPE;
        int hi = ascii_hex_value((unsigned char)req->path.base[i + 1]);
        int lo = ascii_hex_value((unsigned char)req->path.base[i + 2]);
        if (hi < 0 || lo < 0) return HP_CGI_PATH_BAD_ESCAPE;
        if (!hi && !lo) return HP_CGI_PATH_NUL;
        /* One URL path decode only: '+' stays '+', and %2500 is literal %00. */
        i += 2;
    }
    return HP_CGI_PATH_VALID;
}
static void reject_cgi_path(h2o_req_t *req, hp_cgi_path_error_t error)
{
    h2o_send_error_400(req, "Bad Request", error == HP_CGI_PATH_NUL ? "invalid request path" : "400 Bad Request", 0);
}
static bool redirect_directory(php_handler_t *handler, h2o_req_t *req, hp_cgi_path_error_t path_error)
{
    const hp_route_config_t *c = &handler->config;
    if (c->mode != HP_ROUTE_SERVER || !c->index_file || !c->root || !req->path_normalized.len ||
        req->path_normalized_has_null_char || req->path_normalized.base[req->path_normalized.len - 1] == '/') return false;
    size_t prefix = mount_prefix(req);
    char root[PATH_MAX], candidate[PATH_MAX], resolved[PATH_MAX];
    if (!realpath(c->root, root)) return false;
    int n = snprintf(candidate, sizeof(candidate), "%s/%.*s/%s", root,
                     (int)(req->path_normalized.len - prefix), req->path_normalized.base + prefix, c->index_file);
    if (n < 0 || n >= (int)sizeof(candidate) || !resolve_php_script(c, root, candidate, resolved)) return false;
    if (path_error) { reject_cgi_path(req, path_error); return true; }
    size_t path_len = req->query_at == SIZE_MAX ? req->path.len : req->query_at;
    size_t query_len = req->path.len - path_len;
    char *location = h2o_mem_alloc_pool(&req->pool, char, req->path.len + 2);
    memcpy(location, req->path.base, path_len); location[path_len] = '/';
    memcpy(location + path_len + 1, req->path.base + path_len, query_len);
    location[req->path.len + 1] = '\0';
    h2o_send_redirect(req, 308, "Permanent Redirect", location, req->path.len + 1);
    return true;
}
/* Result strings belong to the request. document_uri may alias script_name;
 * every other non-NULL result is independently allocated. Only the owning
 * event loop derives them, before publishing the request to PHP threads. */
static void free_script_metadata(char *script_name, char *path_info, char *document_uri)
{
    if (document_uri != script_name) free(document_uri);
    free(script_name); free(path_info);
}
static char *resolve_script(php_handler_t *handler, h2o_req_t *req, hp_worker_group_t *group,
                            char **script_name, char **path_info, char **document_uri)
{
    char candidate[PATH_MAX], resolved[PATH_MAX], root[PATH_MAX];
    const hp_route_config_t *c = &handler->config;
    if (group) {
        const char *file = hp_worker_group_config(group)->file;
        /* splitCgiPath splits the request before deriving an explicitly
         * selected worker's SCRIPT_NAME from its canonical filename. */
        size_t path_len = req->query_at == SIZE_MAX ? req->path.len : req->query_at;
        h2o_iovec_t cgi_path = h2o_iovec_init(req->path.base, path_len);
        /* URL.Path is decoded, but an explicit worker has not passed through
         * Caddy's file rewrite. Retain dot segments for CGI only; routing and
         * filesystem confinement keep using H2O's normalized path. */
        if (memchr(cgi_path.base, '%', cgi_path.len)) {
            cgi_path = h2o_uri_unescape(&req->pool, cgi_path.base, cgi_path.len);
            if (!cgi_path.base) {
                /* Invalid escapes / decoded NUL are not representable by
                 * this metadata path. Preserve the existing dispatch and
                 * normalized CGI fallback; do not add a new route status. */
                *script_name = copy_iov(req->path_normalized);
                *path_info = script_path_split(c, req->path_normalized, 0) == SIZE_MAX ? NULL : strdup("");
                *document_uri = *script_name;
                return strdup(file);
            }
        }
        size_t split = script_path_split(c, cgi_path, 0);
        if (split != SIZE_MAX) {
            *document_uri = strndup(cgi_path.base, split);
            *path_info = strndup(cgi_path.base + split, cgi_path.len - split);
            if (!*document_uri || !*path_info) return NULL;
        }
        const char *root = c->root ? c->root : "";
        size_t root_len = strlen(root);
        if (!strncmp(file, root, root_len) && file[root_len] == '/') {
            *script_name = strdup(file + root_len);
            if (!*script_name) return NULL;
            if (*document_uri && !strcmp(*document_uri, *script_name)) {
                free(*document_uri);
                *document_uri = *script_name;
            }
        } else {
            /* This is CGI metadata only: the explicitly configured worker
             * still runs. Do not broaden request filesystem confinement. */
            free(*document_uri); free(*path_info);
            *document_uri = *path_info = NULL;
        }
        return strdup(file);
    }
    if (c->mode == HP_ROUTE_DIRECT && c->script) {
        *script_name = strdup(c->script_name ? c->script_name : "/index.php");
        *document_uri = *script_name;
        *path_info = strdup(""); return strdup(c->script);
    }
    if (req->path_normalized_has_null_char || !c->root || !realpath(c->root, root)) return NULL;
    if (req->filereq) {
        if (req->filereq->local_path.len >= sizeof(candidate)) return NULL;
        memcpy(candidate, req->filereq->local_path.base, req->filereq->local_path.len);
        candidate[req->filereq->local_path.len] = 0;
        *script_name = copy_iov(req->filereq->script_name); *path_info = copy_iov(req->filereq->path_info);
    } else {
        size_t prefix_len = mount_prefix(req);
        size_t script_len = script_path_length(c, req->path_normalized, prefix_len);
        if (snprintf(candidate, sizeof(candidate), "%s/%.*s", root, (int)(script_len - prefix_len), req->path_normalized.base + prefix_len) >= (int)sizeof(candidate)) return NULL;
        struct stat initial;
        int exists = realpath(candidate, resolved) && !stat(resolved, &initial);
        if ((!exists || S_ISDIR(initial.st_mode)) && c->index_file) {
            char directory_index[PATH_MAX];
            int len = snprintf(directory_index, sizeof(directory_index), "%s/%s", candidate, c->index_file);
            if (len >= 0 && len < (int)sizeof(directory_index) && resolve_php_script(c, root, directory_index, resolved)) {
                strcpy(candidate, resolved);
                const char *slash = script_len && req->path_normalized.base[script_len - 1] == '/' ? "" : "/";
                if (asprintf(script_name, "%.*s%s%s", (int)script_len, req->path_normalized.base, slash, c->index_file) < 0) return NULL;
            } else {
                if (snprintf(candidate, sizeof(candidate), "%s/%s", root, c->index_file) >= (int)sizeof(candidate)) return NULL;
                if (asprintf(script_name, "%.*s/%s", (int)prefix_len, req->path_normalized.base, c->index_file) < 0) return NULL;
            }
            *path_info = strdup("");
        } else {
            *script_name = strndup(req->path_normalized.base, script_len);
            *path_info = strndup(req->path_normalized.base + script_len, req->path_normalized.len - script_len);
        }
    }
    *document_uri = *script_name;
    return resolve_php_script(c, root, candidate, resolved) ? strdup(resolved) : NULL;
}
static int is_local(h2o_req_t *req)
{
    struct sockaddr_storage ss; char host[NI_MAXHOST];
    socklen_t n = req->conn->callbacks->get_peername(req->conn, (void *)&ss);
    if (!n || h2o_socket_getnumerichost((void *)&ss, n, host) == SIZE_MAX) return 0;
    return !strcmp(host, "127.0.0.1") || !strcmp(host, "::1");
}
static bool match_value(h2o_req_t *req, const regex_t *expression, h2o_iovec_t value)
{
    /* Request slices need not be NUL terminated. Keep the regex input in the
     * owning H2O request pool, never in PHP thread state. */
    char *copy = h2o_strdup(&req->pool, value.base, value.len).base;
    return regexec(expression, copy, 0, NULL, 0) == 0;
}
static bool matches_route(hp_route_match_t *match, h2o_req_t *req)
{
    if (!match) return true;
    if (match->globs.size) {
        char *path = h2o_strdup(&req->pool, req->path_normalized.base, req->path_normalized.len).base;
        bool found = false;
        for (size_t i = 0; i < match->globs.size; ++i)
            if (fnmatch(match->globs.entries[i], path, 0) == 0) { found = true; break; }
        if (!found) return false;
    }
    if (match->methods.size) {
        bool found = false;
        for (size_t i = 0; i < match->methods.size; ++i) {
            const char *method = match->methods.entries[i];
            if (h2o_memis(req->method.base, req->method.len, method, strlen(method))) { found = true; break; }
        }
        if (!found) return false;
    }
    if (match->has_path && !match_value(req, &match->path, req->path_normalized)) return false;
    for (size_t i = 0; i < match->headers.size; ++i) {
        const char *name = match->headers.entries[i].name;
        const regex_t *value = &match->headers.entries[i].value;
        bool found = false;
        if (!strcasecmp(name, "host")) found = match_value(req, value, req->authority);
        else for (size_t j = 0; j < req->headers.size; ++j) {
            h2o_header_t *header = req->headers.entries + j;
            if (header->name->len == strlen(name) && !strncasecmp(header->name->base, name, header->name->len) &&
                match_value(req, value, header->value)) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}
static int handle_request(h2o_handler_t *self, h2o_req_t *req)
{
    php_handler_t *handler = (void *)self;
    if (handler->config.metrics || handler->config.restart) {
        if (!is_local(req)) { h2o_send_error_403(req, "Forbidden", "PHP administration requires a loopback peer\n", 0); return 0; }
        if (handler->config.restart) {
            if (!h2o_memis(req->method.base, req->method.len, H2O_STRLIT("POST"))) {
                h2o_send_error_405(req, "Method Not Allowed", "worker restart requires POST\n", 0); return 0;
            }
            hp_runtime_restart_workers();
            req->res.status = 202; req->res.reason = "Accepted";
            h2o_send_inline(req, "{\"restarting\":true}\n", SIZE_MAX); return 0;
        }
        if (handler->config.metrics == HP_METRICS_PROMETHEUS) {
            char *text = NULL; size_t length = 0;
            if (hp_php_metrics_prometheus(&text, &length)) {
                h2o_send_error_503(req, "Service Unavailable", "PHP metrics unavailable\n", 0); return 0;
            }
            req->res.status = 200; req->res.reason = "OK"; req->res.content_length = length;
            h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL,
                H2O_STRLIT("text/plain; version=0.0.4; charset=utf-8"));
            /* h2o_send_inline synchronously copies into req->pool before it
             * starts a response; producer storage is never retained by H2O. */
            h2o_send_inline(req, text, length); free(text); return 0;
        }
        char buf[16384]; hp_metrics_json(buf, sizeof(buf));
        req->res.status = 200; req->res.reason = "OK";
        h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("application/json"));
        h2o_send_inline(req, buf, SIZE_MAX); return 0;
    }
    /* Match the PHP context's origin before CGI metadata/worker resolution,
     * while keeping the native request/deadline timestamps unchanged. */
    uint64_t php_context_started_ns = hp_now_ns();
    hp_worker_group_t *group = handler->config.group;
    if (handler->config.workers) {
        for (size_t i = 0; i < handler->config.workers->entries.size; ++i) {
            hp_route_match_t *match = handler->config.workers->entries.entries[i].match;
            if ((match || handler->config.mode == HP_ROUTE_WORKER) && matches_route(match, req)) {
                group = handler->config.workers->entries.entries[i].group; break;
            }
        }
    }
    if ((handler->config.group || handler->config.mode == HP_ROUTE_WORKER) && worker_static_file(handler, req)) return -1;
    if (!matches_route(handler->config.match, req) ||
        (handler->config.mode == HP_ROUTE_WORKER && !group)) {
        if (handler->static_roots.size) {
            /* Only existing non-PHP files may reach a paired file.dir. A
             * rejected worker predicate must never expose PHP source. */
            h2o_send_error_404(req, "Not Found", "No matching PHP worker route\n", 0); return 0;
        }
        return -1;
    }
    hp_cgi_path_error_t path_error = cgi_path_error(req);
    /* Reject only when this handler owns a PHP workflow. Earlier worker/static
     * and predicate fallthrough stays untouched; unresolved php_server paths
     * may still belong to a later static/proxy handler. No PHP is queued yet. */
    if (path_error && (group || handler->config.mode == HP_ROUTE_DIRECT || req->filereq)) {
        reject_cgi_path(req, path_error); return 0;
    }
    if (!group && redirect_directory(handler, req, path_error)) return 0;
    char *script_name = NULL, *path_info = NULL, *document_uri = NULL;
    char *script = resolve_script(handler, req, group, &script_name, &path_info, &document_uri);
    if (script && !group && handler->config.workers) {
        for (size_t i = 0; i < handler->config.workers->entries.size; ++i) {
            hp_worker_group_t *candidate = handler->config.workers->entries.entries[i].group;
            if (!handler->config.workers->entries.entries[i].match && !strcmp(script, hp_worker_group_config(candidate)->file)) {
                group = candidate; break;
            }
        }
    }
    if (!script) {
        free_script_metadata(script_name, path_info, document_uri);
        if (req->filereq || group || handler->config.mode == HP_ROUTE_DIRECT || handler->config.mode == HP_ROUTE_WORKER ||
            (handler->config.mode == HP_ROUTE_SERVER && !handler->config.file_server) || php_path(&handler->config, req->path_normalized)) {
            if (path_error) reject_cgi_path(req, path_error);
            else h2o_send_error_404(req, "Not Found", "PHP script not found\n", 0);
            return 0;
        }
        return -1;
    }
    if (path_error) {
        free(script); free_script_metadata(script_name, path_info, document_uri);
        reject_cgi_path(req, path_error); return 0;
    }
    hp_request_t *r = hp_request_new();
    if (!r) { free(script); free_script_metadata(script_name, path_info, document_uri); h2o_send_error_503(req, "Service Unavailable", "PHP request allocation failed\n", 0); return 0; }
    r->script = script; r->script_name = script_name; r->path_info = path_info;
    r->document_uri = document_uri;
    r->document_uri_set = true;
    r->document_uri_owned = document_uri && document_uri != script_name;
    r->group = group;
    r->php_metrics.origin_ns = php_context_started_ns; /* Not published yet. */
    r->method = copy_iov(req->method); r->uri = copy_iov(req->path);
    r->query = req->query_at == SIZE_MAX ? strdup("") : strndup(req->path.base + req->query_at + 1, req->path.len - req->query_at - 1);
    char protocol[16]; size_t plen = h2o_stringify_protocol_version(protocol, req->version);
    r->protocol = req->version == 0x200 ? strdup("HTTP/2.0") : req->version == 0x300 ? strdup("HTTP/3.0") : strndup(protocol, plen);
    r->document_root = handler->config.root ? strdup(handler->config.root) : strdup("");
    r->server_name = copy_iov(req->hostconf->authority.host);
    r->tls = req->scheme == &H2O_URL_SCHEME_HTTPS;
    r->content_length = req->content_length;
    struct timespec wall; clock_gettime(CLOCK_REALTIME, &wall);
    r->request_time = (double)wall.tv_sec + (double)wall.tv_nsec / 1e9;
    set_address(req, r, 1); set_address(req, r, 0);
    for (size_t i = 0; i < req->headers.size; ++i) {
        h2o_header_t *h = req->headers.entries + i;
        char *name = copy_iov(*h->name), *value = copy_iov(h->value);
        hp_header_add(&r->headers, name, value); free(name); free(value);
    }
    char *authority = copy_iov(req->authority);
    hp_header_add(&r->headers, "Host", authority); free(authority);
    /* H2/H3 deliver each Cookie field as a separate header; coalesce them
     * into the first Cookie node with the same "; " join r->cookie uses, so
     * CGI/API expose one reference-shaped value. H1 fragments, raw
     * req->headers, node order, and all other headers stay untouched. */
    if (req->version >= 0x200) {
        size_t cookie_fields = 0;
        for (hp_header_t *h = r->headers; h; h = h->next)
            if (!strcasecmp(h->name, "cookie")) ++cookie_fields;
        if (cookie_fields > 1) {
            char *joined = join_cookie_fields(r->headers);
            bool first = true;
            for (hp_header_t **link = &r->headers; *link;) {
                hp_header_t *h = *link;
                if (strcasecmp(h->name, "cookie")) { link = &h->next; continue; }
                if (first) { free(h->value); h->value = joined; first = false; link = &h->next; }
                else { *link = h->next; free(h->name); free(h->value); free(h); }
            }
        }
    }
    const char *value = hp_header_get(r->headers, "content-type"); r->content_type = strdup(value ? value : "");
    r->cookie = join_cookie_fields(r->headers);
    r->env = hp_headers_clone(handler->config.env);
    for (size_t i = 0; i + 1 < req->env.size; i += 2) {
        char *name = copy_iov(req->env.entries[i]), *v = copy_iov(req->env.entries[i + 1]);
        hp_header_add(&r->env, name, v); free(name); free(v);
    }
    php_context_t *ctx = h2o_context_get_handler_context(req->conn->ctx, self);
    php_generator_t *g = h2o_mem_alloc_shared(&req->pool, sizeof(*g), close_generator);
    memset(g, 0, sizeof(*g));
    g->super.proceed = proceed_generator; g->super.stop = stop_generator;
    g->req = req; g->native = r; g->timeout.cb = on_timeout;
    g->body_timeout.cb = on_body_timeout; g->response_timeout.cb = on_response_timeout;
    g->body_timeout_ms = handler->config.body_timeout_ms; g->response_timeout_ms = handler->config.response_timeout_ms;
    h2o_linklist_insert(&ctx->generators, &g->context_link);
    r->notify = notify_request; r->notify_data = &ctx->receiver; r->network_data = g;
    uint64_t timeout = handler->config.timeout_set ? handler->config.timeout_ms : hp_config.request_timeout_ms;
    uint64_t now = hp_now_ns();
    r->deadline_ns = !timeout || timeout > (UINT64_MAX - now) / 1000000ULL ? UINT64_MAX : now + timeout * 1000000ULL;
    if (req->conn->ctx->shutdown_requested) {
        uint64_t remaining = ctx->drain_deadline_ns
            ? (ctx->drain_deadline_ns > now ? (ctx->drain_deadline_ns - now + 999999) / 1000000 : 0)
            : hp_config.shutdown_timeout_ms;
        if (remaining < timeout) timeout = remaining;
    }
    if (timeout || req->conn->ctx->shutdown_requested) h2o_timer_link(req->conn->ctx->loop, timeout, &g->timeout);
    g->input_waiting = 1; g->input_eof = req->proceed_req == NULL;
    if (req->proceed_req) { req->write_req.cb = write_request; req->write_req.ctx = g; }
    if (hp_runtime_submit(r) != 0) {
        /* The core error generator can itself wait on network flow control.
         * Retain our context and timer until pool disposal so rejected work
         * remains bounded, including streams dispatched after drain began. */
        pthread_mutex_lock(&r->mutex);
        r->status = 503; r->headers_ready = true; r->response_finished = true; r->state = HP_FAILED;
        hp_php_metrics_request_end_locked(&r->php_metrics, hp_now_ns());
        pthread_mutex_unlock(&r->mutex);
        g->finalized = 1;
        h2o_send_error_503(req, "Service Unavailable", "PHP request queue is full or draining\n", H2O_SEND_ERROR_HTTP1_CLOSE_CONNECTION);
        return 0;
    }
    pump_input(g);
    return 0;
}
static void handler_dispose(h2o_handler_t *self)
{
    php_handler_t *handler = (void *)self;
    hp_route_config_dispose(&handler->config);
    for (size_t i = 0; i < handler->static_roots.size; ++i) free(handler->static_roots.entries[i]);
    free(handler->static_roots.entries);
}
void hp_h2o_register(h2o_pathconf_t *path, const hp_route_config_t *config)
{
    php_handler_t *handler = (void *)h2o_create_handler(path, sizeof(*handler));
    handler->super.on_req = handle_request; handler->super.on_context_init = context_init;
    handler->super.on_context_dispose = context_dispose; handler->super.dispose = handler_dispose;
    handler->super.supports_request_streaming = 1;
    hp_route_config_clone(&handler->config, config);
    if (config->root && config->resolve_root_symlink) {
        char canonical[PATH_MAX];
        if (realpath(config->root, canonical)) { free(handler->config.root); handler->config.root = strdup(canonical); }
    }
}

void hp_route_match_release(hp_route_match_t *match)
{
    if (!match || atomic_fetch_sub_explicit(&match->refs, 1, memory_order_acq_rel) != 1) return;
    if (match->has_path) regfree(&match->path);
    for (size_t i = 0; i < match->methods.size; ++i) free(match->methods.entries[i]);
    for (size_t i = 0; i < match->headers.size; ++i) {
        free(match->headers.entries[i].name);
        regfree(&match->headers.entries[i].value);
    }
    for (size_t i = 0; i < match->globs.size; ++i) free(match->globs.entries[i]);
    free(match->globs.entries); free(match->methods.entries); free(match->headers.entries); free(match);
}

void hp_worker_routes_release(hp_worker_routes_t *workers)
{
    if (!workers || atomic_fetch_sub_explicit(&workers->refs, 1, memory_order_acq_rel) != 1) return;
    for (size_t i = 0; i < workers->entries.size; ++i) hp_route_match_release(workers->entries.entries[i].match);
    free(workers->entries.entries); free(workers);
}
void hp_route_config_clone(hp_route_config_t *to, const hp_route_config_t *from)
{
    *to = *from;
    to->root = from->root ? strdup(from->root) : NULL;
    to->index_file = from->index_file ? strdup(from->index_file) : NULL;
    to->script = from->script ? strdup(from->script) : NULL;
    to->script_name = from->script_name ? strdup(from->script_name) : NULL;
    to->name = from->name ? strdup(from->name) : NULL;
    to->env = hp_headers_clone(from->env);
    to->split_paths = from->num_split_paths ? calloc(from->num_split_paths, sizeof(char *)) : NULL;
    for (size_t i = 0; i < from->num_split_paths; ++i) to->split_paths[i] = strdup(from->split_paths[i]);
    if (to->match) atomic_fetch_add_explicit(&to->match->refs, 1, memory_order_relaxed);
    if (to->workers) atomic_fetch_add_explicit(&to->workers->refs, 1, memory_order_relaxed);
}
void hp_route_config_dispose(hp_route_config_t *config)
{
    free(config->root); free(config->index_file); free(config->script); free(config->script_name); free(config->name); hp_headers_free(config->env);
    for (size_t i = 0; i < config->num_split_paths; ++i) free(config->split_paths[i]);
    free(config->split_paths); hp_route_match_release(config->match); hp_worker_routes_release(config->workers);
}

int hp_h2o_is_worker(h2o_handler_t *self)
{
    return self->on_context_init == context_init && (((php_handler_t *)self)->config.group != NULL || ((php_handler_t *)self)->config.mode == HP_ROUTE_WORKER);
}
const hp_route_config_t *hp_h2o_handler_config(h2o_handler_t *self)
{
    return self->on_context_init == context_init ? &((php_handler_t *)self)->config : NULL;
}
void hp_h2o_worker_add_static_root(h2o_handler_t *self, const char *root)
{
    assert(hp_h2o_is_worker(self));
    php_handler_t *handler = (void *)self;
    h2o_vector_reserve(NULL, &handler->static_roots, handler->static_roots.size + 1);
    handler->static_roots.entries[handler->static_roots.size++] = h2o_strdup(NULL, root, SIZE_MAX).base;
}

void h2o_php_context_shutdown(void *context)
{
    h2o_context_t *ctx = context;
    uint64_t drain_deadline_ns = hp_now_ns() + hp_config.shutdown_timeout_ms * 1000000;
    /* Called by the owning loop after listener shutdown. PHP execution can
     * already have completed while its final response still waits on TCP or
     * HTTP/2 flow control, so the scheduler's active list alone is insufficient. */
    for (size_t i = 0; i < ctx->_pathconfs_inited.size; ++i) {
        h2o_pathconf_t *path = ctx->_pathconfs_inited.entries[i];
        if (!path) continue;
        for (size_t j = 0; j < path->handlers.size; ++j) {
            h2o_handler_t *handler = path->handlers.entries[j];
            if (handler->on_context_init != context_init) continue;
            php_context_t *p = h2o_context_get_handler_context(ctx, handler);
            p->drain_deadline_ns = drain_deadline_ns;
            for (h2o_linklist_t *link = p->generators.next; link != &p->generators; link = link->next) {
                php_generator_t *g = H2O_STRUCT_FROM_MEMBER(php_generator_t, context_link, link);
                if (h2o_timer_is_linked(&g->timeout)) h2o_timer_unlink(&g->timeout);
                uint64_t now = hp_now_ns();
                uint64_t request_remaining = g->native->deadline_ns > now
                    ? (g->native->deadline_ns - now + 999999) / 1000000 : 0;
                uint64_t timeout = request_remaining < hp_config.shutdown_timeout_ms
                    ? request_remaining : hp_config.shutdown_timeout_ms;
                h2o_timer_link(ctx->loop, timeout, &g->timeout);
            }
        }
    }
}

size_t h2o_php_context_pending(void *context)
{
    h2o_context_t *ctx = context;
    size_t pending = 0;
    /* Called only by this context's owning loop. After controller stop and
     * the all-loop publication barrier, producers cannot add new messages. */
    for (size_t i = 0; i < ctx->_pathconfs_inited.size; ++i) {
        h2o_pathconf_t *path = ctx->_pathconfs_inited.entries[i];
        if (!path) continue;
        for (size_t j = 0; j < path->handlers.size; ++j) {
            h2o_handler_t *handler = path->handlers.entries[j];
            if (handler->on_context_init != context_init) continue;
            php_context_t *p = h2o_context_get_handler_context(ctx, handler);
            pending += atomic_load_explicit(&p->pending_messages, memory_order_acquire);
        }
    }
    return pending;
}
