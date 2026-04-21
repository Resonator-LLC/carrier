# Carrier

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-2.0.0-green.svg)](CHANGELOG.md)

Cross-platform C library and streaming CLI for the Tox protocol. Part of the Resonator project.

Wire protocol is compact RDF 1.1 Turtle.

See [ARCHITECTURE.md](ARCHITECTURE.md) for design details and [CONTRIBUTING.md](CONTRIBUTING.md) for contributor guidelines.

## Prerequisites

- C11 compiler (gcc or clang)
- CMake 3.16+ (for toxcore)
- pkg-config
- libsodium
- opus
- libvpx

### macOS

```bash
brew install libsodium opus libvpx cmake pkg-config
```

### Debian/Ubuntu

```bash
sudo apt install build-essential cmake pkg-config libsodium-dev libopus-dev libvpx-dev
```

## Building

All dependencies (toxcore, serd) are included as git submodules. One command builds everything:

```bash
git clone --recursive https://source.resonator.network/resonator/carrier.git
cd carrier
make
```

If you already cloned without `--recursive`:

```bash
git submodule update --init --recursive
make
```

The first build compiles toxcore from source (takes ~1 minute). Subsequent builds are incremental.

Produces:

- `build/libcarrier.a` — static C library (30 KB)
- `build/carrier-cli` — streaming CLI binary

### Clean rebuild

```bash
make clean && make
```

## Usage

### Quick test

```bash
echo '[] a carrier:GetId .' | ./build/carrier-cli --profile /tmp/test.tox
```

Output:

```turtle
@prefix carrier: <urn:carrier:> .
@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
[] a carrier:SelfId ; carrier:id "122523B948..." .
```

### CLI options

```
carrier-cli [OPTIONS]

Modes:
  (default)             Turtle protocol on stdin/stdout
  --pipe FRIEND_ID      Raw bidirectional data pipe to friend

Options:
  -p, --profile PATH    Tox profile file (default: carrier_profile.tox)
  -c, --config PATH     Config file
  -n, --nodes PATH      DHT nodes JSON file
  --fifo-in PATH        Read from named pipe instead of stdin
  --fifo-out PATH       Write to named pipe instead of stdout
  -h, --help            Show help
```

### Named pipe (FIFO) mode

```bash
mkfifo /tmp/carrier_in /tmp/carrier_out

./build/carrier-cli \
  --profile my.tox \
  --fifo-in=/tmp/carrier_in \
  --fifo-out=/tmp/carrier_out &

# Send commands
echo '[] a carrier:GetId .' > /tmp/carrier_in

# Read events
cat /tmp/carrier_out
```

### Pipe mode (raw data transfer)

Pipe mode turns carrier-cli into `netcat` over Tox — a raw bidirectional byte pipe between two machines over an encrypted connection. No Turtle parsing, just raw binary I/O.

#### Setup: create two profiles and add each other as friends

```bash
# Get each profile's Tox ID
ALICE_ID=$(echo '[] a carrier:GetId .' | ./build/carrier-cli -p alice.tox 2>/dev/null \
  | grep SelfId | head -1 | sed 's/.*carrier:id "\([^"]*\)".*/\1/')

BOB_ID=$(echo '[] a carrier:GetId .' | ./build/carrier-cli -p bob.tox 2>/dev/null \
  | grep SelfId | head -1 | sed 's/.*carrier:id "\([^"]*\)".*/\1/')

# Add each other (mutual add = instant friendship, no accept needed)
printf '[] a carrier:AddFriend ; carrier:id "%s" ; carrier:message "hi" .\n[] a carrier:Quit .\n' \
  "$BOB_ID" | ./build/carrier-cli -p alice.tox 2>/dev/null

printf '[] a carrier:AddFriend ; carrier:id "%s" ; carrier:message "hi" .\n[] a carrier:Quit .\n' \
  "$ALICE_ID" | ./build/carrier-cli -p bob.tox 2>/dev/null
```

#### Transfer a file

```bash
# Receiver (start first, waits for friend to come online)
./build/carrier-cli --pipe 0 -p bob.tox > received_file.mov

# Sender (on another terminal)
cat original_file.mov | ./build/carrier-cli --pipe 0 -p alice.tox

# With speed monitoring via pv
pv original_file.mov | ./build/carrier-cli --pipe 0 -p alice.tox
```

#### Pipe a tar archive

```bash
# Sender
tar czf - /my/directory | ./build/carrier-cli --pipe 0 -p alice.tox

# Receiver
./build/carrier-cli --pipe 0 -p bob.tox | tar xzf -
```

