#ifndef HP_HANDLER_H
#define HP_HANDLER_H
#include "h2o.h"
#include "h2o_php.h"
typedef struct hp_route_config {
    char *root, *front_controller;
    hp_header_t *env;
    hp_worker_group_t *group;
    uint64_t timeout_ms;
    int metrics, restart, resolve_root_symlink, split_path;
} hp_route_config_t;
void hp_h2o_register(h2o_pathconf_t *, const hp_route_config_t *);
#endif
