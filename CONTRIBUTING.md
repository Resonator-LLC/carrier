# Contributing to Carrier

Thank you for your interest in contributing to Carrier.

## Getting Started

### Prerequisites

- C11 compiler (gcc or clang)
- CMake 3.16+ (for toxcore)
- pkg-config
- libsodium, opus, libvpx

### Building

```bash
# Clone with submodules
git clone --recursive https://source.resonator.network/resonator/carrier.git
cd carrier

# Build toxcore (see README.md for details)
# Then:
make
make test
```

## Code Style

- **Standard:** C11 (`-std=c11`)
- **Warnings:** `-Wall -Wextra -Wpedantic` must compile clean
- **Indentation:** 4 spaces, no tabs
- **Line length:** 100 characters soft limit
- **Braces:** Opening brace on same line
- **Naming:** `snake_case` for functions and variables, `UPPER_CASE` for macros and enum values
- **Prefix:** All public API symbols start with `carrier_` or `CARRIER_`

## Submitting Changes

1. Fork the repository
2. Create a topic branch from `main`
3. Make your changes in small, focused commits
4. Ensure `make` and `make test` pass cleanly
5. Submit a pull request with a clear description of the change

### Commit Messages

Use the imperative mood in the subject line:

```
Add file transfer progress callback
Fix memory leak in turtle_parse on malformed input
Remove deprecated carrier_set_name alias
```

Keep the subject under 72 characters. Add a body separated by a blank line if the change needs explanation.

## Reporting Bugs

Open an issue with:

- Steps to reproduce
- Expected vs actual behavior
- Platform and compiler version
- Carrier version (`CARRIER_VERSION_STRING` from `carrier.h`)

## Security Vulnerabilities

See [SECURITY.md](SECURITY.md) for reporting security issues. Do **not** open a public issue for security vulnerabilities.

## License

By contributing, you agree that your contributions will be licensed under GPL-3.0, consistent with the project license.
