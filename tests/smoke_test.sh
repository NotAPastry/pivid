#!/usr/bin/env bash
# tests/smoke_test.sh — hardware smoke test for the deployed PiVid system.
#
# Runs on the host workstation. ssh's into the TX and RX Pi's and runs
# read-only checks against the running daemons + driver. No state is
# modified on the Pi's; the bind subcommand triggers a sync-word change
# by prompting the operator to press physical buttons.
#
# Subcommands:
#   static   per-Pi checks: binary present, driver loaded, /dev/radio0
#            exists, service active, no recent errors in journal.
#   stream   sample stats heartbeats twice (6 s apart) and verify
#            counters advance: TX `sent` MUST grow, RX `nals` MUST grow,
#            `dropped_ioctl` MUST stay flat.
#   bind     interactive: record pre-bind sync words, prompt the operator
#            to long-press RX then TX bind buttons, wait, re-read and
#            assert post-bind matches across sides AND differs from pre.
#   all      runs static + stream (default).
#
# Env (override on command line, e.g. TX_HOST=<tx-pi-ip> ./smoke_test.sh):
#   TX_HOST       hostname/IP of TX Pi   (default: pivid-tx)
#   RX_HOST       hostname/IP of RX Pi   (default: pivid-rx)
#   DEPLOY_USER   ssh login user         (default: root)
#
# Pre-conditions:
#   - Tasks 27 + 28 deployed; the Pi has been rebooted at least once
#     after the DT overlay was installed.
#   - pivid-tx.service is active and a camera is attached on the TX Pi.
#   - pivid-rx.service is active on the RX Pi.
set -euo pipefail

TX_HOST="${TX_HOST:-pivid-tx}"
RX_HOST="${RX_HOST:-pivid-rx}"
DEPLOY_USER="${DEPLOY_USER:-root}"

# Heartbeat is 5 s; sample twice 6 s apart to guarantee a fresh line.
STREAM_SAMPLE_SEC=6

# --- output helpers ---------------------------------------------------------

c_pass=$'\033[32m'; c_fail=$'\033[31m'; c_warn=$'\033[33m'; c_off=$'\033[0m'
# Disable colours if not on a TTY.
[[ -t 1 ]] || { c_pass=; c_fail=; c_warn=; c_off=; }

pass() { printf '  %sPASS%s  %s\n' "$c_pass" "$c_off" "$*"; }
fail() { printf '  %sFAIL%s  %s\n' "$c_fail" "$c_off" "$*"; FAILED=1; }
warn() { printf '  %sWARN%s  %s\n' "$c_warn" "$c_off" "$*"; }
section() { printf '\n=== %s ===\n' "$*"; }

FAILED=0
trap 'exit $FAILED' EXIT

# --- ssh helper -------------------------------------------------------------

ssh_cmd() {
    local host="$1"; shift
    ssh -o BatchMode=yes -o ConnectTimeout=10 "${DEPLOY_USER}@${host}" "$@"
}

# --- static checks ----------------------------------------------------------

