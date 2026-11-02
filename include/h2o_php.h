#ifndef H2O_PHP_H
#define H2O_PHP_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct hp_request hp_request_t;
typedef struct hp_worker_group hp_worker_group_t;
typedef struct hp_header { char *name, *value; struct hp_header *next; } hp_header_t;
typedef struct hp_chunk { struct hp_chunk *next; size_t len, offset; char data[]; } hp_chunk_t;
typedef struct hp_info { struct hp_info *next; int status; hp_header_t *headers; } hp_info_t;
typedef enum { HP_CREATED, HP_QUEUED, HP_RUNNING, HP_RESPONDING, HP_COMPLETED, HP_CANCELLED, HP_TIMED_OUT, HP_FAILED } hp_request_state_t;
typedef enum { HP_WORKER_STARTING, HP_WORKER_BOOTSTRAPPING, HP_WORKER_READY, HP_WORKER_BUSY, HP_WORKER_DRAINING, HP_WORKER_RESTARTING, HP_WORKER_FAILED, HP_WORKER_STOPPED } hp_worker_state_t;
typedef struct hp_runtime_config {
    unsigned threads, max_threads, queue_limit, max_requests;
    size_t max_buffered_body, max_buffered_response, max_buffered_global;
    uint64_t max_wait_ms, request_timeout_ms, shutdown_timeout_ms;
    char *ini_entries;
} hp_runtime_config_t;
typedef struct hp_worker_config {
    char *file, *name, *root;
    unsigned num, max_requests, max_consecutive_failures;
    hp_header_t *env;
    char **watch; size_t num_watch;
} hp_worker_config_t;
struct hp_request {
    atomic_uint refs;
    pthread_mutex_t mutex;
    pthread_cond_t body_cond, output_cond;
    uint64_t id, created_ns, queued_ns, started_ns, deadline_ns;
    hp_request_state_t state;
    char *method, *uri, *query, *protocol, *script, *script_name, *path_info, *document_root;
    char *remote_addr, *remote_port, *server_name, *server_port, *content_type, *cookie;
    int tls; size_t content_length; double request_time;
    hp_header_t *headers, *env;
    hp_worker_group_t *group;
    hp_chunk_t *body_head, *body_tail, *output_head, *output_tail;
    size_t body_bytes, output_bytes, body_limit, output_limit;
    bool body_eof, cancelled, response_finished, headers_ready, headers_taken;
    int status;
    hp_header_t *response_headers;
    hp_info_t *info_head, *info_tail;
    void (*notify)(hp_request_t *);
    void *network_data, *notify_data;
    atomic_bool notification_pending;
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
ssize_t hp_request_body_read(hp_request_t *, void *, size_t);
/* PHP thread writes with bounded backpressure. */
ssize_t hp_response_write(hp_request_t *, const void *, size_t);
int hp_response_headers(hp_request_t *, int status, const hp_header_t *, bool informational);
void hp_response_finish(hp_request_t *);
/* caller owns returned chunk, released with hp_chunk_free after network send completes */
hp_chunk_t *hp_response_pop(hp_request_t *);
void hp_chunk_free(hp_chunk_t *);
hp_header_t *hp_headers_clone(const hp_header_t *);
void hp_headers_free(hp_header_t *);
int hp_header_add(hp_header_t **, const char *, const char *);
const char *hp_header_get(const hp_header_t *, const char *);
void hp_runtime_config_defaults(void);
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
void hp_metrics_json(char *, size_t);
size_t hp_bridge_buffered_bytes(void);
void hp_sapi_interrupt_request(hp_request_t *);
int hp_sapi_init(const char *, unsigned);
void hp_sapi_shutdown(void);
void hp_sapi_thread_init(void);
void hp_sapi_thread_shutdown(void);
int hp_sapi_execute(hp_request_t *);
int hp_sapi_worker(hp_worker_group_t *, const char *);
void h2o_php_register_configurator(void *globalconf);
void h2o_php_context_shutdown(void *context);
size_t h2o_php_context_pending(void *context);
#endif
