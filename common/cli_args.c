#include "cli_args.h"
#include <string.h>

int cli_parse(int argc, char **argv, const char *def_config, CliArgs *out) {
    out->config_path = def_config;
    out->check = out->show_help = out->show_version = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help"))      out->show_help = true;
        else if (!strcmp(a, "--version"))                  out->show_version = true;
        else if (!strcmp(a, "--check"))                    out->check = true;
        else if (!strcmp(a, "--config")) {
            if (++i >= argc) return -1;                    /* missing value */
            out->config_path = argv[i];
        } else return -1;                                  /* unknown flag */
    }
    return 0;
}

void cli_print_usage(FILE *f, const char *prog) {
    fprintf(f,
        "Usage: %s [options]\n"
        "  --config <path>   config file (default compiled in)\n"
        "  --check           validate the config file and exit (opens no device)\n"
        "  --version         print version and exit\n"
        "  -h, --help        show this help\n"
        "Advanced/debug env vars: see docs/hidden-env-knobs.md\n",
        prog);
}
