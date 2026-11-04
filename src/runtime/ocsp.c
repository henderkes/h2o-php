/* Native counterpart of H2O's fetch-ocsp-response helper. Runs in a fresh
 * multicall process, before any PHP interpreter or H2O network thread. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ocsp.h"
#include <openssl/err.h>
#include <openssl/ocsp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static int wait_bio(BIO *bio, uint64_t deadline)
{
    int fd = -1; BIO_get_fd(bio, &fd);
    if (fd < 0 || !BIO_should_retry(bio)) return 0;
    for (;;) {
        uint64_t now = monotonic_ms();
        if (now >= deadline) { errno = ETIMEDOUT; return 0; }
        struct pollfd pfd = {.fd = fd, .events = (BIO_should_write(bio) || BIO_should_io_special(bio)) ? POLLOUT : POLLIN};
        int rc = poll(&pfd, 1, (int)(deadline - now));
        if (rc < 0 && errno == EINTR) continue;
        return rc > 0;
    }
}
static X509 *read_issuer(const char *path)
{
    BIO *input = BIO_new_file(path, "r");
    if (!input) return NULL;
    X509 *cert = PEM_read_bio_X509(input, NULL, NULL, NULL);
    BIO_free(input); return cert;
}
int hp_native_ocsp_main(int argc, char **argv)
{
    const char *issuer_path = NULL, *cert_path = NULL, *url_override = NULL, *ca_path = NULL;
    unsigned timeout = 30;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) {
            fputs("Usage: h2o-php --internal-fetch-ocsp-response [--issuer FILE] [--url URL] [--ca-file FILE] [--timeout SECONDS] [CHAIN.pem]\n"
                  "Reads the PEM chain from stdin when no file is supplied; writes verified OCSP DER to stdout.\n", stdout);
            return 0;
        }
        if (!strcmp(argv[i], "--issuer") || !strcmp(argv[i], "--url") || !strcmp(argv[i], "--ca-file") || !strcmp(argv[i], "--timeout")) {
            const char *option = argv[i];
            if (++i == argc) return EX_USAGE;
            if (!strcmp(option, "--issuer")) issuer_path = argv[i];
            else if (!strcmp(option, "--url")) url_override = argv[i];
            else if (!strcmp(option, "--ca-file")) ca_path = argv[i];
            else {
                char *end; unsigned long value = strtoul(argv[i], &end, 10);
                if (*end || !value || value > 300) return EX_USAGE;
                timeout = (unsigned)value;
            }
        } else if (argv[i][0] == '-' || cert_path) return EX_USAGE;
        else cert_path = argv[i];
    }
    /* Also bounds name resolution and any unexpected library blocking path. */
    signal(SIGALRM, SIG_DFL); alarm(timeout);
    int result = EX_CONFIG;
    BIO *input = cert_path ? BIO_new_file(cert_path, "r") : BIO_new_fp(stdin, BIO_NOCLOSE);
    BIO *io = NULL; SSL_CTX *tls = NULL; OCSP_REQUEST *request = NULL; OCSP_CERTID *id = NULL;
    OCSP_RESPONSE *response = NULL; OCSP_BASICRESP *basic = NULL; OSSL_HTTP_REQ_CTX *http = NULL;
    X509_STORE *store = NULL; STACK_OF(X509) *chain = sk_X509_new_null();
    STACK_OF(OPENSSL_STRING) *urls = NULL;
    X509 *leaf = NULL, *issuer = NULL;
    char *host = NULL, *port = NULL, *path = NULL; unsigned char *der = NULL;
    if (!input || !chain) goto Done;
    leaf = PEM_read_bio_X509(input, NULL, NULL, NULL);
    if (!leaf) goto Done;
    for (unsigned n = 0; n < 64; ++n) {
        X509 *next = PEM_read_bio_X509(input, NULL, NULL, NULL);
        if (!next) break;
        if (!sk_X509_push(chain, next)) { X509_free(next); goto Done; }
    }
    ERR_clear_error();
    if (issuer_path) {
        issuer = read_issuer(issuer_path);
        if (!issuer || !sk_X509_push(chain, issuer)) { X509_free(issuer); issuer = NULL; goto Done; }
    } else {
        for (int i = 0; i < sk_X509_num(chain); ++i)
            if (X509_check_issued(sk_X509_value(chain, i), leaf) == X509_V_OK) { issuer = sk_X509_value(chain, i); break; }
    }
    if (!issuer || X509_check_issued(issuer, leaf) != X509_V_OK) {
        fputs("[OCSP] PEM chain does not contain the leaf's issuer\n", stderr); goto Done;
    }
    urls = X509_get1_ocsp(leaf);
    const char *url = url_override ? url_override : urls && sk_OPENSSL_STRING_num(urls) ? sk_OPENSSL_STRING_value(urls, 0) : NULL;
    int secure = 0;
    if (!url || !OCSP_parse_url(url, &host, &port, &path, &secure)) {
        fputs("[OCSP] certificate has no usable HTTP(S) responder URI\n", stderr); goto Done;
    }
    request = OCSP_REQUEST_new();
    id = OCSP_cert_to_id(NULL, leaf, issuer);
    if (!request || !id || !OCSP_request_add0_id(request, id)) { OCSP_CERTID_free(id); id = NULL; goto Done; }
    /* request owns id from here. Keep a borrowed pointer for response matching. */
    if (secure) {
        tls = SSL_CTX_new(TLS_client_method());
        if (!tls) goto Done;
        SSL_CTX_set_verify(tls, SSL_VERIFY_PEER, NULL);
        if (ca_path ? SSL_CTX_load_verify_locations(tls, ca_path, NULL) != 1 : SSL_CTX_set_default_verify_paths(tls) != 1) goto Done;
        io = BIO_new_ssl_connect(tls);
        SSL *ssl = NULL;
        if (!io || BIO_get_ssl(io, &ssl) != 1 || !ssl || !SSL_set1_host(ssl, host) || !SSL_set_tlsext_host_name(ssl, host)) goto Done;
    } else io = BIO_new_connect(host);
    if (!io || BIO_set_conn_hostname(io, host) != 1 || BIO_set_conn_port(io, port) != 1 || BIO_set_nbio(io, 1) != 1) goto Done;
    result = EX_TEMPFAIL;
    uint64_t deadline = monotonic_ms() + timeout * 1000ULL;
    while (BIO_do_connect(io) <= 0) if (!wait_bio(io, deadline)) goto Done;
    if (secure) while (BIO_do_handshake(io) <= 0) if (!wait_bio(io, deadline)) goto Done;
    http = OCSP_sendreq_new(io, path, NULL, 4096);
    char *authority = NULL;
    if (asprintf(&authority, strchr(host, ':') ? "[%s]:%s" : "%s:%s", host, port) < 0) goto Done;
    int headers_ok = http && OCSP_REQ_CTX_add1_header(http, "Host", authority) && OCSP_REQ_CTX_set1_req(http, request);
    free(authority);
    if (!headers_ok) goto Done;
    OCSP_set_max_response_length(http, 16 * 1024 * 1024);
    for (;;) {
        int rc = OCSP_sendreq_nbio(&response, http);
        if (rc == 1) break;
        if (rc == 0 || !wait_bio(io, deadline)) goto Done;
    }
    if (OCSP_response_status(response) != OCSP_RESPONSE_STATUS_SUCCESSFUL || !(basic = OCSP_response_get1_basic(response))) goto Done;
    store = X509_STORE_new();
    if (!store || !X509_STORE_add_cert(store, issuer) || !X509_STORE_set_flags(store, X509_V_FLAG_PARTIAL_CHAIN | X509_V_FLAG_TRUSTED_FIRST)) goto Done;
    if (OCSP_basic_verify(basic, chain, store, 0) != 1) goto Done;
    int status, reason; ASN1_GENERALIZEDTIME *revoked, *this_update, *next_update;
    if (!OCSP_resp_find_status(basic, id, &status, &reason, &revoked, &this_update, &next_update) ||
        !OCSP_check_validity(this_update, next_update, 300, 7 * 86400)) goto Done;
    int length = i2d_OCSP_RESPONSE(response, &der);
    if (length <= 0 || fwrite(der, 1, (size_t)length, stdout) != (size_t)length || fflush(stdout)) goto Done;
    result = 0;
Done:
    if (result) { fprintf(stderr, "[OCSP] native fetch failed (%s)\n", result == EX_TEMPFAIL ? "temporary" : "configuration"); ERR_print_errors_fp(stderr); }
    OPENSSL_free(der); OPENSSL_free(host); OPENSSL_free(port); OPENSSL_free(path);
    X509_email_free(urls); X509_STORE_free(store); OCSP_BASICRESP_free(basic);
    OCSP_RESPONSE_free(response); OCSP_REQ_CTX_free(http); OCSP_REQUEST_free(request);
    BIO_free_all(io); SSL_CTX_free(tls); X509_free(leaf); sk_X509_pop_free(chain, X509_free); BIO_free(input);
    alarm(0); return result;
}
