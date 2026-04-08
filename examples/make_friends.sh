#!/bin/bash
#
# make_friends.sh — Two carrier instances befriend each other and chat.
#
# Instance A (Alice): waits for friend request, accepts it, says hello.
# Instance B (Bob): sends friend request to Alice, receives hello, replies.
#
# Usage:
#   ./examples/make_friends.sh
#

set -e

DIR="$(cd "$(dirname "$0")/.." && pwd)"
CLI="$DIR/build/carrier-cli"
TMP="${DIR}/../tmp"
mkdir -p "$TMP"

# Clean old profiles
rm -f "$TMP/alice.tox" "$TMP/bob.tox"

echo "=== Creating profiles and getting IDs ==="

ALICE_ID=$(echo '[] a carrier:SelfId .' | $CLI -p "$TMP/alice.tox" 2>/dev/null \
    | grep SelfId | head -1 | sed 's/.*carrier:id "\([^"]*\)".*/\1/')
echo "  Alice: $ALICE_ID"

BOB_ID=$(echo '[] a carrier:SelfId .' | $CLI -p "$TMP/bob.tox" 2>/dev/null \
    | grep SelfId | head -1 | sed 's/.*carrier:id "\([^"]*\)".*/\1/')
echo "  Bob:   $BOB_ID"

echo "=== Bob adds Alice as friend ==="
printf '[] a carrier:FriendRequest ; carrier:id "%s" ; carrier:message "Hey Alice!" .\n[] a carrier:Quit .\n' \
    "$ALICE_ID" | $CLI -p "$TMP/bob.tox" 2>/dev/null | while read -r line; do
    echo "  [Bob] $line"
done

echo "=== Starting Alice (background, listening for friend request) ==="

# Alice: accept friend request, set nick, wait for friend, say hello
{
    cat <<'CMDS'
[] a carrier:Nick ; carrier:nick "Alice" .
CMDS
    # Keep stdin open — Alice needs to stay running to receive events
    # We'll wait and read her output instead
    sleep 120
} | $CLI -p "$TMP/alice.tox" 2>/dev/null > "$TMP/alice_out.txt" &
PID_ALICE=$!

echo "=== Starting Bob (background) ==="
{
    cat <<'CMDS'
[] a carrier:Nick ; carrier:nick "Bob" .
CMDS
    sleep 120
} | $CLI -p "$TMP/bob.tox" 2>/dev/null > "$TMP/bob_out.txt" &
PID_BOB=$!

cleanup() {
    kill $PID_ALICE $PID_BOB 2>/dev/null
}
trap cleanup EXIT

echo "=== Waiting for connection and friend request ==="

# Poll Alice's output for friend request
for i in $(seq 1 60); do
    sleep 1
    if grep -q "FriendRequest" "$TMP/alice_out.txt" 2>/dev/null; then
        echo "  Alice received friend request!"
        grep "FriendRequest" "$TMP/alice_out.txt"
        break
    fi
    if [ $i -eq 60 ]; then
        echo "  !! Timeout waiting for friend request"
        echo "  Alice output:"
        cat "$TMP/alice_out.txt"
        echo "  Bob output:"
        cat "$TMP/bob_out.txt"
        exit 1
    fi
    [ $((i % 10)) -eq 0 ] && echo "  ... waiting ($i s)"
done

echo "=== Waiting for both to come online ==="
for i in $(seq 1 60); do
    sleep 1
    A_ONLINE=$(grep -c "FriendOnline\|Connected" "$TMP/alice_out.txt" 2>/dev/null || true)
    B_ONLINE=$(grep -c "FriendOnline\|Connected" "$TMP/bob_out.txt" 2>/dev/null || true)

    if [ "$A_ONLINE" -gt 0 ] && [ "$B_ONLINE" -gt 0 ]; then
        echo "  Both connected!"
        break
    fi
    [ $((i % 10)) -eq 0 ] && echo "  ... waiting ($i s) alice=$A_ONLINE bob=$B_ONLINE"
done

echo ""
echo "=== Alice output ==="
cat "$TMP/alice_out.txt"

echo ""
echo "=== Bob output ==="
cat "$TMP/bob_out.txt"

echo ""
echo "=== Done ==="
