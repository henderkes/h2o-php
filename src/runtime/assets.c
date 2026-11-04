#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "assets.h"
#include "h2o-php-embedded-assets.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

#define CA_MARKER_ENV "H2O_PHP_MANAGED_CA"
#define CA_MEMFD_NAME "h2o-php-mozilla-ca-796f1389bfea"
#ifndef H2O_PHP_PREFER_EMBEDDED_CA
#define H2O_PHP_PREFER_EMBEDDED_CA 0
#endif
static char selected_path[PATH_MAX];
static int owned_ca_fd = -1;

const char *hp_assets_ca_path(void) { return *selected_path ? selected_path : NULL; }
const char *hp_assets_ca_sha256(void) { return HP_EMBEDDED_CA_SHA256; }
const char *hp_assets_ca_source(void) { return HP_EMBEDDED_CA_SOURCE; }
const char *hp_assets_ca_date(void) { return HP_EMBEDDED_CA_DATE; }

static STACK_OF(X509_INFO) *read_certificates(BIO *bio)
{
    int marked = ERR_set_mark();
    STACK_OF(X509_INFO) *items = PEM_X509_INFO_read_bio(bio, NULL, NULL, NULL);
    if (marked) ERR_pop_to_mark(); else ERR_clear_error();
    return items;
}
int hp_assets_ca_store(X509_STORE *store)
{
    if (!store || HP_EMBEDDED_CA_SIZE > INT_MAX) { errno = EINVAL; return -1; }
    BIO *bio = BIO_new_mem_buf(hp_embedded_ca_data, (int)HP_EMBEDDED_CA_SIZE);
    if (!bio) { errno = ENOMEM; return -1; }
    STACK_OF(X509_INFO) *items = read_certificates(bio); BIO_free(bio);
    if (!items) { errno = EPROTO; return -1; }
    unsigned added = 0; int result = 0;
    for (int i = 0; i < sk_X509_INFO_num(items); ++i) {
        X509 *certificate = sk_X509_INFO_value(items, i)->x509;
        if (!certificate) continue;
        int marked = ERR_set_mark();
        int ok = X509_STORE_add_cert(store, certificate);
        unsigned long reason = ok ? 0 : ERR_peek_last_error();
        if (marked) ERR_pop_to_mark(); else ERR_clear_error();
        if (!ok && ERR_GET_REASON(reason) != X509_R_CERT_ALREADY_IN_HASH_TABLE) { result = -1; break; }
        ++added;
    }
    sk_X509_INFO_pop_free(items, X509_INFO_free);
    if (!added || result) { errno = EPROTO; return -1; }
    return 0;
}
static bool valid_bundle(const char *path)
{
    if (!path || !*path) return false;
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 16 * 1024 * 1024) return false;
    BIO *bio = BIO_new_file(path, "r"); if (!bio) { ERR_clear_error(); return false; }
    STACK_OF(X509_INFO) *items = read_certificates(bio); BIO_free(bio);
    if (!items) return false;
    bool present = false;
    for (int i = 0; i < sk_X509_INFO_num(items); ++i)
        if (sk_X509_INFO_value(items, i)->x509) { present = true; break; }
    sk_X509_INFO_pop_free(items, X509_INFO_free); return present;
}
static bool hash_name(const char *name)
{
    if (strlen(name) < 10 || name[8] != '.') return false;
    for (unsigned i = 0; i < 8; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f') || (name[i] >= 'A' && name[i] <= 'F'))) return false;
    for (const char *p = name + 9; *p; ++p) if (*p < '0' || *p > '9') return false;
    return true;
}
static bool valid_directory(const char *path)
{
    if (!path || !*path) return false;
    DIR *directory = opendir(path); if (!directory) return false;
    bool found = false; unsigned count = 0;
    for (struct dirent *entry; (entry = readdir(directory)) && count < 4096;) {
        if (!hash_name(entry->d_name)) continue;
        ++count; char candidate[PATH_MAX];
        int n = snprintf(candidate, sizeof(candidate), "%s/%s", path, entry->d_name);
        if (n > 0 && n < (int)sizeof(candidate) && valid_bundle(candidate)) { found = true; break; }
    }
    closedir(directory); return found;
}
static int select_system_file(const char *path)
{
    if (snprintf(selected_path, sizeof(selected_path), "%s", path) >= (int)sizeof(selected_path)) { errno = ENAMETOOLONG; return -1; }
    return setenv("SSL_CERT_FILE", selected_path, 1);
}
/* An exact marker+path pair distinguishes our inherited CLOEXEC pathname from
 * an explicit user replacement. A still-open descriptor must also match the
 * recorded inode/device, sealed memfd name and immutable contents policy. */
static bool managed_path(const char *marker, const char *path, bool *live)
{
    *live = false;
#ifdef __linux__
    if (!marker || !path) return false;
    char digest[65], name[64]; unsigned long long process, device, inode;
    int fd, consumed = 0;
    if (sscanf(marker, "v1:%64[0-9a-f]:%llu:%d:%llu:%llu:%63s%n", digest, &process, &fd, &device, &inode, name, &consumed) != 6 ||
        marker[consumed] || strcmp(digest, HP_EMBEDDED_CA_SHA256) || strcmp(name, CA_MEMFD_NAME) || !process || fd < 3) return false;
    char expected[64]; snprintf(expected, sizeof(expected), "/proc/self/fd/%d", fd);
    if (strcmp(path, expected)) return false;
    struct stat st;
    if (fstat(fd, &st)) return errno == EBADF; /* Expected after exec with CLOEXEC. */
    if ((uintmax_t)st.st_dev != device || (uintmax_t)st.st_ino != inode || st.st_size != (off_t)HP_EMBEDDED_CA_SIZE)
        return process != (unsigned long long)getpid() || owned_ca_fd < 0;
    char target[128]; ssize_t n = readlink(expected, target, sizeof(target) - 1);
    if (n < 0) return false;
    target[n] = 0;
    if (strcmp(target, "/memfd:" CA_MEMFD_NAME " (deleted)")) return false;
    int seals = fcntl(fd, F_GET_SEALS);
    int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    *live = seals >= 0 && (seals & required) == required;
    return *live;
#else
    (void)marker; (void)path; return false;
#endif
}
static int select_embedded_file(void)
{
#if defined(__linux__) && defined(SYS_memfd_create)
    int fd = (int)syscall(SYS_memfd_create, CA_MEMFD_NAME, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    size_t offset = 0;
    while (offset < HP_EMBEDDED_CA_SIZE) {
        ssize_t n = write(fd, hp_embedded_ca_data + offset, HP_EMBEDDED_CA_SIZE - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto Error;
        offset += (size_t)n;
    }
    if (lseek(fd, 0, SEEK_SET) < 0 || fchmod(fd, 0444) ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) goto Error;
    char path[64], marker[256]; struct stat st;
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    if (fstat(fd, &st) || access(path, R_OK)) goto Error;
    snprintf(marker, sizeof(marker), "v1:%s:%llu:%d:%llu:%llu:%s", HP_EMBEDDED_CA_SHA256,
             (unsigned long long)getpid(), fd, (unsigned long long)st.st_dev, (unsigned long long)st.st_ino, CA_MEMFD_NAME);
    if (setenv(CA_MARKER_ENV, marker, 1)) goto Error;
    if (setenv("SSL_CERT_FILE", path, 1)) { unsetenv(CA_MARKER_ENV); goto Error; }
    snprintf(selected_path, sizeof(selected_path), "%s", path);
    owned_ca_fd = fd;
    /* Deliberately absent from the H2O host-fd registry: PHP fork children
     * must retain their immutable outbound trust file until they exec. */
    return 0;
Error:
    { int saved = errno; close(fd); errno = saved; return -1; }
#else
    errno = ENOTSUP; return -1;
#endif
}
int hp_assets_init(void)
{
    const char *file = getenv("SSL_CERT_FILE"), *directory = getenv("SSL_CERT_DIR");
    const char *marker = getenv(CA_MARKER_ENV);
    bool live = false, managed = managed_path(marker, file, &live);
    if (managed && live && !directory) {
        snprintf(selected_path, sizeof(selected_path), "%s", file);
        return 0;
    }
    if (managed && !live) owned_ca_fd = -1; /* Never close a reused foreign fd. */
    selected_path[0] = 0;
    if (managed) {
        /* The old managed descriptor is closed by exec, or a caller added an
         * explicit directory override. Remove only our exact old pathname. */
        if (unsetenv("SSL_CERT_FILE")) return -1;
        file = NULL;
    }
    if (marker && unsetenv(CA_MARKER_ENV)) return -1;
    if (file || directory) return 0; /* Any user override, including empty. */
#if H2O_PHP_PREFER_EMBEDDED_CA
    return select_embedded_file();
#endif
    if (valid_bundle(X509_get_default_cert_file()) || valid_directory(X509_get_default_cert_dir())) return 0;
    static const char *files[] = {
        "/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/cert.pem",
        "/etc/pki/tls/certs/ca-bundle.crt", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
        "/usr/local/share/certs/ca-root-nss.crt", "/etc/openssl/cert.pem", NULL
    };
    for (size_t i = 0; files[i]; ++i) if (valid_bundle(files[i])) return select_system_file(files[i]);
    static const char *directories[] = {"/etc/ssl/certs", "/etc/pki/tls/certs", "/usr/local/share/certs", NULL};
    for (size_t i = 0; directories[i]; ++i)
        if (valid_directory(directories[i])) return setenv("SSL_CERT_DIR", directories[i], 1);
    return select_embedded_file();
}
