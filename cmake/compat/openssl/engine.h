/* Local declaration repair for the AlmaLinux 3.5.8 development package.
 * libcrypto retains functional ENGINE ABI although its headers hide it. */
#ifndef H2O_PHP_ENGINE_HEADER_REPAIR
#define H2O_PHP_ENGINE_HEADER_REPAIR
#include <openssl/opensslconf.h>
#undef OPENSSL_NO_ENGINE
#include "engine-upstream-3.5.8.h"
#endif
