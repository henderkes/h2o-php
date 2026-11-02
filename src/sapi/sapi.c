/* Native FrankenPHP-compatible SAPI for H2O.
 * Worker lifecycle portions adapted from php/frankenphp
 * 4384c506d219cd2f8750edfceae57d3db8eb54f3.
 * Copyright (c) 2022-present Kévin Dunglas, MIT; see ../php/LICENSE.frankenphp.
 * Zend execution and bailout recovery remain entirely inside this C unit. */
#include "h2o_php.h"
#include "environment.h"
#include <php.h>
#include <SAPI.h>
#include <php_main.h>
#include <php_ini.h>
#include <php_output.h>
#include <php_variables.h>
#include <Zend/zend_exceptions.h>
#include <Zend/zend_interfaces.h>
#include <Zend/zend_smart_str.h>
#include <Zend/zend_signal.h>
#include <ext/spl/spl_exceptions.h>
#include <ext/standard/basic_functions.h>
#include <ext/standard/php_fopen_wrappers.h>
#include <ext/standard/info.h>
#include <ext/session/php_session.h>
#include <ext/json/php_json.h>
#include <dlfcn.h>
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
#if PHP_VERSION_ID < 80500 || PHP_VERSION_ID >= 80600
#error "This SAPI lifecycle port requires PHP 8.5 headers and library"
#endif

static _Thread_local hp_request_t *current_request;
static _Thread_local hp_worker_group_t *current_group;
static _Thread_local hp_request_t dummy_request;
static _Thread_local bool worker_bootstrapped;
static _Thread_local bool partial_active;
static _Thread_local bool http_finished;
static _Thread_local unsigned worker_requests;
static sapi_module_struct hp_sapi_module;
static char *configured_ini;
static bool engine_started;

#include "../php/frankenphp_arginfo.h"

typedef struct hp_php_slot {
    struct hp_php_slot *next;
    hp_request_t *request;
    zend_atomic_bool *interrupt, *timed_out;
} hp_php_slot_t;
static pthread_mutex_t slots_mutex = PTHREAD_MUTEX_INITIALIZER;
static hp_php_slot_t *slots;
static _Thread_local hp_php_slot_t local_slot;

void hp_sapi_interrupt_request(hp_request_t *request)
{
    pthread_mutex_lock(&slots_mutex);
    for (hp_php_slot_t *slot = slots; slot; slot = slot->next) {
        if (!request || request == slot->request) {
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

static bool is_real_request(void) { return current_request && current_request != &dummy_request; }

static void json_string(FILE *out, const char *value)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)value; p && *p; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (*p < 32) fprintf(out, "\\u%04x", *p);
            else fputc(*p, out);
        }
    }
    fputc('"', out);
}

static void log_message(const char *message, int severity)
{
    flockfile(stderr);
    fputs("{\"component\":\"php.engine\",\"message\":", stderr);
    json_string(stderr, message);
    fprintf(stderr, ",\"severity\":%d,\"request_id\":%" PRIu64 "}\n", severity,
            is_real_request() ? current_request->id : 0);
    funlockfile(stderr);
}

static int send_headers(sapi_headers_struct *headers)
{
    if (!is_real_request() || SG(request_info).no_headers || http_finished)
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
    if (status < 100 || status > 999) {
        log_message("Invalid response status code; sending HTTP 500", 4);
        status = 500;
    }
    int result = hp_response_headers(current_request, status, list, status >= 100 && status < 200);
    hp_headers_free(list);
    return result ? SAPI_HEADER_SEND_FAILED : SAPI_HEADER_SENT_SUCCESSFULLY;
}

static size_t write_output(const char *data, size_t length)
{
    if (!is_real_request() || http_finished) return length;
    ssize_t written = hp_response_write(current_request, data, length);
    if (written < 0) {
        php_handle_aborted_connection();
        return 0;
    }
    return (size_t)written;
}

static void flush_output(void *server_context)
{
    (void)server_context;
    if (!http_finished) sapi_send_headers();
    if (is_real_request()) hp_request_notify(current_request);
}

