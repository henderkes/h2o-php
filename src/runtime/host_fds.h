#ifndef H2O_PHP_HOST_FDS_H
#define H2O_PHP_HOST_FDS_H
#include <stdint.h>

/* Host-owned descriptors only. Application streams, sockets and database
 * handles never enter this registry. Nesting is allowed on one pthread. */
void hp_host_fds_begin(void);
void hp_host_fds_end(void);
void hp_host_fd_register_locked(int fd);
int hp_host_fd_close(int fd);
/* Success holds the mutation gate until the matching parent/child callback. */
int hp_host_fds_fork_prepare(uint64_t timeout_ms);
void hp_host_fds_fork_parent(void);
void hp_host_fds_fork_child(void);
#endif
