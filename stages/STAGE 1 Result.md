# Carrier — Stage 1 Result

## What is Carrier

Carrier is a cross-platform C library and streaming CLI for the Tox protocol, part of the Resonator project ecosystem.

## Problem Statement

Existing Tox clients are full GUI/TUI applications. They cannot be used as libraries, embedded in mobile apps, composed with Unix pipelines, or controlled programmatically. The goal is a Tox protocol library that works everywhere: Linux CLI, Android (NDK), iOS (Swift bridge), React Native, WASM — and can be controlled via streaming I/O for composability with the Unix tool ecosystem.

## Key Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| **ncurses** | Not used | Cross-platform target (Android/iOS/WASM) cannot link ncurses |
| **curl/HTTP** | Not in library | DHT nodes provided as static list/file. Name lookup is caller's responsibility. Zero HTTP in library. |
| **Audio/Video** | Signaling + raw PCM/frame callbacks | Library handles call invite/accept/reject/hangup + ToxAV iteration. No OpenAL, no hardware access. Caller provides their platform's audio/video stack. |
| **Wire protocol** | Compact RDF 1.1 Turtle | Semantic, self-describing, grep-friendly, graph-mergeable. Each statement is one line: `[] a carrier:Message ; carrier:from 0 ; carrier:text "hello" .` |
| **Dependencies** | toxcore + libsodium + opus + vpx + standard C lib | Minimal. No ncurses, no curl, no OpenAL, no libconfig (yet). |
| **Naming** | `carrier_*` / `Carrier*` | Consistent namespace |
| **Build deps** | From source, within the project | toxcore built from source at `c-toxcore/`, installed to `deps/`. No system package manager. |
| **License** | All rights reserved | |

## What Was Built (Stage 1)

### Artifacts

| Artifact | Size | Description |
|----------|------|-------------|
| `libcarrier.a` | 30 KB | Static C library. Pure C11, no UI deps. |
| `carrier-cli` | 1.4 MB | Streaming CLI binary (arm64 macOS). Reads Turtle from stdin, writes Turtle to stdout. |

### Code Statistics

| Component | Lines | Files |
|-----------|-------|-------|
| `carrier.c` (library core) | 1,194 | 1 |
| `carrier.h` (public API) | 310 | 1 |
| `carrier_internal.h` | 55 | 1 |
| `carrier_events.c/h` | 113 | 2 |
| `turtle_emit.c/h` | 243 | 2 |
| `turtle_parse.c/h` | 447 | 2 |
| `main_cli.c` | 192 | 1 |
| **Total** | **2,554** | **10** |

### Project Structure

```
resonator/
├── carrier/
│   ├── include/
│   │   └── carrier.h              # Public API header
│   ├── src/
│   │   ├── carrier.c              # Library: lifecycle, callbacks, commands
│   │   ├── carrier_internal.h     # Internal struct Carrier definition
│   │   ├── carrier_events.c       # Event emission helpers
│   │   ├── carrier_events.h
│   │   ├── turtle_emit.c          # CarrierEvent → Turtle serializer
│   │   ├── turtle_emit.h
│   │   ├── turtle_parse.c         # Turtle → carrier_*() command dispatcher
│   │   ├── turtle_parse.h
│   │   └── main_cli.c             # carrier-cli entry point
│   ├── build/
│   │   ├── libcarrier.a           # Static library
│   │   └── carrier-cli            # CLI binary
│   ├── stages/
│   │   └── STAGE 1 Result.md      # This file
│   └── Makefile
├── c-toxcore/                     # toxcore built from source
├── serd/                          # Serd RDF Turtle parser (built from source)
└── deps/                          # Local install prefix for toxcore
    ├── include/tox/
    │   ├── tox.h
    │   ├── toxav.h
    │   └── toxencryptsave.h
    └── lib/
        └── libtoxcore.a
```

## Architecture

### Library (`libcarrier.a`)

The library is a single opaque `Carrier*` handle with a callback-based event model:

```c
Carrier *c = carrier_new("profile.tox", NULL, NULL);
carrier_set_event_callback(c, my_handler, my_ctx);

while (running) {
    carrier_iterate(c);
    usleep(carrier_iteration_interval(c) * 1000);
}

carrier_free(c);
```

Events are delivered as `CarrierEvent` structs via a single callback. The library never does I/O to stdout, never touches hardware, never creates threads. The caller owns the event loop.

### Tox Callback → Event Flow

