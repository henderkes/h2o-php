#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "config_watch.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WATCH_SCAN_MS 100
#define WATCH_SETTLE_MS 200
#define CONFIG_LIMIT (16U * 1024U * 1024U)

struct hp_config_watch {
    char *path, **patterns;
    size_t count;
    uint64_t fingerprint, next_scan, settle_at;
    bool dirty, ready;
};

static uint64_t hash_bytes(uint64_t hash, const void *bytes, size_t length)
{
    const unsigned char *p = bytes;
    for (size_t i = 0; i < length; ++i) hash = (hash ^ p[i]) * UINT64_C(1099511628211);
    return hash;
}
static uint64_t hash_path(uint64_t hash, const char *path)
{
    struct stat st;
    hash = hash_bytes(hash, path, strlen(path) + 1);
    if (stat(path, &st)) return hash;
    uint64_t values[] = {(uint64_t)st.st_dev, (uint64_t)st.st_ino, (uint64_t)st.st_mode,
        (uint64_t)st.st_size, (uint64_t)st.st_mtim.tv_sec, (uint64_t)st.st_mtim.tv_nsec,
        (uint64_t)st.st_ctim.tv_sec, (uint64_t)st.st_ctim.tv_nsec};
    return hash_bytes(hash, values, sizeof(values));
}
static uint64_t snapshot(const hp_config_watch_t *watch)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < watch->count; ++i) {
        hash = hash_bytes(hash, watch->patterns[i], strlen(watch->patterns[i]) + 1);
        /* The startup filename is literal, even if it contains glob syntax. */
        if (!i) { hash = hash_path(hash, watch->path); continue; }
        glob_t paths = {0};
        int result = glob(watch->patterns[i], 0, NULL, &paths);
        if (!result) for (size_t j = 0; j < paths.gl_pathc; ++j) {
            hash = hash_path(hash, paths.gl_pathv[j]);
        }
        globfree(&paths);
    }
    return hash;
}
static char *absolute_path(const char *path)
{
    if (path[0] == '/') return strdup(path);
    char cwd[PATH_MAX], *absolute = NULL;
    if (!getcwd(cwd, sizeof(cwd)) || asprintf(&absolute, "%s/%s", cwd, path) < 0) return NULL;
    return absolute;
}
hp_config_watch_t *hp_config_watch_create(const char *config, const char *const *patterns, size_t count)
{
    hp_config_watch_t *watch = calloc(1, sizeof(*watch));
    if (!watch) return NULL;
    watch->count = count + 1;
    watch->path = absolute_path(config);
    watch->patterns = calloc(watch->count, sizeof(*watch->patterns));
    if (!watch->path || !watch->patterns) goto Error;
    watch->patterns[0] = strdup(watch->path);
    if (!watch->patterns[0]) goto Error;
    for (size_t i = 0; i < count; ++i) {
        watch->patterns[i + 1] = absolute_path(patterns[i]);
        if (!watch->patterns[i + 1]) goto Error;
    }
    watch->fingerprint = snapshot(watch);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    watch->next_scan = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + WATCH_SCAN_MS;
    return watch;
Error:
    hp_config_watch_destroy(watch); return NULL;
}
void hp_config_watch_destroy(hp_config_watch_t *watch)
{
    if (!watch) return;
    if (watch->patterns) for (size_t i = 0; i < watch->count; ++i) free(watch->patterns[i]);
    free(watch->patterns); free(watch->path); free(watch);
}
void hp_config_watch_step(hp_config_watch_t *watch, uint64_t now)
{
    if (!watch || now < watch->next_scan) return;
    uint64_t fingerprint = snapshot(watch);
    if (fingerprint != watch->fingerprint) {
        watch->fingerprint = fingerprint;
        watch->dirty = true; watch->ready = false;
        watch->settle_at = now + WATCH_SETTLE_MS;
    }
    watch->next_scan = now + WATCH_SCAN_MS;
    if (watch->dirty && now >= watch->settle_at) watch->ready = true;
}
uint64_t hp_config_watch_deadline(const hp_config_watch_t *watch) { return watch ? watch->next_scan : UINT64_MAX; }
bool hp_config_watch_ready(const hp_config_watch_t *watch) { return watch && watch->ready; }
const char *hp_config_watch_path(const hp_config_watch_t *watch) { return watch->path; }
char *hp_config_watch_read(hp_config_watch_t *watch, size_t *length)
{
    watch->dirty = watch->ready = false;
    int fd = open(watch->path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    char *yaml = NULL;
    if (fstat(fd, &st)) goto Error;
    if (!S_ISREG(st.st_mode)) { errno = EINVAL; goto Error; }
    if (st.st_size > CONFIG_LIMIT) { errno = EFBIG; goto Error; }
    size_t capacity = 4096, used = 0;
    yaml = malloc(capacity + 1);
    if (!yaml) goto Error;
    for (;;) {
        ssize_t n = read(fd, yaml + used, capacity - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto Error;
        if (!n) break;
        used += (size_t)n;
        if (used > CONFIG_LIMIT) { errno = EFBIG; goto Error; }
        if (used == capacity) {
            capacity *= 2;
            if (capacity > CONFIG_LIMIT + 1) capacity = CONFIG_LIMIT + 1;
            char *next = realloc(yaml, capacity + 1);
            if (!next) goto Error;
            yaml = next;
        }
    }
    close(fd); yaml[used] = 0; *length = used; return yaml;
Error:
    { int saved = errno; close(fd); free(yaml); errno = saved; return NULL; }
}
