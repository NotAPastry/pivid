#ifndef AGENT_SCHEMA_JSON_H
#define AGENT_SCHEMA_JSON_H
#include <stddef.h>
#include "agent.h"
#include "settings.h"

/* Escape a string into JSON-string body bytes (no surrounding quotes). Escapes
 * \ " and control chars. Truncates to cap-1. Returns the written length. */
size_t schema_json_escape(const char *in, char *out, size_t cap);

/* Build the /api/schema JSON array (role-gated) into buf. Each element:
 * {key,label,section,type,ui_class,tier,role,description,must_match,min,max,
 *  enum:[{value,label}...]}. Returns bytes written, or -1 on overflow. */
int schema_json_build(AgentRole role, char *buf, size_t cap);

/* Build the /api/config values object {KEY:"value",...} (role-gated). Returns
 * bytes written, or -1 on overflow. */
int config_json_build(const Settings *s, AgentRole role, char *buf, size_t cap);

#endif
