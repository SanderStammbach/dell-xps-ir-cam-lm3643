#!/usr/bin/env bash
# Live view of the IR camera (HM1092).
# Starts ir-bridge -> v4l2loopback and shows the picture with ffplay.
# Closing the window (or q / Ctrl+C) also stops the bridge.
#
#   ./ir-view.sh            normal picture (bridge auto exposure)
#   ./ir-view.sh -s         stretch contrast (shows very dark pictures too)
#   ./ir-view.sh -s -- -L 0 remaining options go to ir-bridge (here: IR LED off)

set -u
DIR="$(dirname "$(readlink -f "$0")")"
DEV=/dev/video42          # v4l2loopback device
STRETCH=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -s) STRETCH=1; shift ;;
        -d) DEV="$2"; shift 2 ;;
        --) shift; break ;;
        *)  break ;;
    esac
done

if pgrep -x ir-bridge >/dev/null; then
    echo "ir-bridge is already running (pgrep -a ir-bridge)." >&2
    exit 1
fi

"$DIR/ir-bridge" -d "$DEV" -v "$@" &
BRIDGE=$!
trap 'kill -TERM $BRIDGE 2>/dev/null; wait $BRIDGE 2>/dev/null' EXIT INT TERM

# wait until the bridge delivers frames (exclusive_caps: only then a capture device)
for _ in $(seq 50); do
    v4l2-ctl -d "$DEV" --info 2>/dev/null | grep -q "Video Capture" && break
    kill -0 $BRIDGE 2>/dev/null || { echo "ir-bridge exited." >&2; exit 1; }
    sleep 0.1
done

VF="scale=iw*2:ih*2:flags=neighbor"
[[ $STRETCH == 1 ]] && VF="normalize=smoothing=15,$VF"

ffplay -hide_banner -loglevel warning -f v4l2 -i "$DEV" \
       -vf "$VF" -window_title "IR camera (HM1092)" -fflags nobuffer
