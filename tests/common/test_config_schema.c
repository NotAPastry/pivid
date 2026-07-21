#include "../common/config_schema.h"
#include "../common/recipe_defaults.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>  /* atol */
#include <string.h>

static const ConfigKeyMeta *find(const char *k) { return config_schema_lookup(k); }

static void test_routing(void) {
    const ConfigKeyMeta *m;
    m = find("BINDING_PHRASE");
    assert(m && m->tier == TIER_RESTART && strcmp(m->section, "pairing") == 0);
    assert(find("FLRC_MODE") == NULL);   /* knob removed: DYNAMIC is the only framing */
    m = find("FLRC_CR");
    assert(m && m->tier == TIER_MODRELOAD && m->type == FIELD_ENUM && m->enum_opts);
    m = find("FLRC_CRC_WIDTH");
    assert(m && m->tier == TIER_MODRELOAD && m->type == FIELD_ENUM && m->enum_opts);
    {   /* options are exactly the three CRC-on widths -- no OFF (IMP-5) */
        int n = 0, have_off = 0;
        for (const EnumOpt *o = m->enum_opts; o->value; o++) {
            if (strcmp(o->value, "0") == 0) have_off = 1;
            n++;
        }
        assert(n == 3 && !have_off);
    }
    /* RX_BOOST: RX-portal-only SetRxPath boost knob (IMP-3 prep). Only the
     * useful upper half 4..7 is offered (user-trimmed 2026-07-06: 0-3 sit at
     * or below the HF reference gain); modreload tier (consumed at probe). */
    m = find("RX_BOOST");
    assert(m && m->tier == TIER_MODRELOAD && m->role == ROLE_RX &&
           m->type == FIELD_ENUM && m->ui_class == UI_ADVANCED &&
           strcmp(m->section, "rf") == 0 && m->enum_opts);
    {
        int n = 0;
        for (const EnumOpt *o = m->enum_opts; o->value; o++, n++) {
            char want[2] = { (char)('4' + n), '\0' };
            assert(strcmp(o->value, want) == 0);   /* values are exactly "4".."7", in order */
        }
        assert(n == 4);
    }
    m = find("RF_FREQUENCY");
    assert(m && m->tier == TIER_MODRELOAD && m->type == FIELD_INT &&
           m->min == 1900000000L && m->max == 2500000000L);
    m = find("RF_TX_POWER");
    assert(m && m->tier == TIER_MODRELOAD && m->type == FIELD_INT && m->max == 12);
    assert(m->role == ROLE_TX);   /* one-way link: the RX never transmits (2026-07-06) */
    m = find("FEC_M_ANCHOR");
    assert(m && m->tier == TIER_LIVE && m->role == ROLE_TX);
    m = find("PIVID_AP_PASSWORD");
    assert(m && m->tier == TIER_NETCONFIG && strcmp(m->section, "network") == 0 &&
           m->ui_class == UI_BASIC && m->role == ROLE_BOTH);
    m = find("AQM");
    assert(m && m->tier == TIER_RESTART && m->role == ROLE_TX &&
           m->type == FIELD_ENUM && m->ui_class == UI_BASIC &&
           strcmp(m->section, "aqm") == 0 && m->enum_opts &&
           m->restart_unit && strcmp(m->restart_unit, "pivid-tx") == 0);
    int aqm_have_enforce = 0;
    for (const EnumOpt *o = m->enum_opts; o->value; o++)
        if (strcmp(o->value, "enforce") == 0) aqm_have_enforce = 1;
    assert(aqm_have_enforce);
    printf("test_routing: OK\n");
}

static void test_metadata_invariants(void) {
    int basic = 0;
    for (size_t i = 0; i < config_schema_len; i++) {
        const ConfigKeyMeta *m = &config_schema[i];
        assert(m->label && m->label[0]);            /* every key labelled */
        assert(m->description && m->description[0]); /* every key documented */
        if (m->type == FIELD_ENUM) {
            assert(m->enum_opts);
            int n = 0;
            for (const EnumOpt *o = m->enum_opts; o->value; o++) {
                assert(o->label && o->label[0]);
                n++;
            }
            assert(n >= 2);                          /* enums have >=2 options */
        } else {
            assert(m->enum_opts == NULL);
        }
        if (m->type == FIELD_INT && (m->min != 0 || m->max != 0))
            assert(m->min < m->max);                 /* sane ranges */
        if (m->ui_class == UI_BASIC) basic++;
        /* tier <-> restart_unit: only TIER_RESTART role-specific keys carry a unit */
        if (m->restart_unit) assert(m->tier == TIER_RESTART);
        if (m->tier == TIER_CASTER) assert(m->role == ROLE_RX);
        if (m->tier == TIER_PLAYER) assert(m->role == ROLE_RX);
    }
    /* Basic set is exactly: binding phrase, RF frequency, Forward-to-PC,
     * AP password, MSP telemetry enable, AQM toggle, local HDMI player,
     * composite output, composite TV standard. */
    assert(basic == 9);
    printf("test_metadata_invariants: OK (%zu keys, %d basic)\n", config_schema_len, basic);
}

