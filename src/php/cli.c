#include "h2o_php.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <php.h>
#include <php_main.h>
#include <ext/standard/info.h>

bool hp_php_cli_should_dispatch(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/'); base = base ? base + 1 : argv[0];
    if (argc > 1 && (!strcmp(argv[1], "php-cli") || !strcmp(argv[1], "php-server") ||
                     !strcmp(argv[1], "serve") || !strcmp(argv[1], "effective-config"))) return false;
    if (!strcmp(base, "php") || !strcmp(base, "php-zts") || !strcmp(base, "h2o-php-cli")) return true;
    /* PHP_BINARY contains this executable. Preserve CLI recursion, including
     * ambiguous -v/-m/-c flags, without changing PHP's own constant or argv. */
    const char *child = getenv("H2O_PHP_CLI_CHILD");
    if (child && !strcmp(child, "1")) return true;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-m") && i + 1 < argc &&
            (!strcmp(argv[i + 1], "worker") || !strcmp(argv[i + 1], "master") ||
             !strcmp(argv[i + 1], "daemon") || !strcmp(argv[i + 1], "test"))) return false;
        if (!strcmp(arg, "--mode") || !strncmp(arg, "--mode=", 7) || !strcmp(arg, "--conf") ||
            !strncmp(arg, "--conf=", 7) || !strcmp(arg, "-t") || !strcmp(arg, "--test")) return false;
        if (!strcmp(arg, "-c")) { ++i; continue; }
        if (!strcmp(arg, "-h") || !strcmp(arg, "--help") || !strcmp(arg, "-v") || !strcmp(arg, "--version") ||
            !strcmp(arg, "--list-directives")) continue;
        if (!strcmp(arg, "--")) return i + 1 < argc;
        if (arg[0] != '-' || !arg[1]) return true; /* PHP file (including extensionless) or stdin */
        if (strchr("nrfdlibswSmaBeFRH", arg[1]) || !strncmp(arg, "--r", 3) || !strcmp(arg, "--ini")) return true;
    }
    return false;
}

#if defined(H2O_PHP_CLI_AVAILABLE) && H2O_PHP_CLI_AVAILABLE
#include <sapi/cli/cli.h>

ZEND_FUNCTION(frankenphp_log);
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(hp_cli_log_args, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, message, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, level, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, context, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()
static const zend_function_entry cli_functions[] = {
    ZEND_FE(frankenphp_log, hp_cli_log_args)
    ZEND_FE_END
};
static PHP_MINIT_FUNCTION(hp_cli)
{
    REGISTER_LONG_CONSTANT("FRANKENPHP_LOG_LEVEL_DEBUG", -4, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("FRANKENPHP_LOG_LEVEL_INFO", 0, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("FRANKENPHP_LOG_LEVEL_WARN", 4, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("FRANKENPHP_LOG_LEVEL_ERROR", 8, CONST_CS | CONST_PERSISTENT);
    return SUCCESS;
}
static PHP_MINFO_FUNCTION(hp_cli)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "FrankenPHP CLI compatibility", "native H2O / PHP CLI SAPI");
    php_info_print_table_end();
}
static zend_module_entry cli_module = {
    STANDARD_MODULE_HEADER,
    "frankenphp-cli", cli_functions,
    PHP_MINIT(hp_cli), NULL, NULL, NULL, PHP_MINFO(hp_cli),
    "h2o-native-0.2", STANDARD_MODULE_PROPERTIES
};
static int (*previous_extensions)(void);
static int register_cli_extension(void)
{
    if (previous_extensions && previous_extensions() != SUCCESS) return FAILURE;
    return zend_register_internal_module(&cli_module) ? SUCCESS : FAILURE;
}
int hp_php_cli_main(int argc, char **argv)
{
    if (setenv("H2O_PHP_CLI_CHILD", "1", 1)) return 70;
    if (hp_log_open(NULL)) return 70;
    previous_extensions = php_register_internal_extensions_func;
    php_register_internal_extensions_func = register_cli_extension;
    int status = do_php_cli(argc, argv);
    php_register_internal_extensions_func = previous_extensions;
    hp_log_close();
    return status;
}
#else
int hp_php_cli_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    fputs("php-cli requires matching PHP 8.6+ ZTS headers/library with exported do_php_cli; rebuild with H2O_PHP_ENABLE_CLI=ON\n", stderr);
    return 70;
}
#endif
