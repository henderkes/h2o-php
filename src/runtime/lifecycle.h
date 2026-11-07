#ifndef HP_LIFECYCLE_H
#define HP_LIFECYCLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <poll.h>

/* Owned immutable input. Serving generations retain the exact submitted YAML,
 * not a filename whose contents can change during startup or rollback. */
typedef struct hp_lifecycle_config {
    unsigned refs;
    char *yaml, *path, **argv;
    size_t length;
    uint64_t startup_ms, shutdown_ms;
} hp_lifecycle_config_t;

/* Returns -1 for commands handled by the existing CLI. */
int hp_lifecycle_command(int argc, char **argv);
struct st_yoml_t;
char *hp_lifecycle_resolve_yaml(const struct st_yoml_t *document);
int hp_lifecycle_write_probe(char *const argv[], uint64_t startup_ms, uint64_t shutdown_ms,
                             const char *yaml);
/* Validation runs in the existing separate, pre-thread configuration probe.
 * step never waits; cancel retains ownership until step observes child exit. */
typedef struct hp_lifecycle_validation hp_lifecycle_validation_t;
hp_lifecycle_validation_t *hp_lifecycle_validation_begin(const char *executable, const char *yaml,
                                                         size_t length, char **error);
int hp_lifecycle_validation_step(hp_lifecycle_validation_t *validation, hp_lifecycle_config_t **config, char **error);
uint64_t hp_lifecycle_validation_deadline(const hp_lifecycle_validation_t *validation);
void hp_lifecycle_validation_cancel(hp_lifecycle_validation_t *validation, const char *message);
void hp_lifecycle_validation_destroy(hp_lifecycle_validation_t *validation);
hp_lifecycle_config_t *hp_lifecycle_load(const char *executable, const char *yaml, size_t length, char **error,
                                        bool (*cancelled)(void));
void hp_lifecycle_retain(hp_lifecycle_config_t *config);
void hp_lifecycle_release(hp_lifecycle_config_t *config);
/* Local control transport: bounded, versioned messages; socket access is 0600. */
#define HP_LIFECYCLE_CLIENT_LIMIT 32
/* The supervisor owns the manager; request/reply handles remain valid until
 * reply is called. Receiving and sending never wait for a peer. */
typedef struct hp_lifecycle_control hp_lifecycle_control_t;
typedef struct hp_lifecycle_client hp_lifecycle_client_t;
hp_lifecycle_control_t *hp_lifecycle_control_create(void);
void hp_lifecycle_control_destroy(hp_lifecycle_control_t *control);
bool hp_lifecycle_control_has_capacity(const hp_lifecycle_control_t *control);
void hp_lifecycle_control_accept(hp_lifecycle_control_t *control, int listener, uint64_t now);
void hp_lifecycle_control_poll(hp_lifecycle_control_t *control, struct pollfd *fds, uint64_t *next);
void hp_lifecycle_control_process(hp_lifecycle_control_t *control, const struct pollfd *fds, uint64_t now);
hp_lifecycle_client_t *hp_lifecycle_control_request(hp_lifecycle_control_t *control, unsigned *command,
                                                   bool *force, char **yaml, size_t *length);
void hp_lifecycle_reply(hp_lifecycle_client_t *client, int status, const char *message);
int hp_native_lifecycle_run(hp_lifecycle_config_t *config, int control_fd);

#define HP_LIFECYCLE_LOAD 1U
#define HP_LIFECYCLE_STOP 2U
#endif
