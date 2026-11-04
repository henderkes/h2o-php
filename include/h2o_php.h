#ifndef H2O_PHP_H
#define H2O_PHP_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <sys/types.h>
#include "../src/observability/php_metrics.h"

typedef struct hp_request hp_request_t;
typedef struct hp_worker_group hp_worker_group_t;
typedef struct hp_header { char *name, *value; struct hp_header *next; } hp_header_t;
typedef struct hp_chunk { struct hp_chunk *next; size_t len, offset; char data[]; } hp_chunk_t;
typedef struct hp_info { struct hp_info *next; int status; hp_header_t *headers; } hp_info_t;
typedef enum { HP_CREATED, HP_QUEUED, HP_RUNNING, HP_RESPONDING, HP_COMPLETED, HP_CANCELLED, HP_TIMED_OUT, HP_FAILED } hp_request_state_t;
typedef enum { HP_WORKER_STARTING, HP_WORKER_BOOTSTRAPPING, HP_WORKER_READY, HP_WORKER_BUSY, HP_WORKER_DRAINING, HP_WORKER_RESTARTING, HP_WORKER_FAILED, HP_WORKER_STOPPED } hp_worker_state_t;
typedef enum { HP_THREAD_HEALTHY, HP_THREAD_UNHEALTHY } hp_thread_health_t;
typedef enum { HP_PHASE_NONE, HP_PHASE_STARTUP, HP_PHASE_EXECUTION, HP_PHASE_SHUTDOWN } hp_execution_phase_t;
typedef struct hp_execution_result {
    int exit_status;
    hp_thread_health_t health;
    hp_execution_phase_t phase;
} hp_execution_result_t;
typedef struct hp_runtime_config {
    unsigned threads, max_threads;
    unsigned queue_limit; /* 0: no configured pending-count cap; positive: global/pool shares. */
    unsigned max_requests;
    size_t max_buffered_body, max_buffered_response, max_buffered_global;
    uint64_t max_wait_ms, request_timeout_ms, shutdown_timeout_ms, max_idle_ms, startup_timeout_ms;
    char *ini_entries, *log_path;
    bool threads_explicit, max_threads_explicit, max_threads_auto;
} hp_runtime_config_t;
typedef struct hp_worker_config {
    char *file, *name, *root;
    char *metrics_name; /* Immutable Prom label; does not rename native config/log names. */
    unsigned num, max_requests, max_threads; /* num: minimum count; 0 defaults to H2O's CPU count */
    int64_t max_consecutive_failures; /* -1: unlimited; default6; Go amd64 int width. */
    uint64_t startup_timeout_ms;
    hp_header_t *env;
    char **watch; size_t num_watch;
} hp_worker_config_t;
struct hp_request {
    atomic_uint refs;
    pthread_mutex_t mutex;
    pthread_cond_t body_cond, output_cond;
    uint64_t id, created_ns, queued_ns, started_ns, deadline_ns;
    hp_request_state_t state;
    hp_php_metric_request_t php_metrics; /* Protected by this request mutex. */
    char *method, *uri, *query, *protocol, *script, *script_name, *path_info, *document_root;
    /* Immutable CGI metadata after queue publication. Normally aliases
     * script_name; explicit worker request paths can own a separate split
     * prefix. A computed NULL must remain distinct from dummy-context fallback
     * because NULL bypasses the PHP input filter while non-NULL empty does not. */
    char *document_uri;
    char *remote_addr, *remote_port, *server_name, *server_port, *content_type, *cookie;
    int tls; size_t content_length; double request_time;
    hp_header_t *headers, *env;
    hp_worker_group_t *group;
    hp_chunk_t *body_head, *body_tail, *output_head, *output_tail;
    /* output_bytes is queued payload plus producer reservations, NOT just
     * linked-list bytes. output_inflight is detached batch payload. Both are
     * request-mutex protected and their sum may not exceed output_limit. */
    size_t body_bytes, output_bytes, output_inflight, body_limit, output_limit;
    bool body_eof, body_producer_blocked, cancelled, response_finished, headers_ready, headers_taken;
    bool document_uri_set, document_uri_owned;
    /* Request mutex: owning loop has a detached output batch and must pop
     * again from proceed (or cancel/finish); false is published by empty pop. */
    bool output_consumer_active;
    /* Request mutex: explicit SAPI flush intent, monotonic for this request. */
    bool output_flush_requested;
    int status;
    hp_header_t *response_headers;
    hp_info_t *info_head, *info_tail;
    void (*notify)(hp_request_t *);
    void *network_data, *notify_data;
    atomic_bool notification_pending;
    /* Monotonic SC stop publication pairs with global waiter registration.
     * Wake registered waiters under request -> budget lock order. */
    atomic_bool budget_stopped;
    hp_request_t *queue_next, *active_next;
};
extern hp_runtime_config_t hp_config;
uint64_t hp_now_ns(void);
hp_request_t *hp_request_new(void);
void hp_request_retain(hp_request_t *);
void hp_request_release(hp_request_t *);
void hp_request_cancel(hp_request_t *);
void hp_request_notify(hp_request_t *);
/* network thread pushes input without blocking; -1 means buffer saturation */
int hp_request_body_push(hp_request_t *, const void *, size_t, bool eof);
/* Owning loop atomically arms a single refill wake above the body low-water
 * mark. False means it must retry now (or observe EOF/cancellation). */
