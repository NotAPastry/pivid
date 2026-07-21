#include "../common/cli_args.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define DEF "/opt/pivid/pivid.conf"

static void test_default_config(void) {
    char *argv[] = { "pivid-tx", NULL };
    CliArgs a;
    assert(cli_parse(1, argv, DEF, &a) == 0);
    assert(strcmp(a.config_path, DEF) == 0);
    assert(!a.check && !a.show_help && !a.show_version);
    printf("test_default_config: OK\n");
}

static void test_config_value(void) {
    char *argv[] = { "pivid-tx", "--config", "/tmp/x.conf", NULL };
    CliArgs a;
    assert(cli_parse(3, argv, DEF, &a) == 0);
    assert(strcmp(a.config_path, "/tmp/x.conf") == 0);
    printf("test_config_value: OK\n");
}

static void test_flags(void) {
    char *h[] = { "p", "--help", NULL };       CliArgs a;
    assert(cli_parse(2, h, DEF, &a) == 0 && a.show_help);
    char *v[] = { "p", "--version", NULL };
    assert(cli_parse(2, v, DEF, &a) == 0 && a.show_version);
    char *c[] = { "p", "--check", NULL };
    assert(cli_parse(2, c, DEF, &a) == 0 && a.check);
    char *sh[] = { "p", "-h", NULL };
    assert(cli_parse(2, sh, DEF, &a) == 0 && a.show_help);
    printf("test_flags: OK\n");
}

static void test_errors(void) {
    char *unk[] = { "p", "--nope", NULL };     CliArgs a;
    assert(cli_parse(2, unk, DEF, &a) == -1);          /* unknown flag */
    char *noval[] = { "p", "--config", NULL };
    assert(cli_parse(2, noval, DEF, &a) == -1);        /* missing value */
    printf("test_errors: OK\n");
}

int main(void) {
    test_default_config(); test_config_value(); test_flags(); test_errors();
    printf("test_cli_args: ALL OK\n");
    return 0;
}
