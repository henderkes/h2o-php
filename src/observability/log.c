#include "h2o_php.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Open before PHP starts; close after its execution threads and engine stop.
 * The mutex also makes the FILE lifetime explicit for every native caller. */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file;

int hp_log_open(const char *path)
{
    FILE *next = stderr;
    if (path && strcmp(path, "stderr")) {
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
        if (fd < 0) {
            fprintf(stderr, "php.log: cannot open '%s' for append: %s\n", path, strerror(errno));
            return -1;
        }
        next = fdopen(fd, "a");
        if (!next) {
            int saved_errno = errno;
            close(fd);
            fprintf(stderr, "php.log: cannot create stream for '%s': %s\n", path, strerror(saved_errno));
            return -1;
        }
    }
    pthread_mutex_lock(&log_mutex);
    if (log_file && log_file != stderr) fclose(log_file);
    log_file = next;
    pthread_mutex_unlock(&log_mutex);
    return 0;
}

void hp_log_close(void)
{
    pthread_mutex_lock(&log_mutex);
    if (log_file && log_file != stderr) fclose(log_file);
    else fflush(stderr);
    log_file = NULL;
    pthread_mutex_unlock(&log_mutex);
}

static FILE *begin_record(void)
{
    pthread_mutex_lock(&log_mutex);
    FILE *out = log_file ? log_file : stderr;
    flockfile(out);
    return out;
}

static void end_record(FILE *out)
{
    fputc('\n', out);
    fflush(out);
    funlockfile(out);
    pthread_mutex_unlock(&log_mutex);
}

void hp_log_json(const char *record, size_t length)
{
    FILE *out = begin_record();
    if (length) fwrite(record, 1, length, out);
    end_record(out);
}

static void json_string(FILE *out, const char *value)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)value; p && *p; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (*p < 32) fprintf(out, "\\u%04x", *p);
            else fputc(*p, out);
        }
    }
    fputc('"', out);
}

void hp_log_message(const char *component, const char *message, int severity, uint64_t request_id)
{
    FILE *out = begin_record();
    fputs("{\"component\":", out);
    json_string(out, component);
    fputs(",\"message\":", out);
    json_string(out, message);
    fprintf(out, ",\"severity\":%d,\"request_id\":%" PRIu64 "}", severity, request_id);
    end_record(out);
}