static void test_schema_player_local(void) {
    {
        const ConfigKeyMeta *m = config_schema_lookup("PLAYER_LOCAL");
        assert(m != NULL);
        assert(m->role == ROLE_RX);
        assert(m->type == FIELD_ENUM);
        assert(m->tier == TIER_PLAYER);
        assert(strcmp(m->section, "player") == 0);
        printf("test_schema_player_local: OK\n");
    }
}

static void test_composite_keys_present(void) {
    const ConfigKeyMeta *m = config_schema_lookup("PLAYER_COMPOSITE");
    assert(m != NULL);
    assert(m->tier == TIER_BOOTCFG);
    assert(m->role == ROLE_RX);
    assert(m->type == FIELD_ENUM);

    const ConfigKeyMeta *t = config_schema_lookup("PLAYER_TV_STANDARD");
    assert(t != NULL);
    assert(t->tier == TIER_BOOTCFG);
    assert(t->role == ROLE_RX);
    assert(t->type == FIELD_ENUM);
    printf("test_composite_keys_present: OK\n");
}

/* No duplicate keys: config_schema_lookup returns the FIRST hit, so a copy-paste
 * duplicate would silently shadow the second row. */
static void test_no_duplicate_keys(void) {
    for (size_t i = 0; i < config_schema_len; i++)
        for (size_t j = i + 1; j < config_schema_len; j++)
            assert(strcmp(config_schema[i].key, config_schema[j].key) != 0);
    printf("test_no_duplicate_keys: OK\n");
}

/* Every opt_recipe enum value must have a defaults entry, carrying the 7
 * always-on keys; the conditional keys only under the right recipe. */
static int rd_has(const char *recipe, const char *key) {
    for (size_t i = 0; i < recipe_defaults_len; i++)
        if (strcmp(recipe_defaults[i].recipe, recipe) == 0 &&
            strcmp(recipe_defaults[i].key, key) == 0) return 1;
    return 0;
}
static const char *rd_val(const char *recipe, const char *key) {
    for (size_t i = 0; i < recipe_defaults_len; i++)
        if (strcmp(recipe_defaults[i].recipe, recipe) == 0 &&
            strcmp(recipe_defaults[i].key, key) == 0) return recipe_defaults[i].value;
    return NULL;
}
static int rd_shown(const char *recipe, const char *key) {
    for (size_t i = 0; i < recipe_defaults_len; i++)
        if (strcmp(recipe_defaults[i].recipe, recipe) == 0 &&
            strcmp(recipe_defaults[i].key, key) == 0) return recipe_defaults[i].shown;
    return -1;   /* not present at all */
}

static void test_recipe_defaults_coverage(void) {
    const ConfigKeyMeta *rm = config_schema_lookup("ENCODER_RECIPE");
    assert(rm && rm->enum_opts);
    const char *core[] = { "BITRATE","SLICE_MB","INTRA",
        "DROPPABLE_LEN","IDR_PERIOD",
        "FEC_M_ANCHOR","FEC_M_REF","FEC_K","SPS_REINJECT_MS" };
    for (const EnumOpt *e = rm->enum_opts; e->value; e++) {
        int seen = 0;
        for (size_t i = 0; i < recipe_defaults_len; i++)
            if (strcmp(recipe_defaults[i].recipe, e->value) == 0) seen = 1;
        assert(seen);  /* every recipe has at least one default */
        for (size_t k = 0; k < sizeof core / sizeof core[0]; k++)
            assert(rd_has(e->value, core[k]));  /* and all 9 core keys */
    }
    printf("test_recipe_defaults_coverage: OK\n");
}

/* FEC always on; every value within schema bounds; keys/recipes valid;
 * conditional-key applicability. */