```
toxcore callback (e.g. on_friend_message)
    → cb_friend_message() in carrier.c
        → builds CarrierEvent { .type = CARRIER_EVENT_MESSAGE, ... }
            → carrier_emit(c, &ev)
                → c->event_cb(&ev, c->event_userdata)
                    → (in CLI: turtle_emit_event → fprintf to stdout)
```

### CLI (`carrier-cli`)

Thin wrapper (~192 lines) that:

1. Creates a Carrier instance
2. Sets `turtle_emit_event` as the event callback (writes Turtle to stdout)
3. Enters a `poll()` loop: reads Turtle commands from stdin, calls `carrier_iterate()` on each tick
4. Supports `--fifo-in` / `--fifo-out` for named pipe mode

### RDF Turtle Protocol

Prefix declarations (emitted once at stream start):

```turtle
@prefix carrier: <http://resonator.network/v2/carrier#> .
@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
```

Commands (stdin → carrier-cli):

```turtle
[] a carrier:GetId .
[] a carrier:SetNick ; carrier:nick "Resonator" .
[] a carrier:AddFriend ; carrier:id "76518406F6..." ; carrier:message "Hi!" .
[] a carrier:SendMsg ; carrier:to 0 ; carrier:text "Hello" .
[] a carrier:SendGroupMsg ; carrier:group 1 ; carrier:text "Hello group" .
[] a carrier:SetStatus ; carrier:status "away" .
[] a carrier:SendFile ; carrier:to 0 ; carrier:path "/tmp/photo.jpg" .
[] a carrier:Call ; carrier:friendId 0 ; carrier:audio true ; carrier:video false .
[] a carrier:Answer ; carrier:friendId 0 .
[] a carrier:Hangup ; carrier:friendId 0 .
[] a carrier:Quit .
```

Events (carrier-cli → stdout):

```turtle
[] a carrier:SelfId ; carrier:id "122523B948..." .
[] a carrier:Connected ; carrier:transport "UDP" ; carrier:at "2026-03-23T10:00:00"^^xsd:dateTime .
[] a carrier:Message ; carrier:from 0 ; carrier:name "Alice" ; carrier:text "Hi!" ; carrier:at "2026-03-23T10:01:00"^^xsd:dateTime .
[] a carrier:FriendOnline ; carrier:friendId 0 ; carrier:name "Alice" .
[] a carrier:FriendRequest ; carrier:requestId 0 ; carrier:key "AB12..." ; carrier:message "Add me!" .
[] a carrier:CallIncoming ; carrier:friendId 0 ; carrier:audio true ; carrier:video false .
[] a carrier:Error ; carrier:cmd "AddFriend" ; carrier:message "Invalid Tox ID" .
[] a carrier:System ; carrier:message "Friend added as #0" .
```

## Public API Surface (`carrier.h`)

### Lifecycle

- `carrier_new(profile, config, nodes)` → `Carrier*`
- `carrier_free(c)`
- `carrier_iterate(c)` — process Tox events (call periodically)
- `carrier_iteration_interval(c)` — suggested ms between iterate calls
- `carrier_set_event_callback(c, cb, userdata)`

### Identity & Status

- `carrier_get_id(c)` — emits `CARRIER_EVENT_SELF_ID`
- `carrier_set_nick(c, nick)`
- `carrier_set_status(c, status)`
- `carrier_set_status_message(c, msg)`

### Friends

- `carrier_add_friend(c, tox_id_hex, message)`
- `carrier_accept_friend(c, request_id)`
- `carrier_decline_friend(c, request_id)`
- `carrier_delete_friend(c, friend_id)`
- `carrier_send_message(c, friend_id, text)`

### File Transfers

- `carrier_send_file(c, friend_id, path)`
- `carrier_accept_file(c, friend_id, file_id, save_path)`
- `carrier_cancel_file(c, friend_id, file_id)`

### Groups

- `carrier_create_group(c, name, is_public)`
- `carrier_join_group(c, friend_id)`
- `carrier_leave_group(c, group_id)`
- `carrier_send_group_message(c, group_id, text)`
- `carrier_invite_to_group(c, group_id, friend_id)`

### Audio/Video (Signaling + Raw PCM/Frames)

- `carrier_call(c, friend_id, audio, video)`
- `carrier_answer(c, friend_id, audio, video)`
- `carrier_hangup(c, friend_id)`
- `carrier_send_audio(c, friend_id, pcm, samples, channels, sample_rate)`
- `carrier_send_video(c, friend_id, y, u, v, width, height)`
- `carrier_set_audio_bitrate(c, friend_id, bitrate)`
- `carrier_set_video_bitrate(c, friend_id, bitrate)`