bool hp_request_body_pause(hp_request_t *);
ssize_t hp_request_body_read(hp_request_t *, void *, size_t);
/* PHP thread writes with bounded backpressure. */
ssize_t hp_response_write(hp_request_t *, const void *, size_t);
/* Flush never waits for more bytes. An active loop consumer already owns
 * the next send/proceed continuation; idle consumers are woken. */
void hp_response_flush_notify(hp_request_t *);
int hp_response_headers(hp_request_t *, int status, const hp_header_t *, bool informational);
void hp_response_finish(hp_request_t *);
/* Legacy diagnostic pop releases per-request queue credit immediately and
 * returns one chunk for hp_chunk_free. HTTP generators use the batch API. */
hp_chunk_t *hp_response_pop(hp_request_t *);
/* Detach only currently queued chunks; never wait to accumulate a batch.
 * This transfers payload from output_bytes to output_inflight without freeing
 * per-request/global credit. Caller retains the request through send completion
 * and releases each returned list with hp_response_release_chunks, NOT the
 * generic hp_chunk(s)_free APIs. The consumer must pop again after send or
 * terminate the request; an empty pop atomically arms producer notifications.
 * finished requires published finish and no queue. */
hp_chunk_t *hp_response_pop_batch(hp_request_t *, size_t max_chunks, size_t max_bytes, bool *finished);
/* Owning loop only, after proceed/stop or a bodyless-response discard. */
void hp_response_release_chunks(hp_request_t *, hp_chunk_t *);
void hp_chunk_free(hp_chunk_t *);
void hp_chunks_free(hp_chunk_t *);
hp_header_t *hp_headers_clone(const hp_header_t *);
void hp_headers_free(hp_header_t *);
int hp_header_add(hp_header_t **, const char *, const char *);
const char *hp_header_get(const hp_header_t *, const char *);
void hp_runtime_config_defaults(void);
int hp_runtime_config_prepare(void);
void hp_runtime_each_worker(void (*)(const hp_worker_config_t *, void *), void *);
hp_worker_group_t *hp_worker_group_create(const hp_worker_config_t *);
const hp_worker_config_t *hp_worker_group_config(hp_worker_group_t *);
int hp_runtime_start(void);
void hp_runtime_begin_shutdown(void);
/* Request controller shutdown without blocking an H2O event loop. */
void hp_runtime_request_stop(void);
bool hp_runtime_stopped(void);
void hp_runtime_stop(void);
int hp_runtime_submit(hp_request_t *); /* retains on success; -1 saturated/draining */
/* worker next returns scheduler-owned reference; done releases it */
hp_request_t *hp_worker_next(hp_worker_group_t *);
void hp_runtime_request_done(hp_request_t *);
void hp_worker_ready(hp_worker_group_t *);
bool hp_worker_should_restart(hp_worker_group_t *);
void hp_runtime_restart_workers(void);
/* Pause other scheduler execution before a PHP-initiated fork. Token is the
 * calling scheduler slot; success holds the fork gate until parent/child exit. */
int hp_runtime_fork_prepare(void *owner_token, uint64_t timeout_ms);
void hp_runtime_fork_parent(void);
void hp_runtime_fork_child(void);
void hp_runtime_set_cpu_sampler(double (*sample)(uint64_t, void *), void *data);
void hp_metrics_json(char *, size_t);
size_t hp_bridge_buffered_bytes(void);
void hp_sapi_interrupt_request(hp_request_t *);
/* Stable scheduler token identifies one PHP pthread, including bootstrap. */
void hp_sapi_set_worker_token(void *);
void hp_sapi_interrupt_worker(void *);
void *hp_sapi_worker_token(void);
unsigned hp_sapi_aux_threads(void);
int64_t hp_sapi_memory_limit(void);
int hp_sapi_fork_prepare(uint64_t);
void hp_sapi_fork_parent(void);
void hp_sapi_fork_child(void);
bool hp_process_is_fork_child(void);
int hp_process_start(void);
void hp_process_stop(void);
void hp_process_set_signal(int, void (*)(int));
int hp_pcntl_install(void);
void hp_pcntl_finalize(void);
void hp_pcntl_context_enter(void);
void hp_pcntl_context_leave(void);
void hp_pcntl_spawn_prepare(sigset_t *);
void hp_pcntl_spawn_restore(const sigset_t *);
int hp_php_cli_main(int, char **);
bool hp_php_cli_should_dispatch(int, char **);
int hp_log_open(const char *);
void hp_log_close(void);
int hp_sapi_init(const char *, unsigned);
void hp_sapi_shutdown(void);
void hp_sapi_thread_init(void);
void hp_sapi_thread_shutdown(void);
hp_execution_result_t hp_sapi_execute(hp_request_t *);
hp_execution_result_t hp_sapi_worker(hp_worker_group_t *, const char *);
void h2o_php_register_configurator(void *globalconf);
void h2o_php_context_shutdown(void *context);
size_t h2o_php_context_pending(void *context);
#endif
