#!/bin/bash
#
# shell_bot.sh — Execute shell commands received as TextMessages.
#
# Accepts friend requests containing "mutabor". When a friend sends
# a TextMessage, runs the text as a shell command and replies with
# the output.
#
# Usage:
#   ./shell_bot.sh [profile.tox]
#

PROFILE="${1:-shell_bot.tox}"
CLI="${CLI:-$(dirname "$0")/../build/carrier-cli}"
PIDFILE="/tmp/shell_bot.pid"

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    log "Already running (PID $(cat "$PIDFILE"))."
    exit 1
fi
echo $$ > "$PIDFILE"

cleanup() {
    exec 3>&- 2>/dev/null
    [ -n "${CARRIER_PID:-}" ] && kill "$CARRIER_PID" 2>/dev/null
    rm -f "$CMD_FIFO" "$OUT_FIFO" "$PIDFILE"
}
trap cleanup EXIT INT TERM

extract() {
    local pred="$1" line="$2"
    echo "$line" | sed -n "s/.*carrier:$pred \([^ ;.]*\).*/\1/p"
}

extract_str() {
    local pred="$1" line="$2"
    echo "$line" | sed -n "s/.*carrier:$pred \"\([^\"]*\)\".*/\1/p"
}

turtle_escape() {
    printf '%s' "$1" | perl -0777 -pe 's/\\/\\\\/g; s/"/\\"/g; s/\n/\\n/g; s/\r/\\r/g'
}

send() {
    printf '%s\n' "$1" >&3
}

while true; do
    CMD_FIFO=$(mktemp -u /tmp/shell_bot_cmd.XXXXXX)
    OUT_FIFO=$(mktemp -u /tmp/shell_bot_out.XXXXXX)
    mkfifo "$CMD_FIFO" "$OUT_FIFO"

    log "Starting carrier (profile: $PROFILE)"

    # Start carrier: reads from CMD_FIFO, writes to OUT_FIFO
    "$CLI" -p "$PROFILE" --fifo-in="$CMD_FIFO" --fifo-out="$OUT_FIFO" 2>/dev/null &
    CARRIER_PID=$!

    # Open persistent write fd to command FIFO
    exec 3>"$CMD_FIFO"

    send '[] a carrier:Nick ; carrier:nick "ShellBot" .'
    send '[] a carrier:StatusMessage ; carrier:message "Send me a command" .'

    # Read events from output FIFO
    while IFS= read -r line; do
        log "$line"

        case "$line" in
            *"carrier:FriendRequest"*)
                req_id=$(extract "requestId" "$line")
                msg=$(extract_str "message" "$line")
                if [ -n "$req_id" ]; then
                    case "$msg" in
                        *mutabor*)
                            log "Accepting friend request #$req_id (passphrase ok)"
                            send "[] a carrier:FriendAccept ; carrier:requestId $req_id ."
                            ;;
                        *)
                            log "Declining friend request #$req_id (bad passphrase: '$msg')"
                            send "[] a carrier:FriendDecline ; carrier:requestId $req_id ."
                            ;;
                    esac
                fi
                ;;

            *"a carrier:TextMessage"*)
                friend_id=$(extract "friendId" "$line")
                cmd=$(extract_str "text" "$line")
                if [ -n "$friend_id" ] && [ -n "$cmd" ]; then
                    log "Friend #$friend_id: $cmd"
                    output=$(exec 3>&-; bash -c "$cmd" 2>&1 | head -c 1200)
                    escaped=$(exec 3>&-; turtle_escape "$output")
                    send "[] a carrier:TextMessage ; carrier:friendId $friend_id ; carrier:text \"$escaped\" ."
                    log "Replied with ${#output} bytes"
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
    done < "$OUT_FIFO"

    exec 3>&-
    kill "$CARRIER_PID" 2>/dev/null; CARRIER_PID=""
    rm -f "$CMD_FIFO" "$OUT_FIFO"

    log "Carrier exited, restarting in 3s..."
    sleep 3
done