#### Enter pipe mode from Turtle mode

```turtle
[] a carrier:Pipe ; carrier:friendId 0 .
```

After this command, the CLI switches from Turtle to raw binary I/O.

#### Benchmark results (local loopback)

| Metric | Value |
|--------|-------|
| File | DJI_0002.MOV (422 MB drone footage) |
| Transfer time | ~45 seconds |
| Throughput | ~9.4 MB/s |
| Integrity | MD5 verified identical |
| Transport | Tox encrypted lossless packets |

Flow control is automatic — Tox's send queue backpressure throttles the stdin read rate. The pipe handles files of any size with zero data corruption.

### Unified RDF Turtle Protocol

Types are nouns — the same type flows from input, over the wire, to output. Carrier acts on predicates it understands and passes the rest through. Any statement may carry extra triples.

#### Input (stdin)

```turtle
[] a carrier:SelfId .
[] a carrier:Nick ; carrier:nick "MyName" .
[] a carrier:Status ; carrier:status "away" .
[] a carrier:StatusMessage ; carrier:message "Working" .
[] a carrier:FriendRequest ; carrier:id "TOXID_HEX..." ; carrier:message "Hello!" .
[] a carrier:FriendAccept ; carrier:requestId 0 .
[] a carrier:TextMessage ; carrier:friendId 0 ; carrier:text "Hello there" .
[] a carrier:GroupMessage ; carrier:groupId 1 ; carrier:text "Hi group" .
[] a carrier:FileTransfer ; carrier:friendId 0 ; carrier:path "/tmp/photo.jpg" .
[] a carrier:Group ; carrier:name "My Group" ; carrier:privacy "public" .
[] a carrier:Call ; carrier:friendId 0 ; carrier:audio true .
[] a carrier:CallAnswer ; carrier:friendId 0 .
[] a carrier:CallHangup ; carrier:friendId 0 .
[] a carrier:Save .
[] a carrier:Quit .
```

Extra triples are preserved end-to-end:

```turtle
[] a carrier:TextMessage ; carrier:friendId 0 ; carrier:text "Hi" ; carrier:mood "happy" .
```

#### Output (stdout)

```turtle
[] a carrier:Connected ; carrier:transport "UDP" ; carrier:at "2026-03-23T10:00:00"^^xsd:dateTime .
[] a carrier:SelfId ; carrier:id "122523B948..." .
[] a carrier:FriendOnline ; carrier:friendId 0 ; carrier:name "Alice" .
[] a carrier:TextMessage ; carrier:friendId 0 ; carrier:name "Alice" ; carrier:text "Hi!" ; carrier:mood "happy" ; carrier:at "2026-03-23T10:01:00"^^xsd:dateTime .
[] a carrier:Nick ; carrier:friendId 0 ; carrier:nick "Bob" .
[] a carrier:Status ; carrier:friendId 0 ; carrier:status 1 .
[] a carrier:FriendRequest ; carrier:requestId 0 ; carrier:key "AB12..." ; carrier:message "Add me!" .
[] a carrier:Call ; carrier:friendId 0 ; carrier:audio true ; carrier:video false .
[] a carrier:Error ; carrier:cmd "FriendRequest" ; carrier:message "Invalid Tox ID" .
[] a carrier:System ; carrier:message "Friend added as #0" .
```

## Using as a C library

```c
#include "carrier.h"

void on_event(const CarrierEvent *ev, void *ctx) {
    if (ev->type == CARRIER_EVENT_TEXT_MESSAGE) {
        printf("Message from %s: %s\n",
               ev->text_message.name, ev->text_message.text);
    }
}

int main(void) {
    /* Pass NULL log callback to disable logging (library never
     * writes to stdout or stderr on its own). */
    Carrier *c = carrier_new("profile.tox", NULL, NULL, NULL, NULL);
    carrier_set_event_callback(c, on_event, NULL);
    carrier_set_nick(c, "MyBot");

    while (1) {
        carrier_iterate(c);
        usleep(carrier_iteration_interval(c) * 1000);
    }

    carrier_free(c);
}
```

Compile:

```bash
cc -I carrier/include -I deps/include my_app.c \
  -L carrier/build -lcarrier \
  -L deps/lib -ltoxcore \
  $(pkg-config --libs libsodium opus vpx) \
  -lpthread -o my_app
```

## Running Tests

```bash
make test     # 20 unit tests, no toxcore dependency
make asan     # build with AddressSanitizer
```

## License

MIT — See [LICENSE](LICENSE)
