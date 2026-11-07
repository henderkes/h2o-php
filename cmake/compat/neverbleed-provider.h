/* OpenSSL 4 private-key provider for the pinned neverbleed transport.
 * Private key material stays in neverbleed's daemon. The server holds only
 * public parameters and an authenticated daemon key reference. */
#include <openssl/core.h>
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#include <openssl/provider.h>
#include <openssl/params.h>

#define NBP_REFERENCE "neverbleed-reference"
typedef struct nbp_handle {
    struct st_neverbleed_rsa_exdata_t data;
    struct nbp_handle *next;
    uint64_t token;
    unsigned refs;
} nbp_handle_t;
typedef struct {
    EVP_PKEY *public_key;
    nbp_handle_t *handle;
    int rsa;
} nbp_key_t;
typedef struct {
    nbp_key_t *key;
    EVP_MD_CTX *digest;
    char mdname[80], mgf1name[80];
    int padding, saltlen;
    unsigned client_version, negotiated_version, implicit_rejection;
} nbp_context_t;
static pthread_mutex_t nbp_handles_lock = PTHREAD_MUTEX_INITIALIZER;
static nbp_handle_t *nbp_handles;
static uint64_t nbp_next_token;
static pid_t nbp_owner_process;

static nbp_handle_t *nbp_handle_find(uint64_t token, int retain)
{
    if (getpid() != nbp_owner_process) return NULL;
    pthread_mutex_lock(&nbp_handles_lock);
    nbp_handle_t *h;
    for (h = nbp_handles; h && h->token != token; h = h->next) {}
    if (h && retain) ++h->refs;
    pthread_mutex_unlock(&nbp_handles_lock);
    return h;
}
static void nbp_handle_retain(nbp_handle_t *h)
{
    if (!h || getpid() != nbp_owner_process) return;
    pthread_mutex_lock(&nbp_handles_lock); ++h->refs; pthread_mutex_unlock(&nbp_handles_lock);
}
static void nbp_handle_release(nbp_handle_t *h)
{
    /* A PHP fork child inherits only copies of these objects and locks. It
     * must never delete the parent's daemon key or enter inherited mutexes. */
    if (!h || getpid() != nbp_owner_process) return;
    pthread_mutex_lock(&nbp_handles_lock);
    if (--h->refs) { pthread_mutex_unlock(&nbp_handles_lock); return; }
    nbp_handle_t **slot = &nbp_handles;
    while (*slot != h) slot = &(*slot)->next;
    *slot = h->next;
    pthread_mutex_unlock(&nbp_handles_lock);
    neverbleed_iobuf_t buf = {NULL};
    iobuf_push_str(&buf, "del_pkey"); iobuf_push_num(&buf, h->data.key_index);
    iobuf_transaction_no_response(&buf, get_thread_data(h->data.nb));
    free(h);
}
static struct st_neverbleed_rsa_exdata_t *nbp_exdata(EVP_PKEY *pkey)
{
    uint64_t token = 0;
    OSSL_PARAM params[] = {OSSL_PARAM_uint64(NBP_REFERENCE, &token), OSSL_PARAM_END};
    if (!EVP_PKEY_get_params(pkey, params) || params[0].return_size != sizeof(token)) return NULL;
    nbp_handle_t *h = nbp_handle_find(token, 0);
    return h ? &h->data : NULL;
}
static void *nbp_key_new(int rsa)
{
    nbp_key_t *key = calloc(1, sizeof(*key));
    if (key) key->rsa = rsa;
    return key;
}
static void *nbp_rsa_new(void *ctx) { (void)ctx; return nbp_key_new(1); }
static void *nbp_ec_new(void *ctx) { (void)ctx; return nbp_key_new(0); }
static void nbp_key_free(void *opaque)
{
    nbp_key_t *key = opaque;
    if (!key || getpid() != nbp_owner_process) return;
    EVP_PKEY_free(key->public_key); nbp_handle_release(key->handle); free(key);
}
static void *nbp_key_dup(const void *opaque, int selection)
{
    (void)selection;
    const nbp_key_t *source = opaque;
    nbp_key_t *key = malloc(sizeof(*key));
    if (!key) return NULL;
    *key = *source;
    if (key->public_key) EVP_PKEY_up_ref(key->public_key);
    nbp_handle_retain(key->handle);
    return key;
}
static int nbp_key_import(void *opaque, int selection, const OSSL_PARAM params[])
{
    nbp_key_t *key = opaque;
    EVP_PKEY *public_key = NULL;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(NULL, key->rsa ? "RSA" : "EC", "provider=default");
    int ok = ctx && EVP_PKEY_fromdata_init(ctx) > 0 &&
        EVP_PKEY_fromdata(ctx, &public_key, EVP_PKEY_PUBLIC_KEY, (OSSL_PARAM *)params) > 0;
    EVP_PKEY_CTX_free(ctx);
    if (!ok) { EVP_PKEY_free(public_key); return 0; }
    nbp_handle_t *handle = NULL;
    const OSSL_PARAM *reference = OSSL_PARAM_locate_const(params, NBP_REFERENCE);
    if (reference) {
        uint64_t token;
        if (!OSSL_PARAM_get_uint64(reference, &token) || !(handle = nbp_handle_find(token, 1))) {
            EVP_PKEY_free(public_key); return 0;
        }
    } else if (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) {
        EVP_PKEY_free(public_key); return 0;
    }
    EVP_PKEY_free(key->public_key); nbp_handle_release(key->handle);
    key->public_key = public_key; key->handle = handle;
    return 1;
}
static int nbp_key_export(void *opaque, int selection, OSSL_CALLBACK *callback, void *argument)
{
    nbp_key_t *key = opaque;
    /* Exporting a private key must fail, never silently substitute public data. */
    if (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) return 0;
    OSSL_PARAM *params = NULL;
    if (!key->public_key || !EVP_PKEY_todata(key->public_key, EVP_PKEY_PUBLIC_KEY, &params)) return 0;
    int result = callback(params, argument);
    OSSL_PARAM_free(params); return result;
}
static const OSSL_PARAM nbp_rsa_types[] = {
    OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_N, NULL, 0), OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_E, NULL, 0),
    OSSL_PARAM_uint64(NBP_REFERENCE, NULL), OSSL_PARAM_END
};
static const OSSL_PARAM nbp_ec_types[] = {
    OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, NULL, 0),
    OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, NULL, 0),
    OSSL_PARAM_uint64(NBP_REFERENCE, NULL), OSSL_PARAM_END
};
static const OSSL_PARAM *nbp_rsa_io_types(int selection) { (void)selection; return nbp_rsa_types; }
static const OSSL_PARAM *nbp_ec_io_types(int selection) { (void)selection; return nbp_ec_types; }
static const OSSL_PARAM *nbp_key_gettable(void *ctx)
{
    (void)ctx;
    static const OSSL_PARAM types[] = {
        OSSL_PARAM_int(OSSL_PKEY_PARAM_BITS, NULL), OSSL_PARAM_int(OSSL_PKEY_PARAM_SECURITY_BITS, NULL),
        OSSL_PARAM_int(OSSL_PKEY_PARAM_MAX_SIZE, NULL),
        OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_DEFAULT_DIGEST, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, NULL, 0),
        OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, NULL, 0),
        OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, NULL, 0),
        OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_N, NULL, 0), OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_E, NULL, 0),
        OSSL_PARAM_uint64(NBP_REFERENCE, NULL), OSSL_PARAM_END
    };
    return types;
}
static int nbp_key_get_params(void *opaque, OSSL_PARAM params[])
{
    nbp_key_t *key = opaque;
    if (!key->public_key || !EVP_PKEY_get_params(key->public_key, params)) return 0;
    OSSL_PARAM *reference = OSSL_PARAM_locate(params, NBP_REFERENCE);
    return !reference || (key->handle && OSSL_PARAM_set_uint64(reference, key->handle->token));
}
static int nbp_key_has(const void *opaque, int selection)
{
    const nbp_key_t *key = opaque;
    return key && key->public_key && (!(selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) || key->handle);
}
static int nbp_key_match(const void *a, const void *b, int selection)
{
    (void)selection;
    const nbp_key_t *ka = a, *kb = b;
    return ka->public_key && kb->public_key && EVP_PKEY_eq(ka->public_key, kb->public_key) == 1;
}
static const char *nbp_rsa_operation(int operation) { (void)operation; return "RSA"; }
static const char *nbp_ec_operation(int operation) { return operation == OSSL_OP_SIGNATURE ? "ECDSA" : NULL; }
#define NBP_DISPATCH(id, fn) { id, (void (*)(void))fn }
#define NBP_KEY_FUNCTIONS(newfn, types, operation) \
    NBP_DISPATCH(OSSL_FUNC_KEYMGMT_NEW, newfn), NBP_DISPATCH(OSSL_FUNC_KEYMGMT_FREE, nbp_key_free), \
    NBP_DISPATCH(OSSL_FUNC_KEYMGMT_DUP, nbp_key_dup), NBP_DISPATCH(OSSL_FUNC_KEYMGMT_IMPORT, nbp_key_import), \
    NBP_DISPATCH(OSSL_FUNC_KEYMGMT_IMPORT_TYPES, types), NBP_DISPATCH(OSSL_FUNC_KEYMGMT_EXPORT, nbp_key_export), \
    NBP_DISPATCH(OSSL_FUNC_KEYMGMT_EXPORT_TYPES, types), NBP_DISPATCH(OSSL_FUNC_KEYMGMT_HAS, nbp_key_has), \
    NBP_DISPATCH(OSSL_FUNC_KEYMGMT_MATCH, nbp_key_match), NBP_DISPATCH(OSSL_FUNC_KEYMGMT_GET_PARAMS, nbp_key_get_params), \
    NBP_DISPATCH(OSSL_FUNC_KEYMGMT_GETTABLE_PARAMS, nbp_key_gettable), \
    NBP_DISPATCH(OSSL_FUNC_KEYMGMT_QUERY_OPERATION_NAME, operation), {0, NULL}
