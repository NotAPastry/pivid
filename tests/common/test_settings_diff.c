#include "../common/settings.h"
#include "../common/config_schema.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int n_live; static char last_key[64], last_val[64];
static void cb(const char *k, const char *v, void *ud) {
    (void)ud; n_live++; snprintf(last_key,sizeof last_key,"%s",k); snprintf(last_val,sizeof last_val,"%s",v);
}

static void test_live_diff_only(void) {
    Settings cur; settings_load_from_file(&cur, "/nonexistent");
    Settings next = cur;
    next.fec_m_anchor = cur.fec_m_anchor + 1;   /* TIER_LIVE */
    next.width = cur.width + 16;                /* TIER_RESTART -> must NOT fire */
    n_live = 0;
    int c = settings_diff_by_tier(&cur, &next, TIER_LIVE, cb, NULL);
    assert(c == 1 && n_live == 1);
    assert(strcmp(last_key, "FEC_M_ANCHOR") == 0);
    printf("test_live_diff_only: OK\n");
}
int main(void){ test_live_diff_only(); printf("test_settings_diff: ALL OK\n"); return 0; }
