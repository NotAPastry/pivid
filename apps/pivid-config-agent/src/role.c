#define _POSIX_C_SOURCE 200809L
#include "role.h"
#include <string.h>
#include <unistd.h>

AgentRole role_detect(void) {
    char host[256] = {0};
    if (gethostname(host, sizeof(host) - 1) != 0) return AGENT_ROLE_TX;
    return strstr(host, "rx") ? AGENT_ROLE_RX : AGENT_ROLE_TX;
}
const char *role_daemon_unit(AgentRole r) { return r == AGENT_ROLE_RX ? "pivid-rx" : "pivid-tx"; }
const char *role_status_file(AgentRole r) { return r == AGENT_ROLE_RX ? AGENT_RX_STATUS : AGENT_TX_STATUS; }