static void test_recipe_defaults_invariants(void) {
    const ConfigKeyMeta *rm = config_schema_lookup("ENCODER_RECIPE");
    for (size_t i = 0; i < recipe_defaults_len; i++) {
        const RecipeDefault *d = &recipe_defaults[i];
        /* recipe is a valid enum value */
        int valid_recipe = 0;
        for (const EnumOpt *e = rm->enum_opts; e->value; e++)
            if (strcmp(e->value, d->recipe) == 0) valid_recipe = 1;
        assert(valid_recipe);
        /* key exists and is a TX key */
        const ConfigKeyMeta *m = config_schema_lookup(d->key);
        assert(m && m->role == ROLE_TX);
        /* value within [min,max] (skip the min==0&&max==0 "no range" sentinel) */
        long v = atol(d->value);
        if (!(m->min == 0 && m->max == 0)) { assert(v >= m->min && v <= m->max); }
        /* FEC always on */
        if (strcmp(d->key, "FEC_M_REF") == 0) assert(v >= 1);
        if (strcmp(d->key, "FEC_K") == 0) assert(v >= 1 && v <= 200);
    }
    /* every recipe lists every recipe-managed key now; visibility is the flag */
    assert(rd_has("droppable", "DROPPABLE_LEN") && rd_has("gop", "DROPPABLE_LEN"));
    assert(rd_has("droppable", "IDR_PERIOD")    && rd_has("gop", "IDR_PERIOD"));
    /* DROPPABLE_LEN shown only for droppable */
    assert(rd_shown("droppable", "DROPPABLE_LEN") == 1);
    assert(rd_shown("gop", "DROPPABLE_LEN") == 0);
    assert(rd_shown("gdr", "DROPPABLE_LEN") == 0);
    assert(rd_shown("gdr_idr", "DROPPABLE_LEN") == 0);
    /* IDR_PERIOD shown only for gdr_idr */
    assert(rd_shown("gdr_idr", "IDR_PERIOD") == 1);
    assert(rd_shown("droppable", "IDR_PERIOD") == 0);
    /* INTRA hidden for gdr_idr (it's the refresh window, unused once refresh is
     * dropped; gdr_idr's cadence knob is IDR_PERIOD). Shown for the recipes that
     * DO use --intra: gop/droppable (IDR period) and gdr (refresh window). */
    assert(rd_shown("gdr_idr", "INTRA") == 0);
    assert(rd_shown("gop", "INTRA") == 1);
    assert(rd_shown("gdr", "INTRA") == 1);
    assert(rd_shown("droppable", "INTRA") == 1);
    /* SPS reinject: value 0 + hidden on droppable; 1000 + shown elsewhere */
    assert(strcmp(rd_val("droppable", "SPS_REINJECT_MS"), "0") == 0);
    assert(rd_shown("droppable", "SPS_REINJECT_MS") == 0);
    assert(strcmp(rd_val("gop", "SPS_REINJECT_MS"), "1000") == 0);
    assert(rd_shown("gop", "SPS_REINJECT_MS") == 1);
    printf("test_recipe_defaults_invariants: OK\n");
}

static void test_recipe_defaults_json(void) {
    char buf[4096];
    int n = recipe_defaults_json_build(buf, sizeof buf);
    assert(n > 0 && (size_t)n < sizeof buf);
    assert(buf[0] == '{' && buf[n - 1] == '}');
    /* contains each recipe object and a known key/value pair */
    assert(strstr(buf, "\"droppable\":{"));
    assert(strstr(buf, "\"gop\":{"));
    assert(strstr(buf, "\"gdr\":{"));
    assert(strstr(buf, "\"gdr_idr\":{"));
    assert(strstr(buf, "\"FEC_M_REF\":{\"v\":\"4\",\"show\":true}"));   /* gdr's ref-P parity */
    assert(strstr(buf, "\"SPS_REINJECT_MS\":{\"v\":\"0\",\"show\":false}")); /* droppable, hidden */
    /* buffer safety: a too-small cap must report truncation, not overrun */
    char tiny[16];
    assert(recipe_defaults_json_build(tiny, sizeof tiny) == -1);
    printf("test_recipe_defaults_json: OK\n");
}

/* recipe_defaults_json_build groups *consecutive* rows by recipe, so the table
 * must keep every recipe's rows in one contiguous run -- otherwise a recipe
 * would be emitted as a duplicate JSON key and the dashboard would silently
 * keep only the last block. Guard that layout invariant. */
