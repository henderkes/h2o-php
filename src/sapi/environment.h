#ifndef H2O_PHP_ENVIRONMENT_H
#define H2O_PHP_ENVIRONMENT_H
#include <php.h>
int hp_env_init(void);
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
#endif
