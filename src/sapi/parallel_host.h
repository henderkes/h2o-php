#ifndef H2O_PHP_PARALLEL_HOST_H
#define H2O_PHP_PARALLEL_HOST_H
#include <stdint.h>
#include <stddef.h>

/* Versioned optional host integration, shared with our pinned parallel patch.
 * Every opaque context is C-owned; no Zend allocation crosses PHP heaps. */
#define HP_PARALLEL_HOST_ABI 2
typedef struct hp_parallel_host_hooks {
    uint32_t abi;
    size_t size;
    void *(*capture)(void);
    void (*release)(void *);
    void (*enter)(void *);
    void (*leave)(void *);
    void (*thread_ready)(void *);
    void (*thread_stopping)(void *);
    void (*thread_done)(void *);
    void (*thread_joined)(void *);
    void (*thread_idle)(void *);
    void (*thread_busy)(void *);
    int (*native_output)(void);
    void *(*spawn_prepare)(void *);
    void (*spawn_parent)(void *, int);
} hp_parallel_host_hooks_t;
typedef int (*hp_parallel_set_host_hooks_t)(const hp_parallel_host_hooks_t *);
#endif
