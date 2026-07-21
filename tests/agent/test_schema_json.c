#include "../apps/pivid-config-agent/src/schema_json.h"
#include "../common/settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char buf[16384];

    /* Schema JSON for the TX role: an array; contains binding phrase + an enum. */
    int n = schema_json_build(AGENT_ROLE_TX, buf, sizeof buf);
    assert(n > 0);
    assert(buf[0] == '[');
    assert(strstr(buf, "\"key\":\"BINDING_PHRASE\""));
    assert(strstr(buf, "\"type\":\"enum\""));
    assert(strstr(buf, "\"ui_class\":\"basic\""));
    assert(strstr(buf, "\"tier\":\"modreload\""));
    assert(strstr(buf, "\"must_match\":true"));      /* flrc/pairing marker */
    /* RX-only key absent for TX role; TX-only present */
    assert(strstr(buf, "RX_MODE") == NULL);
    assert(strstr(buf, "BITRATE") != NULL);

    /* RF_TX_POWER is per-unit (TX-only role): no must-match badge on the TX
     * page -- only rf keys BOTH ends carry (the over-the-air framing set)
     * must match (user, 2026-07-06). */
    {
        const char *tp = strstr(buf, "\"key\":\"RF_TX_POWER\"");
        assert(tp != NULL);
        const char *mm = strstr(tp, "\"must_match\":");
        assert(mm != NULL && strncmp(mm, "\"must_match\":false", 18) == 0);
    }

    /* RX_BOOST is receiver-local (spec 2026-07-06): rendered for the RX role
     * with must_match FALSE -- the blanket rf-section badge must not apply,
     * or the UI would contradict the key's own "ends need NOT match" help. */
    n = schema_json_build(AGENT_ROLE_RX, buf, sizeof buf);
    assert(n > 0);
    {
        const char *rb = strstr(buf, "\"key\":\"RX_BOOST\"");
        assert(rb != NULL);
        const char *mm = strstr(rb, "\"must_match\":");
        assert(mm != NULL && strncmp(mm, "\"must_match\":false", 18) == 0);
    }
    /* ...and the TX-only PA knob is hidden from the RX page entirely. */
    assert(strstr(buf, "\"key\":\"RF_TX_POWER\"") == NULL);

    /* JSON escaping: a value with a quote is escaped. */
    char esc[64];
    schema_json_escape("a\"b\\c", esc, sizeof esc);
    assert(strcmp(esc, "a\\\"b\\\\c") == 0);

    /* Config JSON: object of key->value for current settings. */
    Settings s; settings_load_from_file(&s, "/nonexistent");
    n = config_json_build(&s, AGENT_ROLE_TX, buf, sizeof buf);
    assert(n > 0 && buf[0] == '{');
    assert(strstr(buf, "\"PIVID_AP_PASSWORD\":\"broadcast\""));
    assert(strstr(buf, "\"RF_FREQUENCY\":\"2100000000\""));  /* default (2100 MHz, on the 4 MHz FE-cal grid) */
    printf("test_schema_json: OK\n");
    return 0;
}
