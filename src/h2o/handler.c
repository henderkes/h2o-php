#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "php_handler.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef struct {
    h2o_handler_t super;
    hp_route_config_t config;
} php_handler_t;
typedef struct {
    h2o_multithread_receiver_t receiver;
    h2o_linklist_t generators;
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
    h2o_timer_t timeout;
    h2o_linklist_t context_link;
    size_t input_offset;
    unsigned started : 1, in_flight : 1, finalized : 1, input_waiting : 1, input_eof : 1, aborted : 1;
} php_generator_t;

static void pump_output(php_generator_t *g);
static void pump_input(php_generator_t *g);

static char *copy_iov(h2o_iovec_t v) { return strndup(v.base ? v.base : "", v.len); }
static void notify_request(hp_request_t *r)
{
    if (atomic_exchange_explicit(&r->notification_pending, true, memory_order_acq_rel))
        return;
    php_message_t *m = calloc(1, sizeof(*m));
    if (!m) abort();
    hp_request_retain(r);
    m->request = r;
    h2o_multithread_send_message((h2o_multithread_receiver_t *)r->notify_data, &m->super);
}
static void receive_messages(h2o_multithread_receiver_t *receiver, h2o_linklist_t *messages)
{
    (void)receiver;
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
    }
}
static void context_init(h2o_handler_t *handler, h2o_context_t *ctx)
{
    php_context_t *p = calloc(1, sizeof(*p));
    if (!p) abort();
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
    if (g->sending) { hp_chunk_free(g->sending); g->sending = NULL; }
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
    if (g->sending) { hp_chunk_free(g->sending); g->sending = NULL; }
    g->in_flight = 0;
    pump_output(g);
}
static const char *reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK"; case 201: return "Created"; case 204: return "No Content";
    case 301: return "Moved Permanently"; case 302: return "Found"; case 304: return "Not Modified";
    case 400: return "Bad Request"; case 403: return "Forbidden"; case 404: return "Not Found";
    case 413: return "Content Too Large"; case 429: return "Too Many Requests";
    case 500: return "Internal Server Error"; case 503: return "Service Unavailable"; case 504: return "Gateway Timeout";
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
static void pump_output(php_generator_t *g)
{
    if (!g->native || g->aborted) return;
    hp_request_t *r = g->native;
    pthread_mutex_lock(&r->mutex);
    bool cancelled = r->cancelled;
    int cancellation_status = r->status >= 500 ? r->status : 504;
    pthread_mutex_unlock(&r->mutex);
    if (cancelled) { abort_response(g, cancellation_status); return; }
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
        if (!g->started) {
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
    hp_chunk_t *chunk = hp_response_pop(r);
    pthread_mutex_lock(&r->mutex);
    bool finished = r->response_finished && !r->output_head;
    pthread_mutex_unlock(&r->mutex);
    if (!chunk && !finished) return;
    bool no_body = g->req->method.len == 4 && !memcmp(g->req->method.base, "HEAD", 4);
    no_body |= g->req->res.status == 204 || g->req->res.status == 304;
    if (no_body) {
        while (chunk) { hp_chunk_free(chunk); chunk = hp_response_pop(r); }
        pthread_mutex_lock(&r->mutex);
        finished = r->response_finished && !r->output_head;
        pthread_mutex_unlock(&r->mutex);
        if (!finished) return;
    }
    if (finished) {
        /* Keep the deadline armed until the network request is disposed: a
         * final send can itself remain blocked by a non-reading peer. */
        g->finalized = 1;
    }
    h2o_iovec_t vec;
    size_t count = 0;
    if (chunk) {
        vec = h2o_iovec_init(chunk->data + chunk->offset, chunk->len - chunk->offset);
        count = 1;
        g->sending = chunk;
    }
    g->in_flight = !finished;
    h2o_send(g->req, count ? &vec : NULL, count, finished ? H2O_SEND_STATE_FINAL : H2O_SEND_STATE_IN_PROGRESS);
    /* Do not touch g: h2o_send can synchronously dispose its request pool. */
}
static void pump_input(php_generator_t *g)
{
    if (!g->native || !g->input_waiting || g->finalized) return;
    hp_request_t *r = g->native;
    pthread_mutex_lock(&r->mutex);
    size_t room = r->body_bytes < r->body_limit ? r->body_limit - r->body_bytes : 0;
    bool cancelled = r->cancelled;
    pthread_mutex_unlock(&r->mutex);
    if (cancelled) return;
    size_t left = g->req->entity.len - g->input_offset;
    size_t n = left < room ? left : room;
    bool eof = g->input_eof && n == left;
    if (n || eof) {
        if (hp_request_body_push(r, g->req->entity.base ? g->req->entity.base + g->input_offset : "", n, eof) != 0) {
            hp_request_cancel(r);
            abort_response(g, 503);
            return;
        }
        g->input_offset += n;
    }
    if (g->input_offset != g->req->entity.len) return;
    if (!g->input_eof && room <= n) return;
    g->input_waiting = 0;
    g->input_offset = 0;
    if (g->req->proceed_req) g->req->proceed_req(g->req, NULL);
}
static int write_request(void *data, int is_end_stream)
{
    php_generator_t *g = data;
    if (!g->native || g->aborted) return -1;
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
    if (!g->native || !g->req || g->aborted) return;
    pthread_mutex_lock(&g->native->mutex);
    g->native->state = HP_TIMED_OUT;
    pthread_mutex_unlock(&g->native->mutex);
    hp_request_cancel(g->native);
    abort_response(g, 504);
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
static int php_path(h2o_iovec_t path)
{
    for (size_t i = 0; i + 4 <= path.len; ++i)
        if (!memcmp(path.base + i, ".php", 4) && (i + 4 == path.len || path.base[i + 4] == '/')) return 1;
    return 0;
}
static char *resolve_script(php_handler_t *handler, h2o_req_t *req, char **script_name, char **path_info)
{
    char candidate[PATH_MAX], resolved[PATH_MAX], root[PATH_MAX];
    const hp_route_config_t *c = &handler->config;
    if (c->group) {
        const hp_worker_config_t *worker = hp_worker_group_config(c->group);
        *script_name = copy_iov(req->path_normalized);
        *path_info = strdup("");
        return strdup(worker->file);
    }
    if (req->path_normalized_has_null_char || !c->root || !realpath(c->root, root)) return NULL;
    if (req->filereq) {
        if (req->filereq->local_path.len >= sizeof(candidate)) return NULL;
        memcpy(candidate, req->filereq->local_path.base, req->filereq->local_path.len);
        candidate[req->filereq->local_path.len] = 0;
        *script_name = copy_iov(req->filereq->script_name);
        *path_info = copy_iov(req->filereq->path_info);
    } else {
        size_t prefix_len = req->pathconf->path.len;
        if (prefix_len > req->path_normalized.len) prefix_len = req->path_normalized.len;
        if (prefix_len && req->pathconf->path.base[prefix_len - 1] == '/') --prefix_len;
        size_t script_len = req->path_normalized.len;
        if (c->split_path) {
            for (size_t i = prefix_len; i + 5 <= req->path_normalized.len; ++i)
                if (!memcmp(req->path_normalized.base + i, ".php/", 5)) { script_len = i + 4; break; }
        }
        if (snprintf(candidate, sizeof(candidate), "%s/%.*s", root, (int)(script_len - prefix_len), req->path_normalized.base + prefix_len) >= (int)sizeof(candidate)) return NULL;
        struct stat initial;
        int exists = realpath(candidate, resolved) && !stat(resolved, &initial);
        if ((!exists || S_ISDIR(initial.st_mode)) && c->front_controller) {
            if (snprintf(candidate, sizeof(candidate), "%s/%s", root, c->front_controller) >= (int)sizeof(candidate)) return NULL;
            if (asprintf(script_name, "%.*s/%s", (int)prefix_len, req->path_normalized.base, c->front_controller) < 0) return NULL;
            *path_info = strdup("");
        } else {
            *script_name = strndup(req->path_normalized.base, script_len);
            *path_info = strndup(req->path_normalized.base + script_len, req->path_normalized.len - script_len);
        }
    }
    if (!realpath(candidate, resolved) || !inside_root(root, resolved)) return NULL;
    struct stat st;
    if (stat(resolved, &st) || !S_ISREG(st.st_mode)) return NULL;
    size_t n = strlen(resolved);
    if (n < 4 || strcmp(resolved + n - 4, ".php")) return NULL;
    return strdup(resolved);
}
static int is_local(h2o_req_t *req)
{
    struct sockaddr_storage ss; char host[NI_MAXHOST];
    socklen_t n = req->conn->callbacks->get_peername(req->conn, (void *)&ss);
    if (!n || h2o_socket_getnumerichost((void *)&ss, n, host) == SIZE_MAX) return 0;
    return !strcmp(host, "127.0.0.1") || !strcmp(host, "::1");
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
        char buf[16384]; hp_metrics_json(buf, sizeof(buf));
        req->res.status = 200; req->res.reason = "OK";
        h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("application/json"));
        h2o_send_inline(req, buf, SIZE_MAX); return 0;
    }
    char *script_name = NULL, *path_info = NULL;
    char *script = resolve_script(handler, req, &script_name, &path_info);
    if (!script) {
        free(script_name); free(path_info);
        if (req->filereq || handler->config.group || php_path(req->path_normalized)) {
            h2o_send_error_404(req, "Not Found", "PHP script not found\n", 0); return 0;
        }
        return -1;
    }
    hp_request_t *r = hp_request_new();
    r->script = script; r->script_name = script_name; r->path_info = path_info;
    r->group = handler->config.group;
    r->method = copy_iov(req->method); r->uri = copy_iov(req->path);
    r->query = req->query_at == SIZE_MAX ? strdup("") : strndup(req->path.base + req->query_at + 1, req->path.len - req->query_at - 1);
    char protocol[16]; size_t plen = h2o_stringify_protocol_version(protocol, req->version);
    r->protocol = strndup(protocol, plen);
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
    const char *value = hp_header_get(r->headers, "content-type"); r->content_type = strdup(value ? value : "");
    value = hp_header_get(r->headers, "cookie"); r->cookie = strdup(value ? value : "");
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
    h2o_linklist_insert(&ctx->generators, &g->context_link);
    r->notify = notify_request; r->notify_data = &ctx->receiver; r->network_data = g;
    uint64_t timeout = handler->config.timeout_ms ? handler->config.timeout_ms : hp_config.request_timeout_ms;
    if (timeout) { r->deadline_ns = hp_now_ns() + timeout * 1000000; h2o_timer_link(req->conn->ctx->loop, timeout, &g->timeout); }
    g->input_waiting = 1; g->input_eof = req->proceed_req == NULL;
    if (req->proceed_req) { req->write_req.cb = write_request; req->write_req.ctx = g; }
    if (hp_runtime_submit(r) != 0) {
        close_generator(g);
        h2o_send_error_503(req, "Service Unavailable", "PHP request queue is full or draining\n", H2O_SEND_ERROR_HTTP1_CLOSE_CONNECTION);
        return 0;
    }
    pump_input(g);
    return 0;
}
static void handler_dispose(h2o_handler_t *self)
{
    php_handler_t *handler = (void *)self;
    free(handler->config.root); free(handler->config.front_controller); hp_headers_free(handler->config.env);
}
void hp_h2o_register(h2o_pathconf_t *path, const hp_route_config_t *config)
{
    php_handler_t *handler = (void *)h2o_create_handler(path, sizeof(*handler));
    handler->super.on_req = handle_request; handler->super.on_context_init = context_init;
    handler->super.on_context_dispose = context_dispose; handler->super.dispose = handler_dispose;
    handler->super.supports_request_streaming = 1;
    handler->config = *config;
    handler->config.root = config->root ? strdup(config->root) : NULL;
    handler->config.front_controller = config->front_controller ? strdup(config->front_controller) : NULL;
    handler->config.env = hp_headers_clone(config->env);
}

void h2o_php_context_shutdown(void *context)
{
    h2o_context_t *ctx = context;
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
            for (h2o_linklist_t *link = p->generators.next; link != &p->generators; link = link->next) {
                php_generator_t *g = H2O_STRUCT_FROM_MEMBER(php_generator_t, context_link, link);
                if (g->aborted) continue;
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
