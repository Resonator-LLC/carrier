#!/bin/bash
#
# webcam_server.sh — Carrier webcam streaming server.
#
# Runs carrier-cli persistently, auto-accepts all friend requests,
# and streams the webcam when a friend opens a pipe.
#
# The friend triggers the stream simply by opening a pipe:
#   carrier-cli -p friend.tox --pipe 0 | ffplay -f mjpeg -
#
# Usage:
#   ./webcam_server.sh [profile.tox]
#

PROFILE="${1:-$HOME/resonator/webcam.tox}"
CLI="${CLI:-$HOME/resonator/carrier/build/carrier-cli}"
DEVICE="${DEVICE:-/dev/video0}"
RESOLUTION="${RESOLUTION:-640x480}"
FPS="${FPS:-15}"
PIDFILE="/tmp/webcam_server.pid"

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

# Prevent double-start
if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    log "Already running (PID $(cat "$PIDFILE")). Kill it first."
    exit 1
fi
echo $$ > "$PIDFILE"

cleanup() {
    [ -n "${WRITER_PID:-}" ] && kill "$WRITER_PID" 2>/dev/null
    [ -n "${FFMPEG_PID:-}" ] && kill "$FFMPEG_PID" 2>/dev/null
    rm -f "$CMD_FIFO" "$PIDFILE"
}
trap cleanup EXIT INT TERM

extract() {
    local pred="$1" line="$2"
    echo "$line" | sed -n "s/.*carrier:$pred \([^ ;.]*\).*/\1/p"
}

# --- Main loop: restart after each stream ends ---
while true; do
    CMD_FIFO=$(mktemp -u /tmp/carrier_cam.XXXXXX)
    mkfifo "$CMD_FIFO"

    log "Starting carrier (profile: $PROFILE)"

    # Background writer keeps the FIFO open and sends initial setup
    {
        echo '[] a carrier:Nick ; carrier:nick "RaspiCam" .'
        echo '[] a carrier:StatusMessage ; carrier:message "Open a pipe to get webcam stream" .'
        while kill -0 $$ 2>/dev/null; do sleep 60; done
    } > "$CMD_FIFO" &
    WRITER_PID=$!

    # Run carrier-cli, process events line by line
    "$CLI" -p "$PROFILE" --fifo-in="$CMD_FIFO" 2>/dev/null | \
    while IFS= read -r line; do
        log "$line"

        case "$line" in
            *"carrier:FriendRequest"*)
                req_id=$(extract "requestId" "$line")
                if [ -n "$req_id" ]; then
                    log "Accepting friend request #$req_id"
                    echo "[] a carrier:FriendAccept ; carrier:requestId $req_id ." > "$CMD_FIFO"
                fi
                ;;

            *"a carrier:Pipe"*)
                friend_id=$(extract "friendId" "$line")
                if [ -n "$friend_id" ]; then
                    log "Pipe opened by friend #$friend_id — streaming webcam"
                    # Friend already opened a pipe to us. Enter pipe mode
                    # back to them and start sending webcam data.
                    echo "[] a carrier:Pipe ; carrier:friendId $friend_id ." > "$CMD_FIFO"
                    sleep 1

                    log "Streaming $DEVICE (${RESOLUTION} @ ${FPS}fps) via MJPEG"
                    ffmpeg -f v4l2 -input_format mjpeg \
                        -video_size "$RESOLUTION" -framerate "$FPS" \
                        -i "$DEVICE" \
                        -c:v copy -f mjpeg \
                        pipe:1 > "$CMD_FIFO" 2>/dev/null &
                    FFMPEG_PID=$!
                    log "Streaming started (ffmpeg PID=$FFMPEG_PID)"
                fi
                ;;

            *"carrier:FriendOnline"*)
                fid=$(extract "friendId" "$line")
                log "Friend #$fid is online"
                ;;

            *"carrier:Connected"*)
                log "Connected to Tox network"
                ;;
        esac
    done

    # Carrier exited — clean up and restart
    kill "$WRITER_PID" 2>/dev/null; WRITER_PID=""
    kill "$FFMPEG_PID" 2>/dev/null; FFMPEG_PID=""
    rm -f "$CMD_FIFO"

    log "Carrier exited, restarting in 3s..."
    sleep 3
done
