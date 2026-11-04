#ifndef HP_LOG_H
#define HP_LOG_H

#include <stddef.h>
#include <stdint.h>

/* A complete JSON record is serialized before acquiring the backend lock;
 * Zend bailout must never cross these functions while they hold that lock. */
void hp_log_json(const char *record, size_t length);
void hp_log_message(const char *component, const char *message, int severity, uint64_t request_id);

#endif
