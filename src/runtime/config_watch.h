#ifndef HP_CONFIG_WATCH_H
#define HP_CONFIG_WATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hp_config_watch hp_config_watch_t;

/* Opt-in supervisor work only; no watcher runs in serving generations. */
hp_config_watch_t *hp_config_watch_create(const char *config, const char *const *patterns, size_t count);
void hp_config_watch_destroy(hp_config_watch_t *watch);
void hp_config_watch_step(hp_config_watch_t *watch, uint64_t now);
uint64_t hp_config_watch_deadline(const hp_config_watch_t *watch);
bool hp_config_watch_ready(const hp_config_watch_t *watch);
const char *hp_config_watch_path(const hp_config_watch_t *watch);
/* Consume one debounced change. A later edit can retry a failed read/reload. */
char *hp_config_watch_read(hp_config_watch_t *watch, size_t *length);

#endif
