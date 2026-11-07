#ifndef HP_PHP_METRICS_H
#define HP_PHP_METRICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hp_php_metric_worker hp_php_metric_worker_t;

/* Embedded in hp_request. Caller holds the request mutex for every *_locked
 * operation; the collector never takes a scheduler/request/budget mutex. */
typedef struct hp_php_metric_request {
    hp_php_metric_worker_t *worker;
    uint64_t origin_ns;
    bool begun, queued, dispatched, ended;
} hp_php_metric_request_t;

typedef enum {
    HP_PHP_METRIC_CRASH,
    HP_PHP_METRIC_RESTART,
    HP_PHP_METRIC_BOOT_FAILURE
} hp_php_metric_stop_t;

/* Cold configuration/lifecycle calls. Handles and copied labels have process
 * lifetime. No collector/label is destroyed while callbacks can still refer to
 * it. Start is supported once per process, including a failed startup. */
hp_php_metric_worker_t *hp_php_metrics_worker_register(const char *label);
int hp_php_metrics_start(unsigned initial_total, bool collect);
void hp_php_metrics_shutdown(void);
void hp_php_metrics_opcache_available(bool available);
bool hp_php_metrics_owner_process(void);

/* Hot events are atomic-only on the supported 64-bit target. Existing PHP fork
 * wrapper child marking is checked before any event. No Zend calls/bailouts. */
void hp_php_metrics_worker_start(hp_php_metric_worker_t *);
void hp_php_metrics_worker_ready(hp_php_metric_worker_t *);
void hp_php_metrics_worker_stop(hp_php_metric_worker_t *, hp_php_metric_stop_t);
void hp_php_metrics_request_begin_locked(hp_php_metric_request_t *, hp_php_metric_worker_t *, uint64_t origin_ns);
void hp_php_metrics_request_queued_locked(hp_php_metric_request_t *);
void hp_php_metrics_request_dispatched_locked(hp_php_metric_request_t *);
/* A queued cancellation closes immediately. A dispatched cancellation waits
 * for response/context finish, matching upstream's post-dispatch fc.done wait. */
void hp_php_metrics_request_cancel_locked(hp_php_metric_request_t *, uint64_t now_ns);
void hp_php_metrics_request_end_locked(hp_php_metric_request_t *, uint64_t now_ns);
int64_t hp_php_metrics_queue_depth(hp_php_metric_worker_t *); /* NULL: regular */

/* Cold real OPcache scheduling callback: PID/child guard covers child atfork
 * handlers before the PHP fork wrapper has set its child flag. */
void hp_php_metrics_opcache_restart(int reason);

/* Caller owns *output and frees it only after its HTTP adapter has copied the
 * bytes. Disabled collection returns an empty exposition successfully.
 * Returns -1 with *output=NULL on failure/inactive/child. Atomic values
 * are individually sampled; this does not promise a transactional scrape. */
int hp_php_metrics_prometheus(char **output, size_t *length);

#endif
