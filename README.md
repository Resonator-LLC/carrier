# Carrier

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/version-3.2.0-green.svg)](include/carrier.h)

Cross-platform C library and streaming CLI for peer-to-peer messaging over
[Jami](https://jami.net) (libjami). Part of the Resonator project.

Wire protocol is compact RDF 1.1 Turtle.

> **History.** Carrier v2 wrapped the Tox protocol (toxcore/libsodium). v3 migrated the
> backend to **libjami**, the GPL-licensed daemon library behind the Jami messenger —
> distributed accounts on OpenDHT, conversations as signed git "Swarm" repositories,
> TLS 1.3 transport. See `arch/jami-migration.md` in the architecture vault for the full
> migration record.

See [ARCHITECTURE.md](ARCHITECTURE.md) for design details and [CONTRIBUTING.md](CONTRIBUTING.md) for contributor guidelines.

## Prerequisites

- C11 + C++20 compiler (clang or gcc — the libjami shim is C++)
- GNU make
- `curl` + `tar` (to fetch the pre-built libjami prefix)
- Python 3 (end-to-end tests only)

All third-party C/C++ dependencies — libjami itself and its ~50 contrib archives —
come from a pre-built, hermetic **libjami prefix**; nothing is taken from Homebrew or
the system package manager. Only OS frameworks and the C runtime are linked from
outside the prefix.

## Building

The Turtle parser (serd) is a git submodule; libjami is a cached binary prefix.

```bash
git clone --recursive https://source.resonator.network/resonator/carrier.git
cd carrier

make libjami    # populate the libjami prefix: fetches the pre-built tarball
                # for the SHA pinned in JAMI_VERSION (~30s); falls back to a
                # hermetic source build (1–3 hours cold) if none is published
make            # build libcarrier.a + carrier-cli
```

If you already cloned without `--recursive`:

```bash
git submodule update --init --recursive
```

The prefix lands under `${XDG_CACHE_HOME:-~/.cache}/resonator/libjami/<sha>/` and is
shared across checkouts. `make libjami-fetch` / `make libjami-build` select fetch-only
or build-only explicitly. Cross-compile slices for mobile:

```bash
make libjami PLATFORM=ios-device && make libcarrier-ios PLATFORM=ios-device
make libjami PLATFORM=ios-simulator && make libcarrier-ios PLATFORM=ios-simulator
make libjami PLATFORM=android-arm64 && make libcarrier-android
```

Produces:

- `build/libcarrier.a` — static C library (carrier + serd objects; the libjami link
  happens in the final consumer)
- `build/carrier-cli` — streaming CLI binary (host platforms only)

### Clean rebuild

```bash
make clean && make
```

## Usage

### Quick test

Accounts are provisioned asynchronously — wait for `carrier:AccountReady` on stdout
before sending account-scoped commands:

```bash
./build/carrier-cli --create-account Alice --data-dir /tmp/carrier-demo
```

```turtle
@prefix carrier: <http://resonator.network/v2/carrier#> .
@prefix xsd: <http://www.w3.org/2001/XMLSchema#> .
[] a carrier:AccountReady ; carrier:account "f7a3…" ; carrier:selfUri "1b2c…40-hex…" ; carrier:displayName "Alice" .
```

Then drive it over stdin (Turtle in, Turtle out):

```turtle
[] a carrier:GetId .
[] a carrier:SendTrustRequest ; carrier:contactUri "jami:<40-hex>" ; carrier:message "Hello!" .
[] a carrier:Quit .
```

### CLI options

```
carrier-cli [OPTIONS]

Options:
  -d, --data-dir PATH        Jami data dir (default: platform default)
  -a, --account ID           Load an existing account by libjami ID
      --create-account N     Create a fresh account with display name N
      --import-account PATH  Import account from a .gz archive blob
      --archive-password PIN Password for the archive (encrypted exports/imports)
      --export-account PATH  After loading the account, export its archive to PATH and exit
      --remove-account ID    Remove the named account from the data dir and exit
      --link-account         Create a new account in linking mode and
                             print the import URI (DeviceLinkPin event)
      --fifo-in PATH         Read from named pipe instead of stdin
      --fifo-out PATH        Write to named pipe instead of stdout
      --log LEVEL            error|warn|info|debug (default: error;
                             falls back to CARRIER_LOG env var)
  -h, --help                 Show this help
```

Exactly one of `--account`, `--create-account`, `--import-account`, `--link-account`,
or `--remove-account` is required. `--export-account` stacks on top of any of the
first three to dump the archive after the account is ready.

### Named pipe (FIFO) mode

```bash
mkfifo /tmp/carrier_in /tmp/carrier_out

./build/carrier-cli \
  --account <id> --data-dir /tmp/carrier-demo \
  --fifo-in=/tmp/carrier_in \
  --fifo-out=/tmp/carrier_out &

# Send commands
echo '[] a carrier:GetId .' > /tmp/carrier_in

# Read events
cat /tmp/carrier_out
```

### Unified RDF Turtle Protocol

Types are nouns under the `carrier:` namespace
(`http://resonator.network/v2/carrier#`). Commands go in on stdin, events come out on
stdout; the same vocabulary covers both directions. Commands may carry
`carrier:account "<id>"` to select an account; with a single loaded account it is
implied.

#### Commands (stdin)

```turtle
# Accounts
[] a carrier:CreateAccount ; carrier:displayName "Alice" .
[] a carrier:ImportAccount ; carrier:archivePath "/tmp/archive.gz" .
[] a carrier:LoadAccount ; carrier:account "ID" .
[] a carrier:ExportAccount ; carrier:path "/tmp/archive.gz" .
[] a carrier:RemoveAccount ; carrier:account "ID" .

# Identity, trust, presence
[] a carrier:GetId .
[] a carrier:SetNick ; carrier:nick "Alice" .
[] a carrier:SendTrustRequest ; carrier:contactUri "jami:<40-hex>" ; carrier:message "Hi" .
[] a carrier:AcceptTrustRequest ; carrier:contactUri "jami:<40-hex>" .
[] a carrier:DiscardTrustRequest ; carrier:contactUri "jami:<40-hex>" .
[] a carrier:RemoveContact ; carrier:contactUri "jami:<40-hex>" .
[] a carrier:BlockContact ; carrier:contactUri "jami:<40-hex>" .
[] a carrier:UnblockContact ; carrier:contactUri "jami:<40-hex>" .
[] a carrier:SubscribePresence ; carrier:contactUri "jami:<40-hex>" .

# Messaging (1:1 and Swarm conversations)
[] a carrier:SendMsg ; carrier:contactUri "jami:<40-hex>" ; carrier:text "Hello" .
[] a carrier:CreateConversation ; carrier:privacy "invites_only" .
[] a carrier:GetSavedConversation .
[] a carrier:SendConversationMsg ; carrier:conversationId "<id>" ; carrier:text "Hi all" .
[] a carrier:AcceptConversationRequest ; carrier:conversationId "<id>" .
[] a carrier:DeclineConversationRequest ; carrier:conversationId "<id>" .
[] a carrier:InviteContact ; carrier:conversationId "<id>" ; carrier:contactUri "jami:<40-hex>" .
[] a carrier:RemoveConversation ; carrier:conversationId "<id>" .
[] a carrier:SendReaction ; carrier:conversationId "<id>" ; carrier:messageId "<commit>" ; carrier:reaction "👍" .

# Files
[] a carrier:SendFile ; carrier:conversationId "<id>" ; carrier:path "/tmp/photo.jpg" .
[] a carrier:AcceptFile ; carrier:conversationId "<id>" ; carrier:messageId "<commit>" ; carrier:fileId "<fid>" ; carrier:path "/tmp/dest.jpg" .
[] a carrier:CancelFile ; carrier:conversationId "<id>" ; carrier:fileId "<fid>" .
[] a carrier:SendRdfObject ; carrier:conversationId "<id>" ; carrier:path "/tmp/object.ttl" .

# Device linking
[] a carrier:LinkDevice .
[] a carrier:AuthorizeDevice ; carrier:pin "jami-auth://…" .
[] a carrier:RevokeDevice ; carrier:contactUri "<device-fingerprint>" .

[] a carrier:Quit .
```

#### Events (stdout)

```turtle
[] a carrier:Connected ; carrier:account "ID" .
[] a carrier:AccountReady ; carrier:account "ID" ; carrier:selfUri "<40-hex>" ; carrier:displayName "Alice" .
[] a carrier:TrustRequest ; carrier:account "ID" ; carrier:contactUri "<40-hex>" ; carrier:payload "…" .
[] a carrier:ContactOnline ; carrier:account "ID" ; carrier:contactUri "<40-hex>" .
[] a carrier:TextMessage ; carrier:account "ID" ; carrier:contactUri "<40-hex>" ; carrier:conversationId "<id>" ; carrier:messageId "<commit>" ; carrier:text "Hi!" .
[] a carrier:GroupMessage ; carrier:account "ID" ; carrier:conversationId "<id>" ; carrier:messageId "<commit>" ; carrier:contactUri "<40-hex>" ; carrier:text "Hi all" .
[] a carrier:FileRecv ; carrier:account "ID" ; carrier:conversationId "<id>" ; carrier:contactUri "<40-hex>" ; carrier:messageId "<commit>" ; carrier:fileId "<fid>" ; carrier:filename "photo.jpg" ; carrier:size 102400 .
[] a carrier:DeviceLinkPin ; carrier:pin "jami-auth://…" .
[] a carrier:Error ; carrier:command "SendMsg" ; carrier:class "NotTrusted" ; carrier:message "…" .
```

The full event set also covers delivery status (`MessageSent`), Swarm membership
(`GroupPeerJoin`/`GroupPeerExit`, `ConversationRequest`/`ConversationReady`),
reactions, continuous presence, file progress/completion, device link/unlink, and
account errors — see the `CarrierEventType` enum in
[include/carrier.h](include/carrier.h) for the authoritative list.

## Using as a C library

```c
#include "carrier.h"

void on_event(const CarrierEvent *ev, void *ctx) {
    if (ev->type == CARRIER_EVENT_TEXT_MESSAGE) {
        printf("Message from %s: %.*s\n",
               ev->text_message.contact_uri,
               (int)ev->text_message.text_len, ev->text_message.text);
    }
}

int main(void) {
    /* NULL log callback disables logging (the library never writes to
     * stdout or stderr on its own). */
    Carrier *c = carrier_new("/tmp/carrier-data", NULL, NULL);
    carrier_set_event_callback(c, on_event, NULL);

    char account_id[CARRIER_ACCOUNT_ID_LEN];
    carrier_create_account(c, "MyBot", NULL, NULL, account_id);

    /* Drive the event loop: poll carrier_clock_fd(c) and call
     * carrier_iterate(c) whenever it becomes readable. Wait for
     * CARRIER_EVENT_ACCOUNT_READY before sending. */
    for (;;) {
        carrier_iterate(c);
        usleep(carrier_iteration_interval(c) * 1000);
    }

    carrier_free(c);
}
```

Linking pulls in libjami and its full contrib set — there is no pkg-config target for
the prefix yet, so reuse the CLI's link recipe: `JAMI_LIB` + `JAMI_CONTRIB_LIBS` +
the platform frameworks block in the [Makefile](Makefile). Rust consumers (Antenna)
do the equivalent in `build.rs`.

## Cryptography

Carrier implements no cryptographic primitives of its own; all cryptography is
provided by libjami and its contrib libraries, linked statically from the prefix
(see `JAMI_CONTRIB_LIBS` in the [Makefile](Makefile)):

| Component | Archives | Role |
|-----------|----------|------|
| **GnuTLS** (+ nettle/hogweed/GMP backend) | `libgnutls.a`, `libnettle.a`, `libhogweed.a`, `libgmp.a` | TLS 1.3 sessions for P2P links and Swarm sync; X.509 identity certificates (account/device certs); archive encryption |
| **OpenSSL** | `libssl.a`, `libcrypto.a` | crypto/TLS backend for contribs that require it (HTTPS in FFmpeg/libgit2 paths) |
| **LibreTLS** | `libtls.a` | libtls API shim over OpenSSL for contribs using the libtls interface |
| **Argon2** | `libargon2.a` | KDF for password-protected account archives |
| **secp256k1** | `libsecp256k1.a` | signatures for the optional `ns.jami.net` name registry |
| **libsrtp** | `libsrtp-<triple>.a` | SRTP media encryption (calls; unused at the current API surface) |
| **OpenDHT / dhtnet** | `libopendht.a`, `libdhtnet.a` | encrypted+signed DHT values, ICE/TLS connection management |

Identity model: each account is an X.509 certificate chain (CA → account → device)
over a locally-generated RSA-4096 keypair; the 40-hex Jami ID is the fingerprint of
the account public key. Conversations are signed git repositories ("Swarms")
replicated peer-to-peer over TLS 1.3. There is no Resonator-operated server and no
plaintext relay.

This inventory is the source of truth for the export-compliance filings (US EAR
self-classification, French ANSSI declaration) — keep it in sync with the Makefile
link line.

## Running Tests

```bash
make test          # 27 unit tests (rdf_canon, vcard_utils, contact_restored,
                   # account_defaults) — no libjami link needed
make test-archive  # e2e: create/export/import/remove account flows (python3,
                   # spawns build/carrier-cli)
make test-saved    # e2e: GetSavedConversation round-trip against libjami
make asan          # build with AddressSanitizer
make tsan          # build with ThreadSanitizer
```

## License

MIT — See [LICENSE](LICENSE)

Carrier links libjami, which is licensed GPL-3.0-or-later; binaries that include
libjami are distributed under the GPL's terms.