static const OSSL_DISPATCH nbp_rsa_key_functions[] = { NBP_KEY_FUNCTIONS(nbp_rsa_new, nbp_rsa_io_types, nbp_rsa_operation) };
static const OSSL_DISPATCH nbp_ec_key_functions[] = { NBP_KEY_FUNCTIONS(nbp_ec_new, nbp_ec_io_types, nbp_ec_operation) };

static void *nbp_context_new(void *provider, const char *properties)
{
    (void)provider; (void)properties;
    nbp_context_t *ctx = calloc(1, sizeof(*ctx));
    if (ctx) { ctx->padding = RSA_PKCS1_PADDING; ctx->saltlen = RSA_PSS_SALTLEN_DIGEST; ctx->implicit_rejection = 1; }
    return ctx;
}
static void nbp_context_free(void *opaque)
{
    nbp_context_t *ctx = opaque;
    if (!ctx) return;
    nbp_key_free(ctx->key); EVP_MD_CTX_free(ctx->digest); free(ctx);
}
static void *nbp_context_dup(void *opaque)
{
    nbp_context_t *source = opaque, *ctx = malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    *ctx = *source; ctx->digest = NULL; ctx->key = NULL;
    if (source->key && !(ctx->key = nbp_key_dup(source->key, OSSL_KEYMGMT_SELECT_KEYPAIR))) goto Fail;
    if (source->digest && (!(ctx->digest = EVP_MD_CTX_new()) || !EVP_MD_CTX_copy_ex(ctx->digest, source->digest))) goto Fail;
    return ctx;
Fail:
    nbp_context_free(ctx); return NULL;
}
static int nbp_padding(const OSSL_PARAM *p, int *padding)
{
    if (!p) return 1;
    if (p->data_type == OSSL_PARAM_INTEGER || p->data_type == OSSL_PARAM_UNSIGNED_INTEGER) return OSSL_PARAM_get_int(p, padding);
    const char *name;
    if (!OSSL_PARAM_get_utf8_string_ptr(p, &name)) return 0;
    if (!strcmp(name, "pkcs1")) *padding = RSA_PKCS1_PADDING;
    else if (!strcmp(name, "pss")) *padding = RSA_PKCS1_PSS_PADDING;
    else if (!strcmp(name, "none")) *padding = RSA_NO_PADDING;
    else if (!strcmp(name, "oaep")) *padding = RSA_PKCS1_OAEP_PADDING;
    else if (!strcmp(name, "pkcs1-with-tls-padding")) *padding = RSA_PKCS1_WITH_TLS_PADDING;
    else return 0;
    return 1;
}
static int nbp_set_params(void *opaque, const OSSL_PARAM params[])
{
    nbp_context_t *ctx = opaque;
    if (!params) return 1;
    /* A rejected parameter set must leave an already usable operation intact. */
    nbp_context_t next = *ctx;
    const OSSL_PARAM *p;
    if (!nbp_padding(OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_PAD_MODE), &next.padding)) return 0;
    if ((p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_DIGEST))) {
        char *target = next.mdname;
        if (!OSSL_PARAM_get_utf8_string(p, &target, sizeof(next.mdname)) || !EVP_get_digestbyname(next.mdname)) return 0;
    }
    if ((p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_MGF1_DIGEST))) {
        char *target = next.mgf1name;
        if (!OSSL_PARAM_get_utf8_string(p, &target, sizeof(next.mgf1name)) || !EVP_get_digestbyname(next.mgf1name)) return 0;
    }
    if ((p = OSSL_PARAM_locate_const(params, OSSL_SIGNATURE_PARAM_PSS_SALTLEN))) {
        if (p->data_type == OSSL_PARAM_INTEGER) { if (!OSSL_PARAM_get_int(p, &next.saltlen)) return 0; }
        else {
            const char *name;
            if (!OSSL_PARAM_get_utf8_string_ptr(p, &name)) return 0;
            if (!strcmp(name, "digest")) next.saltlen = RSA_PSS_SALTLEN_DIGEST;
            else if (!strcmp(name, "max")) next.saltlen = RSA_PSS_SALTLEN_MAX;
            else if (!strcmp(name, "auto")) next.saltlen = RSA_PSS_SALTLEN_AUTO;
            else return 0;
        }
    }
    if ((p = OSSL_PARAM_locate_const(params, OSSL_ASYM_CIPHER_PARAM_TLS_CLIENT_VERSION)) && !OSSL_PARAM_get_uint(p, &next.client_version)) return 0;
    if ((p = OSSL_PARAM_locate_const(params, OSSL_ASYM_CIPHER_PARAM_TLS_NEGOTIATED_VERSION)) && !OSSL_PARAM_get_uint(p, &next.negotiated_version)) return 0;
    if ((p = OSSL_PARAM_locate_const(params, OSSL_ASYM_CIPHER_PARAM_IMPLICIT_REJECTION)) && !OSSL_PARAM_get_uint(p, &next.implicit_rejection)) return 0;
    /* The daemon's transport uses the signing digest for MGF1. Reject an
     * independent digest instead of accepting it and silently substituting. */
    if (*next.mdname && *next.mgf1name) {
        const EVP_MD *md = EVP_get_digestbyname(next.mdname), *mgf = EVP_get_digestbyname(next.mgf1name);
        if (!md || !mgf || EVP_MD_get_type(md) != EVP_MD_get_type(mgf)) return 0;
    }
    *ctx = next;
    return 1;
}
static const OSSL_PARAM *nbp_settable(void *ctx, void *provider)
{
    (void)ctx; (void)provider;
    static const OSSL_PARAM params[] = {
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_MGF1_DIGEST, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PSS_SALTLEN, NULL, 0),
        OSSL_PARAM_uint(OSSL_ASYM_CIPHER_PARAM_TLS_CLIENT_VERSION, NULL),
        OSSL_PARAM_uint(OSSL_ASYM_CIPHER_PARAM_TLS_NEGOTIATED_VERSION, NULL),
        OSSL_PARAM_uint(OSSL_ASYM_CIPHER_PARAM_IMPLICIT_REJECTION, NULL), OSSL_PARAM_END
    };
    return params;
}
static const OSSL_PARAM *nbp_gettable(void *ctx, void *provider)
{
    (void)ctx; (void)provider;
    static const OSSL_PARAM params[] = {
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST, NULL, 0),
        OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_MGF1_DIGEST, NULL, 0),
        OSSL_PARAM_int(OSSL_SIGNATURE_PARAM_PSS_SALTLEN, NULL), OSSL_PARAM_END
    };
    return params;
}
static const char *nbp_padding_name(int padding)
{
    switch (padding) {
    case RSA_PKCS1_PADDING: return "pkcs1";
    case RSA_PKCS1_PSS_PADDING: return "pss";
    case RSA_NO_PADDING: return "none";
    case RSA_PKCS1_OAEP_PADDING: return "oaep";
    case RSA_PKCS1_WITH_TLS_PADDING: return "pkcs1-with-tls-padding";
    default: return NULL;
    }
}
static int nbp_get_params(void *opaque, OSSL_PARAM params[])
{
    nbp_context_t *ctx = opaque; OSSL_PARAM *p;
    if ((p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_DIGEST)) && !OSSL_PARAM_set_utf8_string(p, ctx->mdname)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_MGF1_DIGEST)) &&
        !OSSL_PARAM_set_utf8_string(p, *ctx->mgf1name ? ctx->mgf1name : ctx->mdname)) return 0;
    if ((p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_PSS_SALTLEN))) {
        if (p->data_type == OSSL_PARAM_INTEGER || p->data_type == OSSL_PARAM_UNSIGNED_INTEGER) {
            if (!OSSL_PARAM_set_int(p, ctx->saltlen)) return 0;
        } else {
            char number[32]; const char *name;
            if (ctx->saltlen == RSA_PSS_SALTLEN_DIGEST) name = "digest";
            else if (ctx->saltlen == RSA_PSS_SALTLEN_MAX) name = "max";
            else if (ctx->saltlen == RSA_PSS_SALTLEN_AUTO) name = "auto";
            else { snprintf(number, sizeof(number), "%d", ctx->saltlen); name = number; }
            if (!OSSL_PARAM_set_utf8_string(p, name)) return 0;
        }
    }
    if ((p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_PAD_MODE))) {
        if (p->data_type == OSSL_PARAM_INTEGER || p->data_type == OSSL_PARAM_UNSIGNED_INTEGER) {
            if (!OSSL_PARAM_set_int(p, ctx->padding)) return 0;
        } else {
            const char *name = nbp_padding_name(ctx->padding);
            if (!name || !OSSL_PARAM_set_utf8_string(p, name)) return 0;
        }
    }
    return 1;
}
static int nbp_sign_init(void *opaque, void *key, const OSSL_PARAM params[])
{
    nbp_context_t *ctx = opaque;
    nbp_key_free(ctx->key); ctx->key = nbp_key_dup(key, OSSL_KEYMGMT_SELECT_KEYPAIR);
    return ctx->key && ctx->key->handle && nbp_set_params(ctx, params);
}
static int nbp_rpc_result(nbp_context_t *ctx, neverbleed_iobuf_t *buf, unsigned char *out, size_t *length, size_t capacity)
{
    iobuf_transaction(buf, get_thread_data(ctx->key->handle->data.nb));
    size_t count; const unsigned char *result = iobuf_shift_bytes(buf, &count);
    int ok = result && count && count <= capacity;
    if (ok) { memcpy(out, result, count); *length = count; }
    iobuf_dispose(buf); return ok;
}
static int nbp_sign(void *opaque, unsigned char *sig, size_t *siglen, size_t capacity, const unsigned char *input, size_t length)
{
    nbp_context_t *ctx = opaque;
    if (!ctx->key || !ctx->key->handle) return 0;
    int size = EVP_PKEY_get_size(ctx->key->public_key);
    if (size <= 0) return 0;
    if (!sig) { *siglen = (size_t)size; return 1; }
    if (capacity < (size_t)size) return 0;
    const EVP_MD *md = *ctx->mdname ? EVP_get_digestbyname(ctx->mdname) : NULL;
    const EVP_MD *mgf = *ctx->mgf1name ? EVP_get_digestbyname(ctx->mgf1name) : md;
    if (md && mgf && EVP_MD_get_type(md) != EVP_MD_get_type(mgf)) return 0;
    neverbleed_iobuf_t buf = {NULL};
    iobuf_push_str(&buf, "provider_sign");
    iobuf_push_num(&buf, ctx->key->handle->data.key_index);
    iobuf_push_num(&buf, md ? (size_t)EVP_MD_get_type(md) : SIZE_MAX);
    iobuf_push_num(&buf, (size_t)ctx->padding); iobuf_push_num(&buf, (size_t)(int64_t)ctx->saltlen);
    iobuf_push_bytes(&buf, input, length);
    return nbp_rpc_result(ctx, &buf, sig, siglen, capacity);
}
static int nbp_digest_init(void *opaque, const char *mdname, void *key, const OSSL_PARAM params[])
{
    nbp_context_t *ctx = opaque;
    if (!mdname || strlen(mdname) >= sizeof(ctx->mdname)) return 0;
    strcpy(ctx->mdname, mdname);
    if (!nbp_sign_init(ctx, key, params)) return 0;
    EVP_MD_CTX_free(ctx->digest); ctx->digest = EVP_MD_CTX_new();
    return ctx->digest && EVP_DigestInit_ex(ctx->digest, EVP_get_digestbyname(ctx->mdname), NULL);
}
static int nbp_digest_update(void *opaque, const unsigned char *data, size_t length)
{
    nbp_context_t *ctx = opaque;
    return ctx->digest && EVP_DigestUpdate(ctx->digest, data, length);
}
static int nbp_digest_final(void *opaque, unsigned char *sig, size_t *siglen, size_t capacity)
{
    nbp_context_t *ctx = opaque;
    if (!sig) return nbp_sign(ctx, NULL, siglen, capacity, NULL, 0);
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned length;
    EVP_MD_CTX *copy = EVP_MD_CTX_new();
    int ok = copy && ctx->digest && EVP_MD_CTX_copy_ex(copy, ctx->digest) && EVP_DigestFinal_ex(copy, digest, &length);
    EVP_MD_CTX_free(copy);
    return ok && nbp_sign(ctx, sig, siglen, capacity, digest, length);
}
static int nbp_digest_sign(void *opaque, unsigned char *sig, size_t *siglen, size_t capacity, const unsigned char *input, size_t length)
{
    nbp_context_t *ctx = opaque;
    if (!sig) return nbp_sign(ctx, NULL, siglen, capacity, NULL, 0);
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned digestlen;
    return EVP_Digest(input, length, digest, &digestlen, EVP_get_digestbyname(ctx->mdname), NULL) &&
        nbp_sign(ctx, sig, siglen, capacity, digest, digestlen);
}
static int nbp_decrypt(void *opaque, unsigned char *out, size_t *outlen, size_t capacity, const unsigned char *input, size_t length)
{
    nbp_context_t *ctx = opaque;
    if (!ctx->key || !ctx->key->handle) return 0;
    if (!out) { *outlen = EVP_PKEY_get_size(ctx->key->public_key); return 1; }
    if (*ctx->mdname || *ctx->mgf1name) return 0; /* OAEP custom digests are not accepted by this transport. */
    neverbleed_iobuf_t buf = {NULL};
    iobuf_push_str(&buf, "provider_decrypt");
    iobuf_push_num(&buf, ctx->key->handle->data.key_index); iobuf_push_num(&buf, ctx->padding);
    iobuf_push_num(&buf, ctx->client_version); iobuf_push_num(&buf, ctx->negotiated_version);
    iobuf_push_num(&buf, ctx->implicit_rejection); iobuf_push_bytes(&buf, input, length);
    return nbp_rpc_result(ctx, &buf, out, outlen, capacity);
}
static void *nbp_decrypt_new(void *provider) { return nbp_context_new(provider, NULL); }
static const OSSL_DISPATCH nbp_signature_functions[] = {
    NBP_DISPATCH(OSSL_FUNC_SIGNATURE_NEWCTX, nbp_context_new), NBP_DISPATCH(OSSL_FUNC_SIGNATURE_FREECTX, nbp_context_free),
    NBP_DISPATCH(OSSL_FUNC_SIGNATURE_DUPCTX, nbp_context_dup), NBP_DISPATCH(OSSL_FUNC_SIGNATURE_SIGN_INIT, nbp_sign_init),
    NBP_DISPATCH(OSSL_FUNC_SIGNATURE_SIGN, nbp_sign), NBP_DISPATCH(OSSL_FUNC_SIGNATURE_DIGEST_SIGN_INIT, nbp_digest_init),
    NBP_DISPATCH(OSSL_FUNC_SIGNATURE_DIGEST_SIGN_UPDATE, nbp_digest_update), NBP_DISPATCH(OSSL_FUNC_SIGNATURE_DIGEST_SIGN_FINAL, nbp_digest_final),
    NBP_DISPATCH(OSSL_FUNC_SIGNATURE_DIGEST_SIGN, nbp_digest_sign), NBP_DISPATCH(OSSL_FUNC_SIGNATURE_SET_CTX_PARAMS, nbp_set_params),
    NBP_DISPATCH(OSSL_FUNC_SIGNATURE_SETTABLE_CTX_PARAMS, nbp_settable), NBP_DISPATCH(OSSL_FUNC_SIGNATURE_GET_CTX_PARAMS, nbp_get_params),
    NBP_DISPATCH(OSSL_FUNC_SIGNATURE_GETTABLE_CTX_PARAMS, nbp_gettable), {0, NULL}
};
static const OSSL_DISPATCH nbp_decrypt_functions[] = {
    NBP_DISPATCH(OSSL_FUNC_ASYM_CIPHER_NEWCTX, nbp_decrypt_new), NBP_DISPATCH(OSSL_FUNC_ASYM_CIPHER_FREECTX, nbp_context_free),
    NBP_DISPATCH(OSSL_FUNC_ASYM_CIPHER_DUPCTX, nbp_context_dup), NBP_DISPATCH(OSSL_FUNC_ASYM_CIPHER_DECRYPT_INIT, nbp_sign_init),
    NBP_DISPATCH(OSSL_FUNC_ASYM_CIPHER_DECRYPT, nbp_decrypt), NBP_DISPATCH(OSSL_FUNC_ASYM_CIPHER_SET_CTX_PARAMS, nbp_set_params),
    NBP_DISPATCH(OSSL_FUNC_ASYM_CIPHER_SETTABLE_CTX_PARAMS, nbp_settable), {0, NULL}
};
static const OSSL_ALGORITHM *nbp_query(void *ctx, int operation, int *no_cache)
{
    (void)ctx; *no_cache = 0;
    static const OSSL_ALGORITHM keys[] = {
        {"RSA:rsaEncryption:1.2.840.113549.1.1.1", "provider=neverbleed", nbp_rsa_key_functions, "Neverbleed RSA public key and remote key reference"},
        {"EC:id-ecPublicKey:1.2.840.10045.2.1", "provider=neverbleed", nbp_ec_key_functions, "Neverbleed EC public key and remote key reference"}, {NULL, NULL, NULL, NULL}
    };
    static const OSSL_ALGORITHM signatures[] = {
        {"RSA", "provider=neverbleed", nbp_signature_functions, "Neverbleed RSA signing"},
        {"ECDSA", "provider=neverbleed", nbp_signature_functions, "Neverbleed ECDSA signing"}, {NULL, NULL, NULL, NULL}
    };
    static const OSSL_ALGORITHM decrypt[] = {{"RSA", "provider=neverbleed", nbp_decrypt_functions, "Neverbleed RSA decryption"}, {NULL, NULL, NULL, NULL}};
    switch (operation) { case OSSL_OP_KEYMGMT: return keys; case OSSL_OP_SIGNATURE: return signatures; case OSSL_OP_ASYM_CIPHER: return decrypt; default: return NULL; }
}
static int nbp_provider_init(const OSSL_CORE_HANDLE *handle, const OSSL_DISPATCH *input, const OSSL_DISPATCH **output, void **ctx)
{
    (void)handle; (void)input; *ctx = NULL;
    static const OSSL_DISPATCH functions[] = {NBP_DISPATCH(OSSL_FUNC_PROVIDER_QUERY_OPERATION, nbp_query), {0, NULL}};
    *output = functions; return 1;
}
static int nbp_init(void)
{
    static OSSL_PROVIDER *provider;
    if (!nbp_owner_process) nbp_owner_process = getpid();
    if (getpid() != nbp_owner_process) return 0;
    if (provider) return 1;
    if (!OSSL_PROVIDER_add_builtin(NULL, "neverbleed", nbp_provider_init)) return 0;
    provider = OSSL_PROVIDER_load(NULL, "neverbleed");
    return provider != NULL;
}
static EVP_PKEY *nbp_create(neverbleed_t *nb, size_t index, const char *type, OSSL_PARAM_BLD *builder)
{
    EVP_PKEY *pkey = NULL;
    nbp_handle_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->data.nb = nb; h->data.key_index = index; h->refs = 1;
    pthread_mutex_lock(&nbp_handles_lock);
    h->token = ++nbp_next_token; h->next = nbp_handles; nbp_handles = h;
    pthread_mutex_unlock(&nbp_handles_lock);
    OSSL_PARAM *params = NULL;
    EVP_PKEY_CTX *ctx = NULL;
    if (!OSSL_PARAM_BLD_push_uint64(builder, NBP_REFERENCE, h->token) || !(params = OSSL_PARAM_BLD_to_param(builder)) ||
        !(ctx = EVP_PKEY_CTX_new_from_name(NULL, type, "provider=neverbleed")) || EVP_PKEY_fromdata_init(ctx) <= 0 ||
        EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_KEYPAIR, params) <= 0) { EVP_PKEY_free(pkey); pkey = NULL; }
    EVP_PKEY_CTX_free(ctx); OSSL_PARAM_free(params); nbp_handle_release(h); return pkey;
}
static EVP_PKEY *nbp_create_rsa(neverbleed_t *nb, size_t index, const char *ehex, const char *nhex)
{
    BIGNUM *e = NULL, *n = NULL;
    OSSL_PARAM_BLD *builder = OSSL_PARAM_BLD_new(); EVP_PKEY *pkey = NULL;
    if (builder && BN_hex2bn(&e, ehex) && BN_hex2bn(&n, nhex) &&
        OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, n) && OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, e))
        pkey = nbp_create(nb, index, "RSA", builder);
    BN_free(e); BN_free(n); OSSL_PARAM_BLD_free(builder); return pkey;
}
static EVP_PKEY *nbp_create_ec(neverbleed_t *nb, size_t index, int curve, const void *public, size_t length)
{
    OSSL_PARAM_BLD *builder = OSSL_PARAM_BLD_new(); EVP_PKEY *pkey = NULL;
    const char *name = OBJ_nid2sn(curve);
    if (builder && name && OSSL_PARAM_BLD_push_utf8_string(builder, OSSL_PKEY_PARAM_GROUP_NAME, name, 0) &&
        OSSL_PARAM_BLD_push_octet_string(builder, OSSL_PKEY_PARAM_PUB_KEY, public, length)) pkey = nbp_create(nb, index, "EC", builder);
    OSSL_PARAM_BLD_free(builder); return pkey;
}
static int nbp_private_stub(neverbleed_iobuf_t *buf, int decrypt)
{
    size_t index, padding, mdnid = SIZE_MAX, saltlen = 0, cver = 0, nver = 0, implicit = 1, length;
    const unsigned char *input;
    if (iobuf_shift_num(buf, &index)) return -1;
    if (!decrypt && iobuf_shift_num(buf, &mdnid)) return -1;
    if (iobuf_shift_num(buf, &padding)) return -1;
    if (decrypt) {
        if (iobuf_shift_num(buf, &cver) || iobuf_shift_num(buf, &nver) || iobuf_shift_num(buf, &implicit)) return -1;
    } else if (iobuf_shift_num(buf, &saltlen)) return -1;
    if (!(input = iobuf_shift_bytes(buf, &length))) return -1;
    EVP_PKEY *pkey = daemon_get_pkey(index);
    EVP_PKEY_CTX *ctx = pkey ? EVP_PKEY_CTX_new_from_pkey(NULL, pkey, "provider=default") : NULL;
    unsigned char output[NEVERBLEED_MAX_RSA_BYTES]; size_t output_length = sizeof(output);
    int ok = ctx && (decrypt ? EVP_PKEY_decrypt_init(ctx) : EVP_PKEY_sign_init(ctx)) > 0;
    if (ok && EVP_PKEY_is_a(pkey, "RSA")) {
        ok = EVP_PKEY_CTX_set_rsa_padding(ctx, (int)padding) > 0;
        if (ok && !decrypt && padding == RSA_PKCS1_PSS_PADDING) ok = EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx, (int)(int64_t)saltlen) > 0;
        if (ok && decrypt) {
            unsigned cv = cver, nv = nver, ir = implicit;
            OSSL_PARAM params[] = {OSSL_PARAM_uint(OSSL_ASYM_CIPHER_PARAM_TLS_CLIENT_VERSION, &cv),
                OSSL_PARAM_uint(OSSL_ASYM_CIPHER_PARAM_TLS_NEGOTIATED_VERSION, &nv),
                OSSL_PARAM_uint(OSSL_ASYM_CIPHER_PARAM_IMPLICIT_REJECTION, &ir), OSSL_PARAM_END};
            ok = EVP_PKEY_CTX_set_params(ctx, params) > 0;
        }
    }
    const EVP_MD *md = mdnid == SIZE_MAX ? NULL : EVP_get_digestbynid((int)mdnid);
    if (ok && !decrypt && mdnid != SIZE_MAX) ok = md && EVP_PKEY_CTX_set_signature_md(ctx, md) > 0;
    if (ok) ok = (decrypt ? EVP_PKEY_decrypt(ctx, output, &output_length, input, length) :
                           EVP_PKEY_sign(ctx, output, &output_length, input, length)) > 0;
    EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(pkey);
    iobuf_dispose(buf); iobuf_push_bytes(buf, output, ok ? output_length : 0);
    OPENSSL_cleanse(output, sizeof(output)); return 0;
}
