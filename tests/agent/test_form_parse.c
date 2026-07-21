#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../apps/pivid-config-agent/src/form_parse.h"

int main(void) {
    char dec[64];
    int n = form_url_decode("tall%2Dorange+falcon", 20, dec, sizeof dec);
    assert(n == 18 && strcmp(dec, "tall-orange falcon") == 0);

    Settings s; settings_load_from_file(&s, "/nonexistent");
    int seen = form_parse_apply(
        "BITRATE=1200000&FEC_M_ANCHOR=4&ENCODER_RECIPE=gdr&NOPE=x", &s);
    assert(seen == 3);
    assert(s.bitrate == 1200000 && s.fec_m_anchor == 4);
    assert(strcmp(s.recipe, "gdr") == 0);

    /* unknown key present -> ignored, not counted */
    Settings s2; settings_load_from_file(&s2, "/nonexistent");
    int seen2 = form_parse_apply("UNKNOWN_KEY=999&WIDTH=1280", &s2);
    assert(seen2 == 1 && s2.width == 1280);
    printf("All form_parse tests passed!\n");
    return 0;
}
