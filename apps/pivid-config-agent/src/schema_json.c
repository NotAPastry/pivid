#define _POSIX_C_SOURCE 200809L
#include "schema_json.h"
#include "config_schema.h"
#include <stdio.h>
#include <string.h>

size_t schema_json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    if (cap == 0) return 0;
    for (const char *p = in ? in : ""; *p && o + 7 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n')        { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r')        { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t')        { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20)         { o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c); }
        else                       { out[o++] = (char)c; }
    }
    out[o] = '\0';
    return o;
}

static int role_gated(ConfigRole kr, AgentRole ar) {
    if (kr == ROLE_BOTH) return 1;
    if (kr == ROLE_TX)   return ar == AGENT_ROLE_TX;
    return ar == AGENT_ROLE_RX;
}

static const char *type_str(FieldType t) {
    switch (t) { case FIELD_INT: return "int"; case FIELD_ENUM: return "enum";
                 default: return "str"; }
}
static const char *tier_str(ApplyTier t) {
    /* "reload" not "restart": a daemon service reload, not an OS reboot.
     * TIER_CASTER presents as "live" -- to the user it is a no-drop live apply. */
    switch (t) { case TIER_LIVE: return "live"; case TIER_RESTART: return "reload";
                 case TIER_MODRELOAD: return "modreload";
                 case TIER_CASTER: return "live";
                 case TIER_PLAYER: return "service";
                 case TIER_BOOTCFG: return "reboot";
                 default: return "netconfig"; }
}
static const char *role_str(ConfigRole r) {
    switch (r) { case ROLE_TX: return "tx"; case ROLE_RX: return "rx";
                 default: return "both"; }
}

/* A change to a pairing or chip/flrc key must be mirrored on the other unit.
 * Only rf keys BOTH ends carry qualify: a single-role rf key (e.g. RX_BOOST,
 * receiver-local front-end gain) has no other end to match. */
static int must_match(const ConfigKeyMeta *m) {
    return strcmp(m->section, "pairing") == 0 ||
           (strcmp(m->section, "rf") == 0 && m->role == ROLE_BOTH);
}

int schema_json_build(AgentRole role, char *buf, size_t cap) {
    size_t o = 0;
    char esc[512];
    #define APP(...) do { int _w = snprintf(buf + o, cap - o, __VA_ARGS__); \
        if (_w < 0 || (size_t)_w >= cap - o) { return -1; } o += (size_t)_w; } while (0)
    APP("[");
    int first = 1;
    for (size_t i = 0; i < config_schema_len; i++) {
        const ConfigKeyMeta *m = &config_schema[i];
        if (!role_gated(m->role, role)) continue;
        APP("%s{", first ? "" : ",");
        first = 0;
        APP("\"key\":\"%s\",", m->key);
        schema_json_escape(m->label, esc, sizeof esc);       APP("\"label\":\"%s\",", esc);
        APP("\"section\":\"%s\",", m->section);
        APP("\"type\":\"%s\",", type_str(m->type));
        APP("\"ui_class\":\"%s\",", m->ui_class == UI_BASIC ? "basic" : "advanced");
        APP("\"tier\":\"%s\",", tier_str(m->tier));
        APP("\"role\":\"%s\",", role_str(m->role));
        schema_json_escape(m->description, esc, sizeof esc); APP("\"description\":\"%s\",", esc);
        APP("\"must_match\":%s,", must_match(m) ? "true" : "false");
        APP("\"min\":%ld,\"max\":%ld,", m->min, m->max);
        APP("\"enum\":[");
        if (m->enum_opts) {
            int ef = 1;
            for (const EnumOpt *e = m->enum_opts; e->value; e++) {
                schema_json_escape(e->label, esc, sizeof esc);
                APP("%s{\"value\":\"%s\",\"label\":\"%s\"}", ef ? "" : ",", e->value, esc);
                ef = 0;
            }
        }
        APP("]}");
    }
    APP("]");
    #undef APP
    return (int)o;
}

int config_json_build(const Settings *s, AgentRole role, char *buf, size_t cap) {
    size_t o = 0;
    char raw[256], esc[512];
    #define APP(...) do { int _w = snprintf(buf + o, cap - o, __VA_ARGS__); \
        if (_w < 0 || (size_t)_w >= cap - o) { return -1; } o += (size_t)_w; } while (0)
    APP("{");
    int first = 1;
    for (size_t i = 0; i < config_schema_len; i++) {
        const ConfigKeyMeta *m = &config_schema[i];
        if (!role_gated(m->role, role)) continue;
        if (settings_get_field_str(s, m->key, raw, sizeof raw) != 0) continue;
        schema_json_escape(raw, esc, sizeof esc);
        APP("%s\"%s\":\"%s\"", first ? "" : ",", m->key, esc);
        first = 0;
    }
    APP("}");
    #undef APP
    return (int)o;
}
