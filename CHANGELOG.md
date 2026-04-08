# Changelog

All notable changes to Carrier will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [2.0.0] - 2026-03-23

Complete rewrite as an independent project within the Resonator ecosystem.

### Added
- Unified RDF 1.1 Turtle wire protocol for all input/output
- Streaming CLI (`carrier-cli`) with stdin/stdout Turtle protocol
- Named pipe (FIFO) mode for IPC (`--fifo-in`, `--fifo-out`)
- Raw bidirectional pipe mode (`--pipe`) for binary data transfer
- DHT info reporting and dynamic bootstrap node support
- Audio/video call signaling (call, answer, hangup, bitrate control)
- File transfer with automatic flow control (~9.4 MB/s throughput)
- Group chat support (create, join, leave, invite, message)
- Profile persistence with optional encryption
- Extra triple passthrough — arbitrary RDF triples preserved end-to-end
- Shell bot and webcam server examples
- Cross-platform support (macOS, Linux)

### Changed
- Wire protocol changed from ad-hoc text to RDF 1.1 Turtle
- Event types are nouns, not verbs (e.g., `carrier:TextMessage` not `carrier:SendMessage`)
- Library emits events via callback, never writes to stdout directly

### Removed
- All references to upstream project
- Legacy command format
