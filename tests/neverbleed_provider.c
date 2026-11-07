/* Observable provider boundaries: no PHP crypto fallback, no private export,
 * and an operation remains valid after the originating SSL_CTX is released. */
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/core_names.h>
#include "neverbleed.h"

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    SSL_CTX *ssl = SSL_CTX_new(TLS_server_method());
    neverbleed_t nb = {0}; char error[NEVERBLEED_ERRBUF_SIZE] = "";
    if (!ssl || !SSL_CTX_use_certificate_file(ssl, argv[1], SSL_FILETYPE_PEM) ||
        neverbleed_init(&nb, error) || neverbleed_load_private_key_file(&nb, ssl, argv[2], error) != 1) {
        fprintf(stderr, "startup: %s\n", error); ERR_print_errors_fp(stderr); return 3;
    }
    EVP_PKEY *key = SSL_CTX_get0_privatekey(ssl);
    EVP_PKEY *public_key = X509_get_pubkey(SSL_CTX_get0_certificate(ssl));
    OSSL_PARAM *exported = NULL;
    if (EVP_PKEY_todata(key, EVP_PKEY_KEYPAIR, &exported) > 0) {
        fputs("private key export unexpectedly succeeded\n", stderr); return 4;
    }
    OSSL_PARAM_free(exported); exported = NULL; ERR_clear_error();
    if (EVP_PKEY_todata(key, EVP_PKEY_PUBLIC_KEY, &exported) <= 0 ||
        OSSL_PARAM_locate(exported, OSSL_PKEY_PARAM_PRIV_KEY) || OSSL_PARAM_locate(exported, OSSL_PKEY_PARAM_RSA_D)) {
        fputs("invalid public key export\n", stderr); return 5;
    }
    OSSL_PARAM_free(exported);
    EVP_MD_CTX *sign = EVP_MD_CTX_new(), *verify = EVP_MD_CTX_new();
    if (!EVP_DigestSignInit(sign, NULL, EVP_sha256(), NULL, key) ||
        !EVP_DigestVerifyInit(verify, NULL, EVP_sha256(), NULL, public_key)) return 6;
    pid_t child = fork();
    if (child < 0) return 8;
    if (!child) {
        alarm(3);
        SSL_CTX_free(ssl); EVP_MD_CTX_free(sign); EVP_MD_CTX_free(verify); EVP_PKEY_free(public_key);
        _exit(0);
    }
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 9;
    /* The signing context must retain the remote reference independently. */
    SSL_CTX_free(ssl);
    const unsigned char message[] = "neverbleed provider lifetime regression";
    unsigned char signature[4096]; size_t length = sizeof(signature);
    if (!EVP_DigestSignUpdate(sign, message, sizeof(message)) || !EVP_DigestSignFinal(sign, signature, &length) ||
        !EVP_DigestVerifyUpdate(verify, message, sizeof(message)) || EVP_DigestVerifyFinal(verify, signature, length) != 1) {
        ERR_print_errors_fp(stderr); return 7;
    }
    EVP_MD_CTX_free(sign); EVP_MD_CTX_free(verify); EVP_PKEY_free(public_key);
    puts("private export rejected; public export and retained signing operation verified");
    return 0;
}
