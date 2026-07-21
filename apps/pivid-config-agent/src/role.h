#ifndef AGENT_ROLE_H
#define AGENT_ROLE_H
#include "agent.h"
/* RX if the hostname contains "rx", else TX (mirrors pivid-config-net.sh). */
AgentRole role_detect(void);
const char *role_daemon_unit(AgentRole r);   /* "pivid-tx" / "pivid-rx" */
const char *role_status_file(AgentRole r);    /* AGENT_TX_STATUS / RX */
#endif