static size_t read_post(char *buffer, size_t count)
{
    if (!is_real_request()) return 0;
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

static char *read_cookies(void) { return current_request ? current_request->cookie : NULL; }

static void regvar(zval *target, const char *name, const char *value)
{
    php_register_variable_safe(name, value ? value : "", value ? strlen(value) : 0, target);
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
    regvar(target, "PHP_SELF", r->script_name ? r->script_name : r->uri);
    regvar(target, "REQUEST_URI", r->uri);
    regvar(target, "DOCUMENT_URI", r->script_name);
    regvar(target, "REQUEST_METHOD", r->method);
    regvar(target, "QUERY_STRING", r->query);
    regvar(target, "CONTENT_TYPE", r->content_type);
    regvar(target, "REQUEST_SCHEME", r->tls ? "https" : "http");
    if (r->tls) regvar(target, "HTTPS", "on");
    char count[32];
    if (r->content_length != SIZE_MAX) {
        snprintf(count, sizeof(count), "%zu", r->content_length);
        regvar(target, "CONTENT_LENGTH", count);
    }
    if (SG(request_info).auth_user) {
        regvar(target, "AUTH_TYPE", "Basic");
        regvar(target, "PHP_AUTH_USER", SG(request_info).auth_user);
        regvar(target, "PHP_AUTH_PW", SG(request_info).auth_password);
        regvar(target, "REMOTE_USER", SG(request_info).auth_user);
    }
    for (hp_header_t *h = r->headers; h; h = h->next) {
        size_t len = strlen(h->name);
        char *name = emalloc(len + 6);
        memcpy(name, "HTTP_", 5);
        for (size_t i = 0; i < len; ++i) {
            unsigned char c = (unsigned char)h->name[i];
            name[i + 5] = c == '-' ? '_' : (c >= 'a' && c <= 'z' ? c - 32 : c);
        }
        name[len + 5] = 0;
        regvar(target, name, h->value);
        efree(name);
    }
    /* Explicit request env entries override generated server variables. */
    for (hp_header_t *h = r->env; h; h = h->next) regvar(target, h->name, h->value);
}

static zend_result request_time(double *time)
{
    if (!current_request) return FAILURE;
    *time = current_request->request_time;
    return SUCCESS;
}

static void install_context(hp_request_t *request)
{
    current_request = request;
    publish_request(request);
    http_finished = false;
    SG(server_context) = &current_request;
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
    PG(ignore_user_abort) = current_group != NULL;
    for (hp_header_t *h = request->env; h; h = h->next) hp_env_set(h->name, h->value);
    if (current_group) hp_env_set("FRANKENPHP_WORKER", "1");
    const char *authorization = hp_header_get(request->headers, "authorization");
    php_handle_auth_data(authorization);
}

static void clear_context(void)
{
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
    if (!is_real_request() || http_finished) return;
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
    SG(request_info).current_user_length = 0;
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
        if (PG(output_handler) && PG(output_handler)[0]) {
            zval handler;
            ZVAL_STRING(&handler, PG(output_handler));
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
    if (PG(max_input_time) != -1) zend_set_timeout(INI_INT("max_execution_time"), 0);
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
    if (http_finished || !is_real_request()) RETURN_FALSE;
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
    int previous = SG(sapi_headers).http_response_code;
    SG(sapi_headers).http_response_code = (int)status;
    if (status >= 100 && status < 200) {
        int result = send_headers(&SG(sapi_headers));
        SG(sapi_headers).http_response_code = previous;
        RETURN_LONG(result);
    }
    RETURN_LONG(sapi_send_headers());
}

PHP_FUNCTION(frankenphp_request_headers)
{
    ZEND_PARSE_PARAMETERS_NONE();
    array_init(return_value);
    if (current_request) for (hp_header_t *h = current_request->headers; h; h = h->next) {
        size_t len = strlen(h->name);
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
    flockfile(stderr);
    if (json.s) fwrite(ZSTR_VAL(json.s), 1, ZSTR_LEN(json.s), stderr);
    fputc('\n', stderr);
    funlockfile(stderr);
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
    hp_env_override_functions();
    override_opcache();
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
    PHP_MINIT(frankenphp), NULL, NULL, NULL,
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

/* Preserve lazy php://input on POST media types without a parser. PHP's
 * stock fallback consumes them before userland executes. URL encoded forms
 * retain their registered POST reader; multipart retains its own parser. */
static void lazy_default_post_reader(void) {}

static int startup(sapi_module_struct *module)
{
    php_import_environment_variables = hp_env_import;
    return php_module_startup(module, &frankenphp_module);
}

static int deactivate(void) { return SUCCESS; }

static sapi_module_struct hp_sapi_module = {
    "frankenphp", "H2O Native FrankenPHP-compatible PHP",
    startup, php_module_shutdown_wrapper,
    NULL, deactivate,
    write_output, flush_output, NULL, hp_env_get,
    php_error,
    NULL, send_headers, NULL,
    read_post, read_cookies,
    register_variables, log_message, request_time, NULL,
    STANDARD_SAPI_MODULE_PROPERTIES
};

int hp_sapi_init(const char *ini, unsigned max_threads)
{
    if (hp_env_init()) return -1;
    if (!php_tsrm_startup_ex((int)max_threads + 1)) { hp_env_shutdown(); return -1; }
    zend_signal_startup();
    sapi_startup(&hp_sapi_module);
    /* Defaults favor HTTP applications; php.ini remains available for installed
     * extensions and configured ini entries are applied last. */
    const char *defaults = "expose_php=0\nhtml_errors=0\nimplicit_flush=0\noutput_buffering=4096\nmax_execution_time=0\nmax_input_time=-1\nregister_argc_argv=0\n";
    size_t len = strlen(defaults) + (ini ? strlen(ini) : 0) + 1;
    configured_ini = malloc(len);
    if (!configured_ini) { sapi_shutdown(); tsrm_shutdown(); hp_env_shutdown(); return -1; }
    snprintf(configured_ini, len, "%s%s", defaults, ini ? ini : "");
    hp_sapi_module.ini_entries = configured_ini;
    hp_sapi_module.php_ini_ignore_cwd = 1;
    if (hp_sapi_module.startup(&hp_sapi_module) == FAILURE) {
        sapi_shutdown(); tsrm_shutdown(); hp_env_shutdown();
        free(configured_ini); configured_ini = NULL;
        return -1;
    }
    override_opcache();
    capture_opcache_scalars();
    sapi_register_default_post_reader(lazy_default_post_reader);
    engine_started = true;
    return 0;
}

void hp_sapi_shutdown(void)
{
    if (!engine_started) return;
    hp_sapi_module.shutdown(&hp_sapi_module);
    sapi_shutdown();
    tsrm_shutdown();
    hp_env_shutdown();
    free(configured_ini); configured_ini = NULL;
    hp_sapi_module.ini_entries = NULL;
    engine_started = false;
}

void hp_sapi_thread_init(void)
{
    (void)ts_resource(0);
    initialize_opcache_scalars();
    local_slot.interrupt = &EG(vm_interrupt);
    local_slot.timed_out = &EG(timed_out);
    pthread_mutex_lock(&slots_mutex);
    local_slot.next = slots;
    slots = &local_slot;
    pthread_mutex_unlock(&slots_mutex);
}

void hp_sapi_thread_shutdown(void)
{
    pthread_mutex_lock(&slots_mutex);
    hp_php_slot_t **link = &slots;
    while (*link && *link != &local_slot) link = &(*link)->next;
    if (*link) *link = local_slot.next;
    pthread_mutex_unlock(&slots_mutex);
    ts_free_thread();
}

static int execute_script(hp_request_t *request, const char *file)
{
    int status = 0;
    bool started = false;
    install_context(request);
    zend_first_try {
        if (php_request_startup() == FAILURE) zend_bailout();
        started = true;
        partial_active = true;
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
        zend_destroy_file_handle(&handle);
        status = EG(exit_status);
    } zend_catch {
        status = 255;
    } zend_end_try();
    if (started) {
        /* If a callback exited/fataled, full shutdown cleans its active partial
         * request as well as the persistent script. PHP catches normal exit. */
        zend_try { hp_env_script_reset(); php_request_shutdown(NULL); }
        zend_catch { status = 255; }
        zend_end_try();
        hp_env_after_shutdown();
    }
    if (is_real_request()) {
        pthread_mutex_lock(&current_request->mutex);
        bool headers_ready = current_request->headers_ready;
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
    return status;
}

int hp_sapi_execute(hp_request_t *request)
{
    current_group = NULL;
    return execute_script(request, request->script);
}

int hp_sapi_worker(hp_worker_group_t *group, const char *file)
{
    current_group = group;
    worker_bootstrapped = false;
    worker_requests = 0;
    memset(&dummy_request, 0, sizeof(dummy_request));
    dummy_request.method = "GET";
    dummy_request.uri = dummy_request.script_name = "/";
    dummy_request.query = "";
    dummy_request.script = (char *)file;
    const hp_worker_config_t *config = hp_worker_group_config(group);
    dummy_request.document_root = config->root;
    dummy_request.env = config->env;
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    dummy_request.request_time = wall.tv_sec + wall.tv_nsec / 1e9;
    int status = execute_script(&dummy_request, file);
    current_group = NULL;
    return status || !worker_bootstrapped ? 255 : 0;
}
