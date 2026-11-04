#ifndef HP_CONFIG_COMMANDS_H
#define HP_CONFIG_COMMANDS_H

#include <stdbool.h>
#include <stdio.h>
#include "h2o.h"

/* Call before H2O option parsing. -1 continues with possibly rewritten argv;
 * any nonnegative value is an immediate command exit status. */
int hp_config_command_prepare(int *argc, char ***argv);
bool hp_config_command_wants_dump(void);
/* Call immediately after the normal H2O configurator has applied the input,
 * before listeners, helper processes or the PHP engine are started. */
int hp_config_command_dump(const h2o_globalconf_t *global, FILE *output);

#endif
