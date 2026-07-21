#ifndef COMMON_CLI_ARGS_H
#define COMMON_CLI_ARGS_H
#include <stdbool.h>
#include <stdio.h>

/* Common CLI surface shared by the four daemons. The player has its own
 * parser (separate schema) and does not use this. */
typedef struct {
    const char *config_path;   /* --config value, or def_config if absent */
    bool        check;         /* --check: validate config + exit         */
    bool        show_help;     /* -h / --help                              */
    bool        show_version;  /* --version                                */
} CliArgs;

/* Parse argv. def_config is the binary's default --config path. Returns 0 on
 * success (caller then acts on show_help/show_version/check), -1 on a usage
 * error (caller prints usage to stderr and exits 2). Unknown flags and a
 * --config with no value are usage errors. */
int cli_parse(int argc, char **argv, const char *def_config, CliArgs *out);

/* Print the shared switch list to f, prefixed with `prog`. Ends with the
 * hidden-knobs pointer line. Lists only real switches. */
void cli_print_usage(FILE *f, const char *prog);

#endif
