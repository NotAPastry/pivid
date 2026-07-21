#include "../common/settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_defaults_valid(void) {
    Settings s; settings_load_from_file(&s, "/nonexistent");  /* defaults */
    char err[128] = {0};
    assert(settings_validate(&s, err, sizeof err) == 0);
    printf("test_defaults_valid: OK\n");
}

static void test_bad_recipe_rejected(void) {
    Settings s; settings_load_from_file(&s, "/nonexistent");
    strcpy(s.recipe, "banana");          /* not a valid recipe */
    char err[128] = {0};
    assert(settings_validate(&s, err, sizeof err) == -1);
    assert(strstr(err, "ENCODER_RECIPE") != NULL);
    printf("test_bad_recipe_rejected: OK\n");
}

static void test_check_file_bad_line(void) {
    FILE *f = fopen("/tmp/pivid_bad.conf", "w");
    assert(f); fputs("FPS=9999\n", f); fclose(f);
    char err[128] = {0};
    assert(settings_check_file("/tmp/pivid_bad.conf", err, sizeof err) == -1);
    assert(strstr(err, "FPS") != NULL);
    printf("test_check_file_bad_line: OK\n");
}

static void test_check_file_ok(void) {
    FILE *f = fopen("/tmp/pivid_ok.conf", "w");
    assert(f); fputs("FPS=30\nBITRATE=600000\n", f); fclose(f);
    char err[128] = {0};
    assert(settings_check_file("/tmp/pivid_ok.conf", err, sizeof err) == 0);
    printf("test_check_file_ok: OK\n");
}

/* settings_load_checked is what pivid-rx/tx reload_apply call on SIGHUP: it must
 * REJECT (return -1) an unreadable path and a file with a bad value for a known
 * key, and load a good file into the caller's Settings so a rejected reload can
 * keep the running config (keep-old) rather than apply garbage. */
static void test_load_checked_rejects(void) {
    Settings s; char err[128] = {0};
    /* (a) cannot open -> reject */
    assert(settings_load_checked(&s, "/nonexistent/pivid.conf", err, sizeof err) == -1);
    assert(err[0] != '\0');
    /* (b) known key, bad value -> reject */
    FILE *f = fopen("/tmp/pivid_lc_bad.conf", "w");
    assert(f); fputs("FPS=9999\n", f); fclose(f);
    err[0] = '\0';
    assert(settings_load_checked(&s, "/tmp/pivid_lc_bad.conf", err, sizeof err) == -1);
    assert(strstr(err, "FPS") != NULL);
    /* (c) good file -> accept + load the value */
    f = fopen("/tmp/pivid_lc_ok.conf", "w");
    assert(f); fputs("FPS=24\nBITRATE=700000\n", f); fclose(f);
    err[0] = '\0';
    assert(settings_load_checked(&s, "/tmp/pivid_lc_ok.conf", err, sizeof err) == 0);
    assert(s.fps == 24);
    printf("test_load_checked_rejects: OK\n");
}

int main(void) {
    test_defaults_valid(); test_bad_recipe_rejected();
    test_check_file_bad_line(); test_check_file_ok();
    test_load_checked_rejects();
    printf("test_settings_validate: ALL OK\n");
    return 0;
}