static void test_recipe_defaults_contiguous(void) {
    char seen[8][32]; size_t nseen = 0;
    const char *cur = NULL;
    for (size_t i = 0; i < recipe_defaults_len; i++) {
        const char *r = recipe_defaults[i].recipe;
        if (cur && strcmp(cur, r) == 0) continue;   /* same run, fine */
        /* recipe changed: it must not have appeared in an earlier run */
        for (size_t j = 0; j < nseen; j++) assert(strcmp(seen[j], r) != 0);
        assert(nseen < sizeof seen / sizeof seen[0]);
        snprintf(seen[nseen++], sizeof seen[0], "%s", r);
        cur = r;
    }
    printf("test_recipe_defaults_contiguous: OK\n");
}

int main(void) {
    test_routing();
    test_metadata_invariants();
    test_no_duplicate_keys();
    test_schema_player_local();
    test_composite_keys_present();

    /* New TX IDR-cadence key exists, is an int, TX role, restarts pivid-tx. */
    const ConfigKeyMeta *idr = config_schema_lookup("IDR_PERIOD");
    assert(idr != NULL);
    assert(idr->type == FIELD_INT);
    assert(idr->role == ROLE_TX);
    assert(strcmp(idr->restart_unit, "pivid-tx") == 0);
    /* Recipe enum now offers gdr_idr + gop. */
    const ConfigKeyMeta *rec = config_schema_lookup("ENCODER_RECIPE");
    assert(rec != NULL && rec->enum_opts != NULL);
    int have_gdr_idr = 0, have_gop = 0;
    for (const EnumOpt *o = rec->enum_opts; o->value; o++) {
        if (strcmp(o->value, "gdr_idr") == 0) have_gdr_idr = 1;
        if (strcmp(o->value, "gop") == 0) have_gop = 1;
    }
    assert(have_gdr_idr && have_gop);

    /* Removed keys must be gone (rx/caster split). */
    assert(config_schema_lookup("RX_MODE") == NULL);
    assert(config_schema_lookup("RX_UDP_ADDR") == NULL);
    assert(config_schema_lookup("RX_UDP_PORT") == NULL);
    assert(config_schema_lookup("OSD_ENABLED") == NULL);
    assert(config_schema_lookup("OSD_REENCODE_BITRATE") == NULL);
    assert(config_schema_lookup("OSD_IDR_PERIOD") == NULL);
    assert(config_schema_lookup("OSD_FONT_FACE") == NULL);
    assert(config_schema_lookup("CTL_RSSI_LOW") == NULL);
    assert(config_schema_lookup("VID_LQ_LOW") == NULL);
    assert(config_schema_lookup("FFMPEG_BIN") == NULL);

    /* Forward-to-PC is a caster-tier, RX-role, basic on/off toggle. */
    const ConfigKeyMeta *fwd = config_schema_lookup("FORWARD_TO_PC");
    assert(fwd != NULL);
    assert(fwd->tier == TIER_CASTER);
    assert(fwd->role == ROLE_RX);
    assert(fwd->type == FIELD_ENUM && fwd->enum_opts != NULL);
    assert(fwd->ui_class == UI_BASIC);
    assert(fwd->restart_unit == NULL);   /* caster tier carries no restart_unit */

    /* OSD DisplayPort keys. */
    const ConfigKeyMeta *kf = config_schema_lookup("OSD_KEYFRAME_MS");
    assert(kf != NULL && kf->type == FIELD_INT);
    assert(kf->min == 20 && kf->max == 5000);
    assert(kf->role == ROLE_TX && strcmp(kf->section, "osd") == 0);

    /* The player-font keys were retired with the embedded msp-osd font renderer
     * (the player no longer reads any atlas/smoothing config). */
    assert(config_schema_lookup("OSD_FONT_SMOOTH") == NULL);
    assert(config_schema_lookup("OSD_FONT") == NULL);

    const ConfigKeyMeta *rssi = config_schema_lookup("OSD_VID_RSSI_CELL");
    assert(rssi != NULL && rssi->type == FIELD_STR);
    assert(rssi->role == ROLE_RX && strcmp(rssi->section, "osd") == 0);

    const ConfigKeyMeta *lq = config_schema_lookup("OSD_VID_LQ_CELL");
    assert(lq != NULL && lq->type == FIELD_STR);
    assert(lq->role == ROLE_RX && strcmp(lq->section, "osd") == 0);

    test_recipe_defaults_coverage();
    test_recipe_defaults_invariants();
    test_recipe_defaults_json();
    test_recipe_defaults_contiguous();

    printf("All config_schema tests passed!\n");
    return 0;
}
