#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cpu_budget.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#endif

static pthread_once_t cpu_once = PTHREAD_ONCE_INIT;
static hp_cpu_budget_info_t cpu_info;

/* Match Go ParseInt(base10,bits32): optional sign, ASCII digits, whole input.
 * Do not use atoi/strtol: whitespace and partial strings must not override.
 * Even INT32_MAX is only an integer here; pool-budget validation remains at
 * the caller, before allocating PHP threads. */
static hp_cpu_gomax_state_t parse_gomaxprocs(const char *value, unsigned *positive)
{
    *positive = 0;
    if (!value) return HP_CPU_GOMAX_ABSENT;
    if (!*value) return HP_CPU_GOMAX_EMPTY;
    const unsigned char *p = (const unsigned char *)value;
    bool negative = false;
    if (*p == '+' || *p == '-') { negative = *p == '-'; ++p; }
    if (*p < '0' || *p > '9') return HP_CPU_GOMAX_INVALID;
    const uint32_t limit = negative ? UINT32_C(2147483648) : UINT32_C(2147483647);
    uint32_t magnitude = 0;
    do {
        unsigned digit = *p - '0';
        if (magnitude > (limit - digit) / 10) return HP_CPU_GOMAX_INVALID;
        magnitude = magnitude * 10 + digit;
        ++p;
    } while (*p >= '0' && *p <= '9');
    if (*p) return HP_CPU_GOMAX_INVALID;
    if (negative || !magnitude) return HP_CPU_GOMAX_NONPOSITIVE;
    *positive = (unsigned)magnitude;
    return HP_CPU_GOMAX_POSITIVE;
}

static unsigned discover_cpus(hp_cpu_budget_info_t *info)
{
#if defined(__linux__) && defined(CPU_ALLOC) && defined(CPU_ALLOC_SIZE) && defined(CPU_FREE) && defined(CPU_ZERO_S) && defined(CPU_COUNT_S)
    /* Installed Go 1.27 getCPUCount passes an 8192-byte affinity buffer. Grow
     * checked GNU masks on EINVAL up to the same 65536-CPU bound. */
    const size_t limit = 65536;
    size_t bits = CPU_SETSIZE;
    if (!bits || bits > limit) bits = limit;
    for (;;) {
        size_t bytes = CPU_ALLOC_SIZE(bits);
        if (!bytes || bytes > 8192) { info->affinity_error = EOVERFLOW; break; }
        cpu_set_t *mask = CPU_ALLOC(bits);
        if (!mask) { info->affinity_error = ENOMEM; break; }
        CPU_ZERO_S(bytes, mask);
        int result = sched_getaffinity(0, bytes, mask);
        int error = result == 0 ? 0 : errno;
        int count = result == 0 ? CPU_COUNT_S(bytes, mask) : 0;
        CPU_FREE(mask);
        if (result == 0 && count > 0) {
            info->discovery = HP_CPU_DISCOVERY_LINUX_AFFINITY;
            return (unsigned)count;
        }
        if (result == 0) { info->affinity_error = 0; break; }
        if (error != EINVAL || bits == limit) { info->affinity_error = error; break; }
        bits = bits > limit / 2 ? limit : bits * 2;
    }
    /* Like the inspected Go affinity path: failed/empty mask means one,
     * never an online-CPU fallback that ignores a restricted affinity. */
    info->discovery = HP_CPU_DISCOVERY_FALLBACK_ONE;
    return 1;
#else
    /* POSIX platform fallback only; non-Linux affinity semantics unqualified. */
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    info->affinity_error = ENOSYS;
    if (online > 0) {
        info->discovery = HP_CPU_DISCOVERY_PLATFORM_ONLINE;
        return (unsigned long)online > INT32_MAX ? INT32_MAX : (unsigned)online;
    }
    info->discovery = HP_CPU_DISCOVERY_FALLBACK_ONE;
    return 1;
#endif
}

/* Stable startup policy only. The complete cached parser reference is Go
 * 1.26.5; the installed target is Go1.27.3. Do not present malformed parser
 * edges or asynchronous runtime updates as verified Go1.27 equivalence. */
