# Architecture

## Overview

Carrier is a cross-platform C library that wraps **libjami** (the Jami daemon library)
with an event-driven C API and an RDF Turtle wire format. It consists of four layers:

```
┌──────────────────────────────────────────────────────┐
│  carrier-cli (main_cli.c)                            │
│  Streaming CLI: stdin/stdout Turtle + FIFO mode      │
├──────────────────────────────────────────────────────┤
│  Turtle Protocol Layer                               │
│  turtle_parse.c — Turtle → carrier_*() dispatch      │
│  turtle_emit.c  — CarrierEvent → Turtle output       │
├──────────────────────────────────────────────────────┤
│  libcarrier — public C API (include/carrier.h)       │
│  carrier_jami.cc          C++ shim: API → libjami    │
│  carrier_jami_signals.cc  libjami signals → events   │
│  carrier_events.cc        event queue + clock fd     │
│  carrier_log.cc           log routing                │
│  rdf_canon.c              RDNA2015 canonical hashing │
├──────────────────────────────────────────────────────┤
│  libjami (C++) + contribs                            │
│  OpenDHT + dhtnet   distributed accounts, ICE, P2P   │
│  pjsip/pjnath       SIP signaling, NAT traversal     │
│  GnuTLS (nettle/hogweed/GMP)  TLS 1.3, X.509 identity│
│  OpenSSL/libtls, argon2, secp256k1   contrib crypto  │
│  FFmpeg, libgit2 (Swarm git DAG), libupnp/natpmp …   │
└──────────────────────────────────────────────────────┘
```

libjami is consumed as a pre-built static prefix (`make libjami`,
`~/.cache/resonator/libjami/<sha>/`) pinned by the `JAMI_VERSION` file; carrier links
`libjami-core.a` plus ~50 contrib archives explicitly (see `JAMI_CONTRIB_LIBS` in the
Makefile). The crypto-relevant inventory is documented in README.md § Cryptography.

## Directory Structure

```
carrier/
├── include/carrier.h           Public API (types, functions, events)
├── src/
│   ├── carrier_jami.cc         C++ shim: public API → libjami calls
│   ├── carrier_jami_signals.cc libjami signal handlers → CarrierEvent
│   ├── carrier_internal.hpp    Opaque Carrier struct definition
│   ├── carrier_events.cc/h     Event queue, clock fd, emission helpers
│   ├── carrier_log.cc/h        Log record routing (callback-based)
│   ├── account_defaults.hpp    Per-platform account defaults (e.g. UPnP off on iOS)
│   ├── vcard_utils.hpp         vCard FN parsing (contact display names)
│   ├── rdf_canon.c/h           RDNA2015-subset canonical RDF hashing
│   ├── sha256.h                SHA-256 (used by rdf_canon)
│   ├── turtle_parse.c/h        Serd-powered Turtle parser + command dispatch
│   ├── turtle_emit.c/h         Event serializer to Turtle
│   └── main_cli.c              CLI entry point (poll loop, FIFO mode)
├── tests/                      Unit tests (hand-rolled assert framework)
│                               + python e2e drivers (archive_round_trip.py,
│                               saved_conversation.py, two_peers.py)
├── tools/                      libjami prefix fetch/build scripts
├── third_party/serd/           Turtle parser library (git submodule)
├── JAMI_VERSION                Pinned jami-daemon SHA for the prefix
└── Makefile                    Build system (host + iOS + Android slices)
```

## Event Flow

### Inbound (commands)

```
stdin/FIFO → turtle_parse_and_execute()
                 │
                 ├── Serd parses Turtle into (type, predicates)
                 ├── dispatch_statement() maps type to carrier_*()
                 └── carrier_*() calls libjami (ConfigurationManager,
                     ConversationManager, PresenceManager, DataTransfer)
```

### Outbound (events)

```
libjami signal (e.g. SwarmMessageReceived) — fires on a daemon worker thread
     │
     └── carrier_jami_signals.cc handler → CarrierEvent struct
              │
              └── bounded queue + clock fd (eventfd on Linux, self-pipe on macOS)
                       │
                       └── carrier_iterate() — drains on the CALLER's thread
                                │
                                └── user callback (turtle_emit_event for the CLI)
                                         │
                                         └── fprintf → stdout/FIFO
```

The queue hop is load-bearing: libjami signals arrive on daemon threads, but the
event callback always runs on the thread that called `carrier_iterate()`. This
preserves the single-threaded-callback invariant that Antenna depends on.

## Key Design Decisions

1. **Library never writes to stdout or stderr** — Protocol events go through the
   event callback; operational logs go through a separate log callback
   (`carrier_set_log_callback`). The CLI wires events to stdout and logs to stderr;
   an embedded app (Antenna) routes both into its own systems.

2. **Opaque handle** — `Carrier*` is opaque; internals in `carrier_internal.hpp` are
   not part of the public API. The public header is pure C11; the implementation
   behind it is C++20 (libjami has no stable C ABI, so carrier embeds it via a C++
   shim).

3. **One `Carrier*` per process, many accounts** — libjami is process-scoped and
   cannot be re-initialized in-process. A single handle may hold multiple accounts;
   every account-scoped call takes an `account_id`. Account provisioning is
   asynchronous: callers wait for `CARRIER_EVENT_ACCOUNT_READY`.

4. **Clock-fd signaling, no polling** — `carrier_clock_fd()` exposes a read-end that
   becomes readable when the queue is non-empty, so poll()-based loops get
   zero-idle-CPU wakeups.

5. **RDF Turtle wire format** — Self-describing, grep-friendly, composable with Unix
   pipes. Types are nouns shared between commands and events.

6. **No hardware access** — Carrier handles accounts, trust, messaging, Swarm
   conversations, file transfer, presence, and device linking. Audio/video calls are
   out of scope at the current API surface (the v0.1 call vocabulary was retired in
   vocab v0.2.6).

## Transport & storage model (inherited from libjami)

- **Identity**: X.509 cert chain (CA → account → device) over a locally-generated
  RSA-4096 keypair; the 40-hex Jami ID is the SHA-1 fingerprint of the account
  public key.
- **Discovery**: OpenDHT (Kademlia) — typed, volatile, signed/encrypted values with
  per-type TTLs (minutes); used for rendezvous and presence, not durable storage.
- **Conversations**: "Swarms" — signed git repositories replicated peer-to-peer over
  TLS 1.3; every message is a commit; 1:1 chats are two-member Swarms.
- **Connections**: ICE (pjnath) + dhtnet, with TURN fallback; optional UPnP/NAT-PMP
  port mapping (compiled in via libupnp/libnatpmp; disabled by default on iOS builds,
  see `src/account_defaults.hpp`).

## Build Artifacts

- `build/libcarrier.a` — Static library (carrier + serd objects; consumers do the
  final libjami link)
- `build/carrier-cli` — Streaming CLI binary (host only)
- `build/carrier-tests` — Unit-test binary (via `make test`)
- `build-ios-*/libcarrier.a`, `build-android-arm64/libcarrier.a` — cross-compiled
  slices (via `make libcarrier-ios` / `make libcarrier-android`)