static_one() {
    local host="$1" role="$2"
    local bin="/opt/pivid/pivid-${role}"
    local svc="pivid-${role}.service"

    section "static: ${host} (${role})"

    if ssh_cmd "$host" "[ -x $bin ]"; then
        pass "binary $bin present and executable"
    else
        fail "binary $bin missing or not executable"
    fi

    if ssh_cmd "$host" "lsmod | grep -qE '^lr2021 '"; then
        pass "lr2021 driver loaded"
    else
        fail "lr2021 driver NOT loaded (modprobe lr2021?)"
    fi

    if ssh_cmd "$host" "[ -c /dev/radio0 ]"; then
        pass "/dev/radio0 exists"
    else
        fail "/dev/radio0 missing (DT overlay not applied? need reboot?)"
    fi

    # dmesg may not be world-readable on a hardened image; tolerate
    # permission denied with a WARN rather than failing the check.
    local dmesg_out
    if dmesg_out="$(ssh_cmd "$host" "dmesg -t 2>/dev/null | grep -i lr2021 | tail -20")"; then
        if [[ -z "$dmesg_out" ]]; then
            warn "no lr2021 messages in dmesg (boot ring may have rolled)"
        elif echo "$dmesg_out" | grep -qiE 'fail|error|warn'; then
            fail "lr2021 driver reported errors in dmesg:"
            echo "$dmesg_out" | sed 's/^/         /'
        else
            pass "lr2021 dmesg clean (last $(echo "$dmesg_out" | wc -l) line(s))"
        fi
    else
        warn "could not read dmesg (insufficient permission?)"
    fi

    local svc_state
    svc_state="$(ssh_cmd "$host" "systemctl is-active $svc 2>/dev/null || true")"
    case "$svc_state" in
        active)        pass "$svc is active" ;;
        activating)    warn "$svc is still activating" ;;
        inactive|failed)
            fail "$svc is $svc_state — recent logs:"
            ssh_cmd "$host" "journalctl -u $svc -n 15 --no-pager" | sed 's/^/         /'
            ;;
        *)             fail "$svc unexpected state: $svc_state" ;;
    esac
}

cmd_static() {
    static_one "$TX_HOST" tx
    static_one "$RX_HOST" rx
}

# --- stream checks ----------------------------------------------------------

# Extract a numeric field "$2=NNN" from "$1" (journalctl text). Prints 0
# if not found, so missing-heartbeat cases produce a clear delta=0.
extract_counter() {
    local text="$1" key="$2"
    awk -v k="$key" '
        match($0, k"=[0-9]+") {
            v = substr($0, RSTART + length(k) + 1, RLENGTH - length(k) - 1)
        }
        END { print v + 0 }
    ' <<<"$text"
}

# Pull the latest heartbeat line from a service's recent journal.
latest_heartbeat() {
    local host="$1" svc="$2" pattern="$3"
    # Last 50 lines should always include at least one 5 s heartbeat.
    ssh_cmd "$host" "journalctl -u $svc -n 50 --no-pager" \
        | grep -E "$pattern" | tail -1 || true
}

cmd_stream() {
    section "stream: warming up (${STREAM_SAMPLE_SEC}s between samples)"

    local tx_a tx_b rx_a rx_b
    tx_a="$(latest_heartbeat "$TX_HOST" pivid-tx.service '^.*pivid-tx: sent=')"
    rx_a="$(latest_heartbeat "$RX_HOST" pivid-rx.service '^.*pivid-rx: nals=')"

    if [[ -z "$tx_a" ]]; then
        fail "no TX heartbeat in recent journal (service idle? camera missing?)"
        return
    fi
    if [[ -z "$rx_a" ]]; then
        fail "no RX heartbeat in recent journal (service idle?)"
        return
    fi

    sleep "$STREAM_SAMPLE_SEC"

    tx_b="$(latest_heartbeat "$TX_HOST" pivid-tx.service '^.*pivid-tx: sent=')"
    rx_b="$(latest_heartbeat "$RX_HOST" pivid-rx.service '^.*pivid-rx: nals=')"

    local tx_sent_a tx_sent_b tx_drop_a tx_drop_b
    tx_sent_a="$(extract_counter "$tx_a" sent)"
    tx_sent_b="$(extract_counter "$tx_b" sent)"
    tx_drop_a="$(extract_counter "$tx_a" dropped_ioctl)"
    tx_drop_b="$(extract_counter "$tx_b" dropped_ioctl)"

    local rx_nals_a rx_nals_b
    rx_nals_a="$(extract_counter "$rx_a" nals)"
    rx_nals_b="$(extract_counter "$rx_b" nals)"

    local d_sent=$(( tx_sent_b - tx_sent_a ))
    local d_drop=$(( tx_drop_b - tx_drop_a ))
    local d_nals=$(( rx_nals_b - rx_nals_a ))

    if (( d_sent > 0 )); then
        pass "TX packets advancing: +$d_sent in ${STREAM_SAMPLE_SEC}s"
    else
        fail "TX packets not advancing (sent flat at $tx_sent_a)"
    fi

    if (( d_drop == 0 )); then
        pass "TX dropped_ioctl flat (driver healthy)"
    else
        fail "TX dropped_ioctl grew by $d_drop (driver rejecting writes!)"
    fi

    if (( d_nals > 0 )); then
        pass "RX NALs advancing: +$d_nals in ${STREAM_SAMPLE_SEC}s"
    else
        fail "RX NALs not advancing (radio link broken? wrong sync_word?)"
    fi
}