static bool signed_decimal(const char *p, size_t n, unsigned bits, int64_t *out)
{
    if (!n) return false;
    bool negative = *p == '-';
    if (*p == '-' || *p == '+') { ++p; --n; }
    if (!n) return false;
    uint64_t limit = bits == 32 ? (negative ? UINT64_C(2147483648) : UINT64_C(2147483647))
                               : (negative ? UINT64_C(9223372036854775808) : UINT64_C(9223372036854775807));
    uint64_t value = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
        unsigned digit = (unsigned)(p[i] - '0');
        if (value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    if (negative && value == UINT64_C(9223372036854775808)) *out = INT64_MIN;
    else *out = negative ? -(int64_t)value : (int64_t)value;
    return true;
}
static void parse_cpu_godebug(const char *text, int *container, int *updates)
{
    /* These are the actual a83 executable's compiled defaults, not a claim
     * about every Go module's go directive or //go:debug setting. */
    *container = *updates = 1;
    while (text && *text) {
        const char *end = strchr(text, ',');
        if (!end) end = text + strlen(text);
        const char *eq = memchr(text, '=', (size_t)(end - text));
        int64_t value;
        if (eq && signed_decimal(eq + 1, (size_t)(end - eq - 1), 32, &value)) {
            size_t n = (size_t)(eq - text);
            if (n == 17 && !memcmp(text, "containermaxprocs", n)) *container = (int)value;
            else if (n == 14 && !memcmp(text, "updatemaxprocs", n)) *updates = (int)value;
        }
        text = *end ? end + 1 : end;
    }
}

#if defined(__linux__)
enum { CPU_PATH_CAP = 4096, CPU_LINE_CAP = 65536 };
typedef struct { int fd; size_t at, size; unsigned char bytes[4096]; } cpu_lines_t;
typedef struct { unsigned version; int quota_fd, period_fd; } cpu_quota_fds_t;
/* Retained exactly once; immutable after startup, CLOEXEC, no destructor or
 * inherited-lock cleanup requirement. There is deliberately no updater. */
static cpu_quota_fds_t raw_quota_fds = {0, -1, -1};

static void quota_error(hp_cpu_quota_info_t *q, hp_cpu_quota_state_t state, int error)
{
    q->state = state; q->error = error;
}
static int open_lines(cpu_lines_t *r, const char *path)
{
    memset(r, 0, sizeof(*r));
    r->fd = open(path, O_RDONLY | O_CLOEXEC);
    return r->fd < 0 ? -1 : 0;
}
/* One bounded line, not a bounded whole proc file. Consume the entire line
 * even when its irrelevant super-options exceed the parsing buffer. */
static int next_line(cpu_lines_t *r, char *line, bool *oversized, bool *newline)
{
    size_t n = 0;
    bool any = false;
    *oversized = *newline = false;
    for (;;) {
        if (r->at == r->size) {
            ssize_t got;
            do { got = read(r->fd, r->bytes, sizeof(r->bytes)); } while (got < 0 && errno == EINTR);
            if (got < 0) return -1;
            if (!got) { line[n] = 0; return any ? 1 : 0; }
            r->at = 0; r->size = (size_t)got;
        }
        unsigned char c = r->bytes[r->at++]; any = true;
        if (c == '\n') { *newline = true; line[n] = 0; return 1; }
        if (!c) { errno = EINVAL; return -1; }
        if (n < CPU_LINE_CAP - 1) line[n++] = (char)c;
        else *oversized = true;
    }
}
static bool cpu_list_contains(const char *list, const char *name)
{
    size_t length = strlen(name);
    while (*list) {
        const char *end = strchr(list, ',');
        if (!end) end = list + strlen(list);
        if ((size_t)(end - list) == length && !memcmp(list, name, length)) return true;
        list = *end ? end + 1 : end;
    }
    return false;
}
static bool copy_cpu_path(char *out, const char *path)
{
    size_t n = strlen(path);
    if (!n || path[0] != '/') { errno = EINVAL; return false; }
    if (n >= CPU_PATH_CAP) { errno = ENAMETOOLONG; return false; }
    memcpy(out, path, n + 1); return true;
}
static bool path_prefix(const char *path, const char *root)
{
    size_t n = strlen(root);
    return n == 1 && root[0] == '/' ? true :
        !strncmp(path, root, n) && (path[n] == 0 || path[n] == '/');
}
static bool unescape_cpu_path(char *text)
{
    char *out = text;
    while (*text) {
        if (*text != '\\') { *out++ = *text++; continue; }
        if (text[1] < '0' || text[1] > '7' || !text[2] || text[2] < '0' || text[2] > '7' ||
            !text[3] || text[3] < '0' || text[3] > '7') { errno = EINVAL; return false; }
        unsigned c = (unsigned)(text[1] - '0') * 64 + (unsigned)(text[2] - '0') * 8 + (unsigned)(text[3] - '0');
        if (!c || c > 255) { errno = EINVAL; return false; }
        *out++ = (char)c; text += 4;
    }
    *out = 0; return true;
}
/* Lexical path.Clean model used only by the Caddy lookup. Raw Go's mount
 * namespace prefix rules deliberately use the original paths instead. */
static bool clean_cpu_path(char *out, const char *path)
{
    if (!path || path[0] != '/') { errno = EINVAL; return false; }
    size_t used = 1; out[0] = '/'; out[1] = 0;
    while (*path) {
        while (*path == '/') ++path;
        const char *end = path;
        while (*end && *end != '/') ++end;
        size_t n = (size_t)(end - path);
        if (!n) break;
        if (n == 2 && path[0] == '.' && path[1] == '.') {
            while (used > 1 && out[used - 1] != '/') --used;
            if (used > 1) --used;
        } else if (!(n == 1 && path[0] == '.')) {
            size_t sep = used > 1;
            if (n >= CPU_PATH_CAP - used - sep) { errno = ENAMETOOLONG; return false; }
            if (sep) out[used++] = '/';
            memcpy(out + used, path, n); used += n;
        }
        out[used] = 0; path = end;
    }
    return true;
}
static bool join_cpu_path(char *out, const char *mount, const char *relative, bool clean)
{
    char joined[CPU_PATH_CAP];
    size_t a = strlen(mount), b = strlen(relative);
    bool sep = a && mount[a - 1] != '/' && b && relative[0] != '/';
    if (a >= sizeof(joined) || b >= sizeof(joined) - a - (size_t)sep) { errno = ENAMETOOLONG; return false; }
    memcpy(joined, mount, a);
    if (sep) joined[a++] = '/';
    memcpy(joined + a, relative, b + 1);
    if (clean) return clean_cpu_path(out, joined);
    return copy_cpu_path(out, joined);
}
static bool cpu_memberships(bool raw, char *v1, char *v2, hp_cpu_quota_info_t *q)
{
    cpu_lines_t r;
    if (open_lines(&r, "/proc/self/cgroup")) { quota_error(q, errno == ENOENT ? HP_CPU_QUOTA_NONE : HP_CPU_QUOTA_ERROR, errno); return false; }
    char line[CPU_LINE_CAP]; bool oversized, newline; int result;
    v1[0] = v2[0] = 0;
    while ((result = next_line(&r, line, &oversized, &newline)) > 0) {
        if (oversized || (raw && !newline)) { errno = oversized ? EOVERFLOW : EINVAL; goto Bad; }
        if (!raw) { size_t n = strlen(line); if (n && line[n - 1] == '\r') line[n - 1] = 0; }
        char *controllers = strchr(line, ':');
        if (!controllers) { errno = EINVAL; goto Bad; }
        *controllers++ = 0;
        char *path = strchr(controllers, ':');
        if (!path) { errno = EINVAL; goto Bad; }
        *path++ = 0;
        int64_t id = 0;
        if (!raw && !signed_decimal(line, strlen(line), 64, &id)) { errno = EINVAL; goto Bad; }
        if (!*path || path[0] != '/') { errno = EINVAL; goto Bad; }
        if (cpu_list_contains(controllers, "cpu")) {
            if (!copy_cpu_path(v1, path)) goto Bad;
            if (raw) { close(r.fd); return true; } /* first v1 wins */
        }
        if ((raw && !strcmp(line, "0")) || (!raw && !*controllers && id == 0)) {
            if (!copy_cpu_path(v2, path)) goto Bad;
        } else if (!raw && !*controllers) v2[0] = 0; /* last subsystem map entry */
    }
    if (result < 0) goto Bad;
    close(r.fd); return true;
Bad:
    { int error = errno; close(r.fd); quota_error(q, error == EINVAL || error == ENAMETOOLONG || error == EOVERFLOW ? HP_CPU_QUOTA_MALFORMED : HP_CPU_QUOTA_ERROR, error); return false; }
}
typedef struct { char *root, *mount, *type, *options; } cpu_mount_t;
static bool cpu_mount_line(char *line, cpu_mount_t *m)
{
    char *fields[6], *p = line;
    for (unsigned i = 0; i < 6; ++i) {
        fields[i] = p;
        char *space = strchr(p, ' ');
        if (!space || space == p) { errno = EINVAL; return false; }
        *space = 0; p = space + 1;
    }
    if (!strcmp(p, "-")) { errno = EINVAL; return false; }
    if (p[0] == '-' && p[1] == ' ') p += 2;
    else { char *separator = strstr(p, " - "); if (!separator) { errno = EINVAL; return false; } p = separator + 3; }
    m->root = fields[3]; m->mount = fields[4]; m->type = p;
    char *space = strchr(p, ' ');
    if (!space) { errno = EINVAL; return false; }
    *space++ = 0;
    space = strchr(space, ' '); /* skip source, preserve remaining super options */
    if (!space) { errno = EINVAL; return false; }
    m->options = space + 1;
    if (m->root[0] != '/' || m->mount[0] != '/') { errno = EINVAL; return false; }
    return true;
}
static bool cpu_mount_path(bool raw, const char *membership, const cpu_mount_t *m, char *out)
{
    char root[CPU_PATH_CAP], mount[CPU_PATH_CAP], group[CPU_PATH_CAP];
    if (!copy_cpu_path(root, m->root) || !copy_cpu_path(mount, m->mount)) return false;
    if (raw) {
        if (!unescape_cpu_path(root) || !unescape_cpu_path(mount)) return false;
        if (!path_prefix(membership, root)) return false;
        const char *relative = membership + (strlen(root) == 1 ? 0 : strlen(root));
        if (path_prefix(relative, "/..")) return false;
        return join_cpu_path(out, mount, relative, false);
    }
    if (!clean_cpu_path(group, membership)) return false;
    char clean_root[CPU_PATH_CAP];
    if (!clean_cpu_path(clean_root, root)) return false;
    if (!path_prefix(group, clean_root)) { errno = EXDEV; return false; }
    return join_cpu_path(out, mount, group + (strlen(clean_root) == 1 ? 0 : strlen(clean_root)), true);
}
static bool find_cpu_base(bool raw, char *base, hp_cpu_quota_info_t *q)
{
    char v1[CPU_PATH_CAP], v2[CPU_PATH_CAP];
    if (!cpu_memberships(raw, v1, v2, q)) return false;
    cpu_lines_t r;
    if (open_lines(&r, "/proc/self/mountinfo")) { quota_error(q, errno == ENOENT ? HP_CPU_QUOTA_NONE : HP_CPU_QUOTA_ERROR, errno); return false; }
    char line[CPU_LINE_CAP], v1_base[CPU_PATH_CAP] = {0};
    bool oversized, newline, caddy_v2_mount = false; int result, v1_error = 0;
    unsigned raw_version = *v1 ? 1 : 2;
    const char *raw_group = *v1 ? v1 : v2;
    if (raw && !*raw_group) { close(r.fd); q->state = HP_CPU_QUOTA_NONE; return false; }
    while ((result = next_line(&r, line, &oversized, &newline)) > 0) {
        if (!raw && (oversized || !newline)) {
            /* Scanner accepts an EOF-terminated final line, but not a token
             * over its bound. Ordinary proc lines always have newlines. */
            if (oversized) { errno = EOVERFLOW; goto Bad; }
        }
        if (!raw) { size_t n = strlen(line); if (n && line[n - 1] == '\r') line[n - 1] = 0; }
        cpu_mount_t m;
        if (!cpu_mount_line(line, &m)) goto Bad;
        if (raw) {
            if ((raw_version == 1 && (strcmp(m.type, "cgroup") || !cpu_list_contains(m.options, "cpu"))) ||
                (raw_version == 2 && strcmp(m.type, "cgroup2"))) continue;
            if (oversized) { errno = EOVERFLOW; goto Bad; }
            errno = 0;
            if (!cpu_mount_path(true, raw_group, &m, base)) { if (errno) goto Bad; continue; }
            q->version = raw_version; close(r.fd); return true;
        }
        if (!strcmp(m.type, "cgroup2") && !strcmp(m.mount, "/sys/fs/cgroup")) caddy_v2_mount = true;
        if (*v1 && !v1_error && !strcmp(m.type, "cgroup") && cpu_list_contains(m.options, "cpu")) {
            if (!cpu_mount_path(false, v1, &m, v1_base)) v1_error = errno; /* v2 query takes precedence */
        }
    }
    if (result < 0) goto Bad;
    close(r.fd);
    if (!raw && caddy_v2_mount && *v2) {
        if (!join_cpu_path(base, "/sys/fs/cgroup", v2, true)) { quota_error(q, HP_CPU_QUOTA_MALFORMED, errno); return false; }
        q->version = 2; return true;
    }
    if (!raw && v1_error) { quota_error(q, HP_CPU_QUOTA_MALFORMED, v1_error); return false; }
    if (!raw && *v1_base) { memcpy(base, v1_base, strlen(v1_base) + 1); q->version = 1; return true; }
    q->state = HP_CPU_QUOTA_NONE; return false;
Bad:
    { int error = errno; close(r.fd); quota_error(q, error == EINVAL || error == EOVERFLOW || error == ENAMETOOLONG || error == EXDEV ? HP_CPU_QUOTA_MALFORMED : HP_CPU_QUOTA_ERROR, error); return false; }
}
static void close_quota_fds(cpu_quota_fds_t *fds)
{
    if (fds->quota_fd >= 0) close(fds->quota_fd);
    if (fds->period_fd >= 0) close(fds->period_fd);
    fds->quota_fd = fds->period_fd = -1;
}
static bool open_quota_fds(bool raw, cpu_quota_fds_t *fds, hp_cpu_quota_info_t *q)
{
    char base[CPU_PATH_CAP], path[CPU_PATH_CAP];
    fds->quota_fd = fds->period_fd = -1;
    if (!find_cpu_base(raw, base, q)) return false;
    fds->version = q->version;
    if (!join_cpu_path(path, base, q->version == 1 ? "/cpu.cfs_quota_us" : "/cpu.max", false)) { quota_error(q, HP_CPU_QUOTA_MALFORMED, errno); return false; }
    fds->quota_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fds->quota_fd < 0) { quota_error(q, !raw && q->version == 2 && errno == ENOENT ? HP_CPU_QUOTA_NONE : HP_CPU_QUOTA_ERROR, errno); return false; }
    if (q->version == 1) {
        if (!join_cpu_path(path, base, "/cpu.cfs_period_us", false)) { int error = errno; close_quota_fds(fds); quota_error(q, HP_CPU_QUOTA_MALFORMED, error); return false; }
        fds->period_fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fds->period_fd < 0) { int error = errno; close_quota_fds(fds); quota_error(q, HP_CPU_QUOTA_ERROR, error); return false; }
    }
    return true;
}
static bool quota_text(int fd, bool raw, char *out, hp_cpu_quota_info_t *q)
{
    if (raw) {
        ssize_t n;
        do { n = pread(fd, out, 64, 0); } while (n < 0 && errno == EINTR);
        if (n < 0) { quota_error(q, HP_CPU_QUOTA_ERROR, errno); return false; }
        if (n == 64 || memchr(out, 0, (size_t)n)) { quota_error(q, HP_CPU_QUOTA_MALFORMED, EINVAL); return false; }
        out[n] = 0; return true;
    }
    cpu_lines_t r = {.fd = fd}; bool oversized, newline;
    int result = next_line(&r, out, &oversized, &newline);
    if (result <= 0 || oversized) { quota_error(q, result < 0 ? HP_CPU_QUOTA_ERROR : HP_CPU_QUOTA_MALFORMED, result < 0 ? errno : EINVAL); return false; }
    size_t n = strlen(out); if (n && out[n - 1] == '\r') out[n - 1] = 0;
    return true;
}
static bool ascii_space(unsigned char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static bool parse_quota_text(bool raw, unsigned version, char *text, int64_t *number, int64_t *period, bool *unlimited)
{
    *unlimited = false;
    if (version == 1) {
        char *end = raw ? strchr(text, '\n') : text + strlen(text);
        return end && signed_decimal(text, (size_t)(end - text), 64, number);
    }
    if (raw) {
        char *space = strchr(text, ' ');
        if (!space) return false;
        if ((size_t)(space - text) == 3 && !memcmp(text, "max", 3)) { *unlimited = true; return true; }
        char *end = strchr(space + 1, '\n');
        return end && signed_decimal(text, (size_t)(space - text), 64, number) &&
            signed_decimal(space + 1, (size_t)(end - space - 1), 64, period);
    }
    const char *fields[3]; size_t lengths[3]; unsigned count = 0;
    while (*text) {
        while (ascii_space((unsigned char)*text)) ++text;
        if (!*text) break;
        if (count == 3) return false;
        fields[count] = text;
        while (*text && !ascii_space((unsigned char)*text)) ++text;
        lengths[count] = (size_t)(text - fields[count]);
        ++count;
    }
    if (!count || count > 2) return false;
    if (lengths[0] == 3 && !memcmp(fields[0], "max", 3)) { *unlimited = true; return true; }
    *period = 100000;
    return signed_decimal(fields[0], lengths[0], 64, number) &&
        (count == 1 || signed_decimal(fields[1], lengths[1], 64, period));
}
static void read_cpu_quota(bool raw, const cpu_quota_fds_t *fds, hp_cpu_quota_info_t *q)
{
    char text[CPU_LINE_CAP]; bool unlimited; int64_t quota = 0, period = 0, unused = 0;
    if (!quota_text(fds->quota_fd, raw, text, q)) return;
    if (!parse_quota_text(raw, fds->version, text, &quota, &period, &unlimited)) { quota_error(q, HP_CPU_QUOTA_MALFORMED, EINVAL); return; }
    if (unlimited || (fds->version == 1 && (raw ? quota < 0 : quota <= 0))) { q->state = HP_CPU_QUOTA_UNLIMITED; q->quota_us = quota; return; }
    if (fds->version == 1) {
        if (!quota_text(fds->period_fd, raw, text, q)) return;
        if (!parse_quota_text(raw, 1, text, &period, &unused, &unlimited)) { quota_error(q, HP_CPU_QUOTA_MALFORMED, EINVAL); return; }
    }
    q->quota_us = quota; q->period_us = period;
    if (!raw && fds->version == 1 && period <= 0) { q->state = HP_CPU_QUOTA_NONE; return; }
    /* Kernel-valid quota/period are positive. Synthetic negative/zero v2 or
     * period values remain explicitly malformed, not fabricated CPU counts. */
    if (quota <= 0 || period <= 0) { quota_error(q, HP_CPU_QUOTA_MALFORMED, EINVAL); return; }
    q->state = HP_CPU_QUOTA_FINITE;
}
static void discover_cpu_quota(bool raw, bool retain, hp_cpu_quota_info_t *q)
{
    cpu_quota_fds_t fds = {0, -1, -1};
    if (!open_quota_fds(raw, &fds, q)) return;
    read_cpu_quota(raw, &fds, q);
    if (raw && retain) raw_quota_fds = fds;
    else close_quota_fds(&fds);
}
#endif

static void apply_startup_cpu_policy(hp_cpu_budget_info_t *info, unsigned explicit_count)
{
    unsigned raw = info->affinity_cpus;
    if (info->gomax_state == HP_CPU_GOMAX_POSITIVE) raw = explicit_count;
    else if (info->containermaxprocs > 0 && info->raw_quota.state == HP_CPU_QUOTA_FINITE) {
        uint64_t quota = (uint64_t)info->raw_quota.quota_us, period = (uint64_t)info->raw_quota.period_us;
        uint64_t count = quota / period + (quota % period != 0);
        if (count < 2) count = 2;
        if (count < raw) raw = (unsigned)count;
    }
    info->startup_cpus = info->defaults_cpus = raw;
    if (!info->gomax_present && info->caddy_quota.state == HP_CPU_QUOTA_FINITE) {
        uint64_t count = (uint64_t)info->caddy_quota.quota_us / (uint64_t)info->caddy_quota.period_us;
        if (!count) count = 1;
        info->caddy_override = true;
        if (count > INT32_MAX) { info->defaults_cpus = 0; info->defaults_error = ERANGE; }
        else info->defaults_cpus = (unsigned)count;
    }
    info->automatic_update_eligible = info->gomax_state != HP_CPU_GOMAX_POSITIVE &&
        info->updatemaxprocs != 0 && !info->caddy_override;
    info->quota_qualified = false;
}

static void capture_cpus(void)
{
    cpu_info.affinity_cpus = discover_cpus(&cpu_info);
    unsigned explicit_count = 0;
    const char *value = getenv("GOMAXPROCS"); /* real process env, not php.env */
    cpu_info.gomax_state = parse_gomaxprocs(value, &explicit_count);
    cpu_info.gomax_present = value != NULL;
    parse_cpu_godebug(getenv("GODEBUG"), &cpu_info.containermaxprocs, &cpu_info.updatemaxprocs);
#if defined(__linux__)
    discover_cpu_quota(true, cpu_info.containermaxprocs > 0, &cpu_info.raw_quota);
    if (!cpu_info.gomax_present) discover_cpu_quota(false, false, &cpu_info.caddy_quota);
    else cpu_info.caddy_quota.state = HP_CPU_QUOTA_SKIPPED;
#else
    cpu_info.raw_quota.state = cpu_info.caddy_quota.state = HP_CPU_QUOTA_UNSUPPORTED;
#endif
    apply_startup_cpu_policy(&cpu_info, explicit_count);
}
void hp_cpu_budget_capture_startup(void) { (void)pthread_once(&cpu_once, capture_cpus); }
const hp_cpu_budget_info_t *hp_cpu_budget_info(void)
{
    hp_cpu_budget_capture_startup();
    return &cpu_info;
}
unsigned hp_cpu_budget_default_count(void) { return hp_cpu_budget_info()->defaults_cpus; }
bool hp_cpu_budget_default_count_checked(unsigned *count)
{
    const hp_cpu_budget_info_t *info = hp_cpu_budget_info();
    if (!count) { errno = EINVAL; return false; }
    if (info->defaults_error || !info->defaults_cpus) { errno = info->defaults_error ? info->defaults_error : ERANGE; return false; }
    *count = info->defaults_cpus; return true;
}
unsigned hp_cpu_budget_scaling_count(void) { return hp_cpu_budget_info()->startup_cpus; }
bool hp_cpu_budget_json(char *out, size_t cap)
{
    static const char *states[] = {"absent", "empty", "positive", "nonpositive", "invalid"};
    static const char *sources[] = {"linux-affinity", "platform-online", "fallback-one"};
    static const char *quotas[] = {"none", "finite", "unlimited", "error", "malformed", "skipped-env-present", "unsupported"};
    const hp_cpu_budget_info_t *info = hp_cpu_budget_info();
    int n = snprintf(out, cap,
        "{\"discovery\":\"%s\",\"affinity_cpus\":%u,\"startup_cpus\":%u,\"defaults_cpus\":%u,\"scaling_cpus\":%u,"
        "\"gomaxprocs_env_state\":\"%s\",\"gomaxprocs_env_present\":%s,\"affinity_error\":%d,"
        "\"containermaxprocs\":%d,\"updatemaxprocs\":%d,\"caddy_override\":%s,\"defaults_error\":%d,"
        "\"raw_quota\":{\"state\":\"%s\",\"version\":%u,\"quota_us\":%" PRId64 ",\"period_us\":%" PRId64 ",\"error\":%d},"
        "\"caddy_quota\":{\"state\":\"%s\",\"version\":%u,\"quota_us\":%" PRId64 ",\"period_us\":%" PRId64 ",\"error\":%d},"
        "\"stable_startup_quota_policy\":%s,\"quota_aware\":%s,\"quota_parity_qualified\":false,\"dynamic_updates\":false,\"automatic_update_eligible\":%s,"
        "\"go_private_parser_qualified\":false}",
        sources[info->discovery], info->affinity_cpus, info->startup_cpus,
        info->defaults_cpus, info->startup_cpus, states[info->gomax_state],
        info->gomax_present ? "true" : "false", info->affinity_error,
        info->containermaxprocs, info->updatemaxprocs, info->caddy_override ? "true" : "false", info->defaults_error,
        quotas[info->raw_quota.state], info->raw_quota.version, info->raw_quota.quota_us, info->raw_quota.period_us, info->raw_quota.error,
        quotas[info->caddy_quota.state], info->caddy_quota.version, info->caddy_quota.quota_us, info->caddy_quota.period_us, info->caddy_quota.error,
#if defined(__linux__)
        "true", "true",
#else
        "false", "false",
#endif
        info->automatic_update_eligible ? "true" : "false");
    return n >= 0 && (size_t)n < cap;
}
