#ifndef HP_RUNTIME_ASSETS_H
#define HP_RUNTIME_ASSETS_H
#include <openssl/x509.h>

/* Process-startup only: call before PHP, helpers or execution threads. */
int hp_assets_init(void);
/* Add the pinned Mozilla bundle to one explicit outbound-client store.
 * Never call this for an inbound server/mutual-TLS context by default. */
int hp_assets_ca_store(X509_STORE *store);
/* Native-selected system/memfd fallback, or NULL when native defaults or an
 * explicit user override remain in control. The returned path is stable. */
const char *hp_assets_ca_path(void);
const char *hp_assets_ca_sha256(void);
const char *hp_assets_ca_source(void);
const char *hp_assets_ca_date(void);

#endif