# --- pairing (ELRS-style binding phrase) -----------------------------------

# Reads BINDING_PHRASE= from /opt/pivid/pivid.conf, with surrounding
# double quotes stripped (the serializer quotes string values).
read_binding_phrase() {
    local host="$1"
    ssh_cmd "$host" "grep '^BINDING_PHRASE=' /opt/pivid/pivid.conf 2>/dev/null \
                       || echo BINDING_PHRASE=" \
        | sed -e 's/^BINDING_PHRASE=//' -e 's/^\"//' -e 's/\"$//'
}

# Reads the sync word the daemon derived at startup, from its journal line
# "started, sync_word=0x........" (lowercased). Empty if not found.
read_derived_sync_word() {
    local host="$1" unit="$2"
    ssh_cmd "$host" "journalctl -u $unit --no-pager 2>/dev/null \
                       | grep -o 'sync_word=0x[0-9A-Fa-f]*' | tail -1" \
        | sed 's/^sync_word=//' | tr 'A-F' 'a-f'
}

# Pairing is now deterministic: identical BINDING_PHRASE on both ends
# derives the same radio sync word (no over-the-air handshake). This check
# confirms the phrases match and that each daemon logged the same derived word.
cmd_pair() {
    section "pair: comparing binding phrases + derived sync words"

    local tx_phrase rx_phrase
    tx_phrase="$(read_binding_phrase "$TX_HOST")"
    rx_phrase="$(read_binding_phrase "$RX_HOST")"

    if [[ -z "$tx_phrase" || -z "$rx_phrase" ]]; then
        fail "could not read BINDING_PHRASE from one or both pivid.conf files"
        return
    fi

    printf '  phrase: TX=%q  RX=%q\n' "$tx_phrase" "$rx_phrase"
    if [[ "$tx_phrase" == "$rx_phrase" ]]; then
        pass "binding phrases match across sides"
    else
        fail "binding phrases DIFFER -> link cannot pair (set the same phrase on both)"
    fi

    local tx_sw rx_sw
    tx_sw="$(read_derived_sync_word "$TX_HOST" pivid-tx)"
    rx_sw="$(read_derived_sync_word "$RX_HOST" pivid-rx)"
    printf '  derived sync_word: TX=%s  RX=%s\n' "${tx_sw:-<none>}" "${rx_sw:-<none>}"

    if [[ -z "$tx_sw" || -z "$rx_sw" ]]; then
        warn "no 'sync_word=0x...' startup line in one journal (daemon not started yet?)"
    elif [[ "$tx_sw" == "$rx_sw" ]]; then
        pass "derived sync words match ($tx_sw)"
    else
        fail "derived sync words DIFFER (TX=$tx_sw RX=$rx_sw) -- phrases out of sync?"
    fi
}

# --- dispatch ---------------------------------------------------------------

usage() {
    cat >&2 <<EOF
usage: $0 [static|stream|pair|all]

  static   per-Pi: binary, driver, /dev/radio0, service, recent log
  stream   verify TX sent + RX nals counters advance, dropped_ioctl flat
  pair     compare BINDING_PHRASE + derived sync_word across both Pis
  all      static + stream (default)

env: TX_HOST=$TX_HOST RX_HOST=$RX_HOST DEPLOY_USER=$DEPLOY_USER
EOF
    exit 2
}

case "${1:-all}" in
    static) cmd_static ;;
    stream) cmd_stream ;;
    pair)   cmd_pair ;;
    all)    cmd_static; cmd_stream ;;
    -h|--help|help) usage ;;
    *)      echo "unknown subcommand: $1" >&2; usage ;;
esac
