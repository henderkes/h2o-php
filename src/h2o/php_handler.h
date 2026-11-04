#ifndef HP_HANDLER_H
#define HP_HANDLER_H
#include "h2o.h"
#include "h2o_php.h"
#include <regex.h>
typedef struct hp_route_match {
    atomic_uint refs;
    bool has_path;
    regex_t path;
    H2O_VECTOR(char *) globs;
    H2O_VECTOR(char *) methods;
    H2O_VECTOR(struct { char *name; regex_t value; }) headers;
} hp_route_match_t;
typedef struct hp_worker_routes {
    atomic_uint refs;
    H2O_VECTOR(struct { hp_worker_group_t *group; hp_route_match_t *match; }) entries;
} hp_worker_routes_t;
typedef enum { HP_ROUTE_AUTO, HP_ROUTE_SERVER, HP_ROUTE_DIRECT, HP_ROUTE_WORKER } hp_route_mode_t;
enum { HP_METRICS_OFF, HP_METRICS_JSON, HP_METRICS_PROMETHEUS };
typedef struct hp_route_config {
    char *root, *index_file, *script, *script_name, *name;
    char **split_paths;
    size_t num_split_paths;
    hp_header_t *env;
    hp_worker_group_t *group;
    hp_worker_routes_t *workers;
    hp_route_match_t *match;
    hp_route_mode_t mode;
    uint64_t timeout_ms, body_timeout_ms, response_timeout_ms;
    int metrics, restart, resolve_root_symlink, split_path, file_server, disabled, declared;
    bool timeout_set;
    unsigned specified_options;
} hp_route_config_t;
void hp_h2o_register(h2o_pathconf_t *, const hp_route_config_t *);
int hp_h2o_is_worker(h2o_handler_t *);
void hp_h2o_worker_add_static_root(h2o_handler_t *, const char *);
void hp_route_match_release(hp_route_match_t *);
void hp_worker_routes_release(hp_worker_routes_t *);
void hp_route_config_clone(hp_route_config_t *, const hp_route_config_t *);
void hp_route_config_dispose(hp_route_config_t *);
const hp_route_config_t *hp_h2o_handler_config(h2o_handler_t *);
#endif
