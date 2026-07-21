#ifndef AGENT_HTTP_H
#define AGENT_HTTP_H
#include "civetweb.h"
#include "agent.h"
/* Register all routes on `ctx`. `role` selects which sections render + which
 * daemon the apply targets. */
void http_register(struct mg_context *ctx, AgentRole role);
#endif
