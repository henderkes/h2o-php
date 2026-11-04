/* Explicit ownership of descriptors inherited by a PHP-initiated fork.
 * Registration spans descriptor creation, and removal spans close, under one
 * gate. Therefore an FD cannot be recycled into an application-owned handle
 * between the fork snapshot and the child's close. No /proc or FD scan is used.
 */
#include "host_fds.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t descriptor_gate = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *descriptor_bits;
static size_t descriptor_words;
static _Thread_local unsigned gate_depth;
static bool child_detached;

void hp_host_fds_begin(void)
{
    if (child_detached) return;
    if (gate_depth++ == 0 && pthread_mutex_lock(&descriptor_gate)) abort();
}

void hp_host_fds_end(void)
{
    if (child_detached) return;
    if (!gate_depth) abort();
    if (--gate_depth == 0 && pthread_mutex_unlock(&descriptor_gate)) abort();
}

void hp_host_fd_register_locked(int fd)
{
    if (fd < 0 || child_detached) return;
    if (!gate_depth) abort();
    size_t word = (unsigned)fd / 64;
    if (word >= descriptor_words) {
        size_t count = descriptor_words ? descriptor_words : 64;
        while (count <= word) {
            if (count > SIZE_MAX / (2 * sizeof(*descriptor_bits))) abort();
            count *= 2;
        }
        uint64_t *expanded = realloc(descriptor_bits, count * sizeof(*expanded));
        if (!expanded) abort(); /* Same allocation policy as H2O's core. */
        memset(expanded + descriptor_words, 0, (count - descriptor_words) * sizeof(*expanded));
        descriptor_bits = expanded; descriptor_words = count;
    }
    descriptor_bits[word] |= UINT64_C(1) << ((unsigned)fd % 64);
}

int hp_host_fd_close(int fd)
{
    /* A discarded host object must not close a number reused by an application
     * stream after the fork snapshot was closed. No H2O loop runs in a child. */
    if (child_detached) return 0;
    if (fd < 0) return close(fd);
    hp_host_fds_begin();
    size_t word = (unsigned)fd / 64;
    if (word < descriptor_words)
        descriptor_bits[word] &= ~(UINT64_C(1) << ((unsigned)fd % 64));
    /* Do not retry close after EINTR: Linux has already released the number. */
    int result = close(fd), saved_errno = errno;
    hp_host_fds_end(); errno = saved_errno;
    return result;
}

int hp_host_fds_fork_prepare(uint64_t timeout_ms)
{
    if (child_detached) return 0;
    if (gate_depth) { errno = EDEADLK; return -1; }
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline)) return -1;
    /* Bound the conversion, including the nanosecond carry. */
    if (timeout_ms > UINT_MAX) timeout_ms = UINT_MAX;
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
    int result = pthread_mutex_timedlock(&descriptor_gate, &deadline);
    if (result) { errno = result; return -1; }
    gate_depth = 1;
    return 0;
}

void hp_host_fds_fork_parent(void)
{
    if (!child_detached) hp_host_fds_end();
}

void hp_host_fds_fork_child(void)
{
    if (child_detached) return;
    /* Only close is called in the inherited snapshot. No allocator, H2O,
     * mutex, socket shutdown, or PHP callback runs here. Closing a duplicate
     * never shuts down the parent's shared TCP connection. */
    for (size_t word = 0; word < descriptor_words; ++word) {
        uint64_t bits = descriptor_bits[word];
        while (bits) {
            unsigned bit = (unsigned)__builtin_ctzll(bits);
            size_t fd = word * 64 + bit;
            if (fd <= INT_MAX) (void)close((int)fd);
            bits &= bits - 1;
        }
    }
    child_detached = true;
    gate_depth = 0;
    /* The original allocation and locked mutex belong to the discarded host
     * graph; leave them untouched until this process exits. */
    descriptor_bits = NULL; descriptor_words = 0;
}
