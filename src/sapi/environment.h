#ifndef H2O_PHP_ENVIRONMENT_H
#define H2O_PHP_ENVIRONMENT_H
#include <php.h>
#include "h2o_php.h"
int hp_env_init(void);
/* On the PHP startup thread, after sapi_startup and before module startup. */
int hp_env_startup(void);
void hp_env_shutdown(void);
void hp_env_set(const char *name, const char *value);
void hp_env_bootstrapped(void);
void hp_env_request_clear(void);
void hp_env_script_reset(void);
void hp_env_after_shutdown(void);
void hp_env_import(zval *target);
void hp_env_server_import(zval *target);
char *hp_env_get(const char *name, size_t len);
void hp_env_override_functions(void);
/* Snapshot the calling interpreter's resolved environment into native memory.
 * The receiver copies it into its own Zend heap before consuming it. */
hp_header_t *hp_env_snapshot(void);
void hp_env_apply_snapshot(const hp_header_t *);
#endif
