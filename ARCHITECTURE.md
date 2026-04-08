# Architecture

## Overview

Carrier is a cross-platform C library that wraps the Tox protocol with an event-driven API and an RDF Turtle wire format. It consists of three layers:

```
┌─────────────────────────────────────────────────┐
│  carrier-cli (main_cli.c)                       │
│  Streaming CLI: stdin/stdout Turtle + pipe mode  │
├─────────────────────────────────────────────────┤
│  Turtle Protocol Layer                           │
│  turtle_parse.c — Turtle → carrier_*() dispatch  │
│  turtle_emit.c  — CarrierEvent → Turtle output   │
├─────────────────────────────────────────────────┤
│  libcarrier (carrier.c + carrier_events.c)       │
│  Event-driven C API wrapping toxcore             │
├─────────────────────────────────────────────────┤
│  toxcore + libsodium                             │
│  P2P encrypted transport                         │
└─────────────────────────────────────────────────┘
```

## Directory Structure

```
carrier/
├── include/carrier.h       Public API (types, functions, events)
├── src/
│   ├── carrier.c           Core library: Tox callbacks → CarrierEvent
│   ├── carrier_internal.h  Opaque Carrier struct definition
│   ├── carrier_events.c/h  Event emission helpers
│   ├── turtle_parse.c/h    Serd-powered Turtle parser + command dispatch
│   ├── turtle_emit.c/h     Event serializer to Turtle
│   └── main_cli.c          CLI entry point (poll loop, FIFO, pipe mode)
├── tests/                  Unit tests (hand-rolled assert framework)
├── examples/               Bash scripts demonstrating CLI usage
├── third_party/serd/       Turtle parser library (git submodule)
└── Makefile                Build system
```

## Event Flow

### Inbound (commands)

```
stdin/FIFO → turtle_parse_and_execute()
                 │
                 ├── Serd parses Turtle into (type, predicates)
                 ├── dispatch_statement() maps type to carrier_*()
                 └── carrier_*() calls toxcore API
```

### Outbound (events)

```
toxcore callback (e.g. friend_message_cb)
     │
     └── carrier_emit() → CarrierEvent struct
              │
              └── user callback (turtle_emit_event for CLI)
                       │
                       └── fprintf → stdout/FIFO
```

### Turtle Passthrough

When a received message is itself valid Turtle (`[] a carrier:...`), the emit layer passes it through, stripping the trailing `.` and appending receiver metadata (friendId, name, timestamp). This enables end-to-end semantic messages.

## Key Design Decisions

1. **Library never writes to stdout** — All I/O is controlled by the caller via the event callback. The CLI wires `turtle_emit_event` to stdout; an embedded app would use a different callback.

2. **Opaque handle** — `Carrier*` is opaque; internals in `carrier_internal.h` are not part of the public API.

3. **Single-threaded event loop** — No internal threads. The caller drives `carrier_iterate()` at the suggested interval.

4. **RDF Turtle wire format** — Self-describing, grep-friendly, composable with Unix pipes. Extra triples are preserved end-to-end.

5. **No hardware access** — Library handles signaling only. Audio/video frames are delivered as raw PCM/YUV for the caller to render.

## Build Artifacts

- `build/libcarrier.a` — Static library (carrier + serd objects)
- `build/carrier-cli` — Streaming CLI binary
- `build/test_carrier` — Test binary (via `make test`)
