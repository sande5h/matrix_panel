#!/usr/bin/env bash
# Push firmware or a clip to the panel over wifi -- no USB cable, no idf.py.
#
#   tools/push.sh fw                       build/matrix_panel.bin, then reboots
#   tools/push.sh video demon.bin          upload and play
#   tools/push.sh video demon.bin --keep   upload, leave the current screen up
#   tools/push.sh status
#
# The host defaults to matrix-panel.local and can be overridden:
#   PANEL=192.168.1.99 tools/push.sh fw
# mDNS from this Mac is unreliable, so falling back to the IP is normal.
set -euo pipefail

PANEL="${PANEL:-matrix-panel.local}"
BASE="http://$PANEL:8088"
cd "$(dirname "$0")/.."

post() {  # post <url> <file>
    # --fail-with-body so the panel's own error text survives a 4xx, and
    # --max-time high enough for a few MB over wifi.
    curl --fail-with-body --max-time 300 --progress-bar \
         -H 'Content-Type: application/octet-stream' \
         --data-binary "@$2" "$1"
    echo
}

case "${1:-}" in
fw)
    BIN="${2:-build/matrix_panel.bin}"
    [ -f "$BIN" ] || { echo "no $BIN -- run idf.py build first" >&2; exit 1; }
    echo "flashing $(du -h "$BIN" | cut -f1) to $PANEL"
    post "$BASE/ota" "$BIN"
    echo "rebooting; give it about 10 s"
    ;;
video)
    BIN="${2:?usage: push.sh video <clip.bin> [--keep]}"
    Q="?play=1"; [ "${3:-}" = "--keep" ] && Q=""
    echo "uploading $(du -h "$BIN" | cut -f1) to $PANEL"
    post "$BASE/video$Q" "$BIN"
    ;;
status)
    curl -s "$BASE/status" | python3 -m json.tool
    ;;
reboot)
    curl -s -X POST "$BASE/reboot"; echo
    ;;
*)
    sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
