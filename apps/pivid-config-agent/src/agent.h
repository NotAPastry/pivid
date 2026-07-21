#ifndef AGENT_H
#define AGENT_H

typedef enum { AGENT_ROLE_TX, AGENT_ROLE_RX } AgentRole;

#define AGENT_PIVID_CONF       "/opt/pivid/pivid.conf"
#define AGENT_MODPROBE_CONF    "/etc/modprobe.d/lr2021.conf"
#define AGENT_CONFIG_MODE_FLAG "/run/pivid/config-mode"
#define AGENT_TX_STATUS        "/run/pivid/tx.status"
#define AGENT_RX_STATUS        "/run/pivid/rx.status"
#define AGENT_TX_PIDFILE       "/run/pivid/pivid-tx.pid"
#define AGENT_RX_PIDFILE       "/run/pivid/pivid-rx.pid"
#define AGENT_NET_UNIT         "pivid-config-net.service"
#define AGENT_WIFI_HELPER      "/opt/pivid/pivid-wifi-client.sh"
#define AGENT_DISPLAY_HELPER   "/opt/pivid/pivid-display.sh"
#define AGENT_HTTP_PORT        "80"

/* Config file the agent reads and writes at runtime. Defined in main.o and set
 * once from --config (default AGENT_PIVID_CONF) BEFORE any route is registered,
 * so the civetweb worker threads only ever observe the final value. Every
 * settings load/save in http.c goes through this, not through the compiled-in
 * SETTINGS_FILE default -- that is what makes --config testable off-target. */
extern const char *g_agent_conf_path;

/* Build stamp injected by the Makefile (-DBUILD_VERSION=<git short hash>).
 * Fallback keeps a bare/IDE compile working. */
#ifndef BUILD_VERSION
#define BUILD_VERSION "unknown"
#endif

#endif
