# Security Policy

## Reporting a Vulnerability

Carrier handles encrypted peer-to-peer communication. We take security seriously.

If you discover a security vulnerability, please report it privately:

- **Email:** security@resonator.network

**Do not** open a public issue for security vulnerabilities.

### What to include

- Description of the vulnerability
- Steps to reproduce
- Potential impact
- Suggested fix (if any)

### Response timeline

- **Acknowledgment:** within 48 hours
- **Initial assessment:** within 7 days
- **Fix or mitigation:** coordinated disclosure after patch is available

## Supported Versions

| Version | Supported |
|---------|-----------|
| 2.x     | Yes       |
| < 2.0   | No        |

## Scope

The following are in scope:

- Memory safety issues in libcarrier or the Turtle parser
- Authentication or encryption bypasses
- Denial of service via malformed Turtle input
- Information disclosure through the event protocol

Out of scope:

- Vulnerabilities in toxcore itself (report to [c-toxcore](https://github.com/TokTok/c-toxcore))
- Vulnerabilities in serd (report upstream)
