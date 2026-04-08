#!/bin/bash
#
# hello_friend.sh — Example script that controls carrier-cli via named pipes.
#
# Creates a carrier instance, sets nick and status, waits for the first
# friend to come online, says hello, and exits.
#
# Usage:
#   ./examples/hello_friend.sh [profile.tox]
#

set -e

PROFILE="${1:-carrier_profile.tox}"
PIPE_IN=$(mktemp -u /tmp/carrier_in.XXXXXX)
PIPE_OUT=$(mktemp -u /tmp/carrier_out.XXXXXX)
CLI="./build/carrier-cli"

cleanup() {
    [ -n "$CLI_PID" ] && kill "$CLI_PID" 2>/dev/null
    rm -f "$PIPE_IN" "$PIPE_OUT"
}
trap cleanup EXIT

# Create named pipes
mkfifo "$PIPE_IN" "$PIPE_OUT"

# Start carrier-cli in background, talking through pipes
$CLI --profile "$PROFILE" --fifo-in="$PIPE_IN" --fifo-out="$PIPE_OUT" &
CLI_PID=$!
sleep 1

# Helper: send a Turtle command
send() {
    echo "$1" > "$PIPE_IN"
}

# Helper: read events until we see a pattern (with timeout)
wait_for() {
    local pattern="$1"
    local timeout="${2:-60}"
    local line

    while IFS= read -r -t "$timeout" line <> "$PIPE_OUT"; do
        echo "  <- $line"
        if echo "$line" | grep -q "$pattern"; then
            return 0
        fi
    done

    echo "  !! timeout waiting for: $pattern"
    return 1
}

# Read and print initial output (prefixes + self ID)
echo "=== Starting carrier ==="
for i in 1 2 3; do
    IFS= read -r -t 5 line <> "$PIPE_OUT" && echo "  <- $line"
done

# Set nick
echo "=== Setting nick ==="
send '[] a carrier:Nick ; carrier:nick "HelloBot" .'

# Set status message
echo "=== Setting status message ==="
send '[] a carrier:StatusMessage ; carrier:message "Waiting for friends..." .'

# Set status to available
echo "=== Setting status ==="
send '[] a carrier:Status ; carrier:status "online" .'

# Wait for a friend to come online
echo "=== Waiting for a friend to come online ==="
if wait_for "FriendOnline" 120; then
    # Extract friend ID from the event line
    FRIEND_ID=$(echo "$line" | sed -n 's/.*carrier:friendId \([0-9]*\).*/\1/p')
    FRIEND_NAME=$(echo "$line" | sed -n 's/.*carrier:name "\([^"]*\)".*/\1/p')

    echo "=== Friend online: #${FRIEND_ID} (${FRIEND_NAME}) ==="

    # Say hello
    send "[] a carrier:TextMessage ; carrier:friendId ${FRIEND_ID} ; carrier:text \"Hello ${FRIEND_NAME}! I'm a carrier bot.\" ."
    echo "=== Sent hello to ${FRIEND_NAME} ==="

    # Wait briefly for delivery confirmation
    sleep 2
    while IFS= read -r -t 2 line <> "$PIPE_OUT"; do
        echo "  <- $line"
    done
else
    echo "=== No friends came online ==="
fi

# Save and quit
echo "=== Saving and quitting ==="
send '[] a carrier:Save .'
sleep 1
send '[] a carrier:Quit .'
sleep 1

echo "=== Done ==="
