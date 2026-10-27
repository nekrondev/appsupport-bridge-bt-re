#!/bin/sh
# run-temp.sh — temporarily run the reimplemented AppSupport BT bridge in place of
# Jolla's, WITHOUT changing anything on disk. Stops Jolla's bridge, runs ours, bounces
# container BT so it attaches to ours, then tails the log. On Ctrl-C (or any exit) it
# kills ours and restarts Jolla's bridge, leaving the phone exactly as it was.
#
# Run as root:   devel-su sh /home/defaultuser/run-temp.sh
#
# Override defaults via env, e.g.:  BIN=/tmp/abbr ABBR_DEBUG=0 devel-su sh run-temp.sh
set -u

BIN="${BIN:-/home/defaultuser/abbr}"          # the reimplemented bridge binary
SVC="appsupport-bridge-bt.service"            # Jolla's bridge service
LOG="${LOG:-/tmp/abbr.log}"
DEBUG="${ABBR_DEBUG:-1}"

# Detect the AppSupport LXC container (name + path) so we can toggle container BT.
LXC_NAME="$(ps aux 2>/dev/null | sed -n 's/.*lxc-start.*-n \([^ ]*\).*/\1/p' | head -1)"
LXC_PATH="$(ps aux 2>/dev/null | sed -n 's/.*lxc-start.*--lxcpath=\([^ ]*\).*/\1/p' | head -1)"
LXC_NAME="${LXC_NAME:-defaultuser}"
LXC_PATH="${LXC_PATH:-/tmp/appsupport}"
LXC="lxc-attach -n $LXC_NAME -P $LXC_PATH --"

[ "$(id -u)" = 0 ] || { echo "ERROR: run as root (devel-su)"; exit 1; }
[ -x "$BIN" ] || { echo "ERROR: bridge binary not found/executable: $BIN"; exit 1; }

container_bt() { $LXC cmd bluetooth_manager "$1" >/dev/null 2>&1; }

PID=""
DONE=0
restore() {
    [ "$DONE" = 1 ] && return; DONE=1
    echo
    echo "[restore] stopping reimpl bridge + restoring Jolla's..."
    [ -n "$PID" ] && kill "$PID" 2>/dev/null
    sleep 1; [ -n "$PID" ] && kill -9 "$PID" 2>/dev/null
    systemctl start "$SVC" 2>/dev/null
    sleep 1
    container_bt disable; sleep 1; container_bt enable
    echo "[restore] Jolla bridge: $(systemctl is-active "$SVC" 2>/dev/null). Done."
}
trap restore EXIT
trap 'exit 0' INT TERM

echo "[1/4] stopping Jolla bridge ($SVC)"
systemctl stop "$SVC"; sleep 1

echo "[2/4] launching reimpl bridge: $BIN  (log: $LOG, debug=$DEBUG)"
: > "$LOG"
ABBR_DEBUG="$DEBUG" "$BIN" >"$LOG" 2>&1 &
PID=$!
sleep 3
if ! kill -0 "$PID" 2>/dev/null; then
    echo "ERROR: reimpl bridge exited immediately:"; cat "$LOG"; exit 1
fi

echo "[3/4] bouncing container BT so it attaches to the reimpl bridge"
container_bt disable; sleep 2
$LXC am force-stop com.android.bluetooth >/dev/null 2>&1; sleep 1
container_bt enable; sleep 6

echo "[4/4] READY — reimpl bridge running (pid $PID); AppSupport BT should be ON."
echo "      Test your scenario now (advertising, pushTAN, etc.)."
echo "      Press Ctrl-C to stop and restore Jolla's bridge."
echo "--------------------------- live log ---------------------------"
tail -f "$LOG"