### Profile

- `carrier_save(c)`

### Event Types (22 total)

```
CARRIER_EVENT_CONNECTED          CARRIER_EVENT_DISCONNECTED
CARRIER_EVENT_SELF_ID            CARRIER_EVENT_MESSAGE
CARRIER_EVENT_MESSAGE_SENT       CARRIER_EVENT_FRIEND_REQUEST
CARRIER_EVENT_FRIEND_ONLINE      CARRIER_EVENT_FRIEND_OFFLINE
CARRIER_EVENT_FRIEND_NAME        CARRIER_EVENT_FRIEND_STATUS
CARRIER_EVENT_FRIEND_STATUS_MESSAGE
CARRIER_EVENT_GROUP_MESSAGE      CARRIER_EVENT_GROUP_PEER_JOIN
CARRIER_EVENT_GROUP_PEER_EXIT    CARRIER_EVENT_GROUP_INVITE
CARRIER_EVENT_GROUP_SELF_JOIN    CARRIER_EVENT_CONFERENCE_MESSAGE
CARRIER_EVENT_CONFERENCE_INVITE  CARRIER_EVENT_FILE_RECV
CARRIER_EVENT_FILE_PROGRESS      CARRIER_EVENT_FILE_COMPLETE
CARRIER_EVENT_CALL_INCOMING      CARRIER_EVENT_CALL_STATE
CARRIER_EVENT_AUDIO_FRAME        CARRIER_EVENT_VIDEO_FRAME
CARRIER_EVENT_ERROR              CARRIER_EVENT_SYSTEM
```

## Verification

```bash
# Build
cd carrier && make

# Test: get own Tox ID
echo '[] a carrier:GetId .' | ./build/carrier-cli --profile /tmp/test.tox
# Output:
# @prefix carrier: <http://resonator.network/v2/carrier#> .
# @prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
# [] a carrier:SelfId ; carrier:id "122523B948..." .

# Test: set nick and get ID
printf '[] a carrier:SetNick ; carrier:nick "Resonator" .\n[] a carrier:GetId .\n[] a carrier:Quit .\n' \
  | ./build/carrier-cli --profile /tmp/test.tox

# Test: FIFO mode
mkfifo /tmp/carrier_in /tmp/carrier_out
./build/carrier-cli --fifo-in=/tmp/carrier_in --fifo-out=/tmp/carrier_out --profile /tmp/test.tox &
echo '[] a carrier:GetId .' > /tmp/carrier_in
cat /tmp/carrier_out
```

## What Remains (Future Stages)

### Stage 2 — Completeness

- File transfer data path (receive chunks, write to disk)
- Group join from invite data
- Conference support (conferences vs groups)
- Encrypted profile support (password prompt/eval)
- Config file loading via libconfig
- Custom DHT nodes file parsing (JSON)

### Stage 3 — Robustness

- Message retry queue
- Chat logging to disk
- Blocked words filter
- Friend/group config persistence
- Error recovery and reconnection

### Stage 4 — Cross-Platform

- Android NDK build + JNI wrapper
- iOS static lib + Swift bridge
- WASM/Emscripten build (single-threaded `carrier_iterate()` from JS)
- React Native native module

### Stage 5 — C Library Polish

- Shared library build (`libcarrier.so` / `.dylib`)
- pkg-config file
- CMake build system (alongside Make)
- API documentation
- Thread safety guarantees / documentation
- Versioned API with soname

## Why RDF Turtle

The wire protocol uses compact RDF 1.1 Turtle instead of JSON because:

- **Semantic structure**: Each message is typed (`a carrier:Message`) with named predicates. Any RDF tool can reason about the data, merge it with other graphs, query it with SPARQL.
- **Unix pipeline composability**: Each statement is one line ending in `.`, so `grep`, `awk`, `sed` work. `grep "carrier:Message"` filters messages. But you can also pipe into a triplestore.
- **Self-describing**: `@prefix` declarations and `a carrier:TypeName` mean the stream is parseable without out-of-band schema knowledge.
- **Graph merging**: Multiple carrier-cli instances' outputs can be concatenated into a single valid RDF graph. With JSON you'd need a wrapper.
- **Language-agnostic consumption**: Python's rdflib, Rust's sophia, Java's Jena, JS's N3.js can all parse it natively.
