/*  carrier.h
 *
 *  Carrier — Cross-platform C library for P2P transport.
 *  Part of the Resonator project.
 *
 *  Copyright (c) 2026-2027 Resonator LLC
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 *
 *  ---------------------------------------------------------------------------
 *  Backend: libjami (Jami Euclid 16.x). The shim lives in src/carrier_jami.cc
 *  and src/carrier_jami_signals.cc. See arch/jami-migration.md for the full
 *  migration plan; decisions D1–D20 are the load-bearing context.
 *
 *  Scope as of M5α (this header): lifecycle, account creation/loading,
 *  self-ID, trust requests, 1:1 text messaging, multi-party Swarm
 *  conversations (create, send, accept/decline invitation, invite member,
 *  remove), reactions, continuous presence, Swarm file transfers
 *  (offer/accept/cancel via DataTransferSignal::DataTransferEvent and
 *  inbound `application/data-transfer+json` commits), and device linking
 *  (new-device account-creation in linking mode, source-device authorize,
 *  revoke). Calls (audio/video) are out of scope at v0.2.x — the
 *  forward-looking call vocabulary inherited from v0.1 was retired in
 *  v0.2.6 and will be reintroduced in v0.3 if a consumer demands it.
 *  Functions and event types are added as they land — there are no
 *  forward-declared placeholders for unimplemented surface.
 *
 *  v3.2 break (M4a): `message_id` is now a 40-hex Swarm commit hash
 *  (`char[CARRIER_MESSAGE_ID_LEN]`) on every event that carries one,
 *  matching libjami's ground truth. The v3.1 `uint64_t message_id` was a
 *  v0.1 carryover — Jami never had numeric message IDs.
 *
 *  Threading: libjami signals fire on daemon worker threads. The shim
 *  marshals them into a bounded queue + clock fd (eventfd on Linux, self-pipe
 *  on macOS); `carrier_iterate()` drains the queue on the caller's thread, so
 *  the event callback always runs on the thread that called `carrier_iterate`.
 *  This preserves Antenna's single-threaded-callback invariant (see D6).
 *
 *  Accounts: one `Carrier*` per process (libjami is process-scoped), but a
 *  `Carrier*` may hold multiple accounts; every account-scoped API call takes
 *  an `account_id`. Accounts are provisioned asynchronously — a successful
 *  `carrier_create_account`/`carrier_load_account` call schedules the work,
 *  and the caller waits for `CARRIER_EVENT_ACCOUNT_READY` (or
 *  `CARRIER_EVENT_ACCOUNT_ERROR`) before sending messages (D7, D8).
 *  ---------------------------------------------------------------------------
 */

#ifndef CARRIER_H
#define CARRIER_H

#define CARRIER_VERSION_MAJOR 3
#define CARRIER_VERSION_MINOR 2
#define CARRIER_VERSION_PATCH 0
#define CARRIER_VERSION_STRING "3.2.0"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Carrier Carrier;

/* ---------------------------------------------------------------------------
 * Fixed-buffer sizes for inline event fields.
 *
 * Long-form bodies (message text, trust-request payloads, error text) are
 * passed as `const char *` pointers scoped to the callback invocation; the
 * caller must copy them to persist beyond the callback.
 * ---------------------------------------------------------------------------*/

#define CARRIER_URI_LEN              128  /* "jami:<40hex>" or SIP URI, with headroom */
#define CARRIER_ACCOUNT_ID_LEN        64  /* libjami internal account handle */
#define CARRIER_CONVERSATION_ID_LEN   64  /* Swarm commit hash (40 hex) + headroom */
#define CARRIER_MESSAGE_ID_LEN        64  /* Swarm message commit hash (40 hex) + headroom */
#define CARRIER_NAME_LEN             128  /* display name (also used for file display name) */
#define CARRIER_REACTION_LEN          32  /* emoji grapheme cluster, generous cap */
#define CARRIER_STATUS_LEN            16  /* presence status string ("online", "offline", ...) */
#define CARRIER_FILE_ID_LEN           96  /* "<40hex>_<digits>[.<ext>]" file identifier */
#define CARRIER_PATH_LEN            1024  /* file system path */
#define CARRIER_DEVICE_ID_LEN         96  /* Jami device fingerprint (40-hex DH or 64-hex Ed25519) */
#define CARRIER_PIN_LEN              128  /* "jami-auth://<40-or-64-hex>/<6char>" import token */

/* ---------------------------------------------------------------------------
 * Event types
 * ---------------------------------------------------------------------------*/

typedef enum {
    CARRIER_EVENT_CONNECTED,                /* libjami daemon up, account registered */
    CARRIER_EVENT_DISCONNECTED,             /* account lost registration */
    CARRIER_EVENT_ACCOUNT_READY,            /* account reached REGISTERED; safe to send */
    CARRIER_EVENT_ACCOUNT_ERROR,            /* registration failed; see `cause` */
    CARRIER_EVENT_SELF_ID,                  /* response to carrier_get_id() */
    CARRIER_EVENT_TRUST_REQUEST,            /* peer sent us a trust request */
    CARRIER_EVENT_CONTACT_ONLINE,           /* peer reachable on the DHT */
    CARRIER_EVENT_CONTACT_OFFLINE,          /* peer unreachable */
    CARRIER_EVENT_CONTACT_NAME,             /* peer published a display name */
    CARRIER_EVENT_TEXT_MESSAGE,             /* inbound 1:1 text message */
    CARRIER_EVENT_MESSAGE_SENT,             /* status update for an outbound message */
    CARRIER_EVENT_GROUP_MESSAGE,            /* inbound multi-party Swarm text message */
    CARRIER_EVENT_GROUP_PEER_JOIN,          /* a member joined a Swarm conversation */
    CARRIER_EVENT_GROUP_PEER_EXIT,          /* a member left or was banned */
    CARRIER_EVENT_CONVERSATION_REQUEST,     /* invitation to join a Swarm */
    CARRIER_EVENT_CONVERSATION_READY,       /* Swarm cloned/synced enough to use */
    CARRIER_EVENT_CONVERSATION_SYNC_FINISHED, /* per-account: all swarms synced */
    CARRIER_EVENT_SWARM_COMMIT,             /* raw Swarm git commit (DAG-level view) */
    CARRIER_EVENT_REACTION,                 /* peer reacted to a Swarm message (M4) */
    CARRIER_EVENT_PRESENCE,                 /* continuous presence update for a contact */
    CARRIER_EVENT_FILE_RECV,                /* incoming file-transfer offer in a Swarm */
    CARRIER_EVENT_FILE_PROGRESS,            /* file transfer started (per-direction, single-shot) */
    CARRIER_EVENT_FILE_COMPLETE,            /* file transfer reached terminal state */
    CARRIER_EVENT_DEVICE_LINK_PIN,          /* new-device side: import token minted (carries pin) */
    CARRIER_EVENT_DEVICE_LINKED,            /* device joined the account's known set */
    CARRIER_EVENT_DEVICE_UNLINKED,          /* device left the account's known set (e.g. revoked) */
    CARRIER_EVENT_ERROR,                    /* command-level failure */
    CARRIER_EVENT_SYSTEM,                   /* operational notice */
} CarrierEventType;

/* ---------------------------------------------------------------------------
 * Event data
 * ---------------------------------------------------------------------------*/

typedef struct {
    CarrierEventType type;
    int64_t timestamp;  /* CLOCK_REALTIME ms */

    /* Account that fired the event. Empty string for non-account events
     * (SYSTEM, and ERROR when the command had no account context). */
    char account_id[CARRIER_ACCOUNT_ID_LEN];

    /* CONNECTED and DISCONNECTED carry only the outer `account_id` and have
     * no union arm. All other events populate the union. */
    union {
        /* Fires when RegistrationStateChanged reaches REGISTERED. */
        struct {
            char self_uri[CARRIER_URI_LEN];     /* this account's 40-hex Jami ID */
            char display_name[CARRIER_NAME_LEN];
        } account_ready;

        /* Fires when registration fails or the account is unrecoverable. */
        struct {
            const char *cause;  /* libjami error string, callback-scoped */
        } account_error;

        /* Answer to carrier_get_id(). */
        struct {
            char self_uri[CARRIER_URI_LEN];
        } self_id;

        /* Inbound trust request. Reply via carrier_accept_trust_request
         * or carrier_discard_trust_request. */
        struct {
            char from_uri[CARRIER_URI_LEN];
            const char *payload;   /* VCard / greeting, callback-scoped, may be NULL */
            size_t      payload_len;
        } trust_request;

        struct {
            char contact_uri[CARRIER_URI_LEN];
        } contact_online;

        struct {
            char contact_uri[CARRIER_URI_LEN];
        } contact_offline;

        struct {
            char contact_uri[CARRIER_URI_LEN];
            char display_name[CARRIER_NAME_LEN];
        } contact_name;

        /* Inbound 1:1 text message. At M2, Swarm conversations are an
         * implementation detail of 1:1 messaging — `conversation_id` is
         * surfaced so M3+ callers (who address Swarms directly) see the
         * same field on both paths.
         *
         * `message_id` is the Swarm git commit hash (40-hex SHA-1) — used
         * to address the message for reactions, edits, and threading. */
        struct {
            char        contact_uri[CARRIER_URI_LEN];
            char        conversation_id[CARRIER_CONVERSATION_ID_LEN];
            char        message_id[CARRIER_MESSAGE_ID_LEN];
            const char *text;         /* UTF-8, callback-scoped, not NUL-bounded */
            size_t      text_len;
        } text_message;

        /* Delivery status update for an outbound message.
         * `status` values mirror libjami's MessageStates enum:
         *   0 UNKNOWN, 1 SENDING, 2 SENT, 3 READ, 4 FAILURE, 5 CANCELLED. */
        struct {
            char contact_uri[CARRIER_URI_LEN];
            char conversation_id[CARRIER_CONVERSATION_ID_LEN];
            char message_id[CARRIER_MESSAGE_ID_LEN];
            int  status;
        } message_sent;

        /* Inbound multi-party Swarm text message. Distinct from text_message
         * so callers can route 1:1 vs group conversations differently;
         * SwarmMessageReceived is the same libjami signal in both cases —
         * the shim discriminates by conversation privacy mode. */
        struct {
            char        conversation_id[CARRIER_CONVERSATION_ID_LEN];
            char        message_id[CARRIER_MESSAGE_ID_LEN];   /* Swarm commit hash */
            char        contact_uri[CARRIER_URI_LEN];         /* sender */
            char        display_name[CARRIER_NAME_LEN];       /* sender's display name (may be empty) */
            const char *text;                                 /* UTF-8, callback-scoped */
            size_t      text_len;
        } group_message;

        /* A member joined or was added to a Swarm conversation
         * (ConversationMemberEvent action=joins). */
        struct {
            char conversation_id[CARRIER_CONVERSATION_ID_LEN];
            char member_uri[CARRIER_URI_LEN];
        } group_peer_join;

        /* A member left or was banned from a Swarm conversation
         * (ConversationMemberEvent action=leave or banned). */
        struct {
            char conversation_id[CARRIER_CONVERSATION_ID_LEN];
            char member_uri[CARRIER_URI_LEN];
        } group_peer_exit;

        /* Inbound invitation to join a Swarm conversation. Reply via
         * carrier_accept_conversation_request or
         * carrier_decline_conversation_request. */
        struct {
            char conversation_id[CARRIER_CONVERSATION_ID_LEN];
            char contact_uri[CARRIER_URI_LEN];   /* inviter */
        } conversation_request;

        /* Swarm has cloned and synced enough to participate. Caller may
         * begin sending messages on this conversation_id after this fires. */
        struct {
            char conversation_id[CARRIER_CONVERSATION_ID_LEN];
        } conversation_ready;

        /* Per-account synchronization complete: all Swarms this account
         * is a member of have finished cloning. No conversation_id —
         * libjami's signal is account-scoped. */
        struct {
            char _placeholder;  /* keeps the arm non-empty */
        } conversation_sync_finished;

        /* Raw Swarm git commit (DAG-level view, fires for every commit
         * including non-text). Coarser than group_message; for consumers
         * that want the underlying DAG. */
        struct {
            char conversation_id[CARRIER_CONVERSATION_ID_LEN];
            char message_id[CARRIER_MESSAGE_ID_LEN];
            char contact_uri[CARRIER_URI_LEN];       /* commit author */
        } swarm_commit;

        /* Inbound emoji reaction on a Swarm message. `message_id` is the
         * commit being reacted to; `reaction_id` is the Swarm commit that
         * carries the reaction itself. `text` is the emoji body. */
        struct {
            char conversation_id[CARRIER_CONVERSATION_ID_LEN];
            char message_id[CARRIER_MESSAGE_ID_LEN];     /* reacted-to commit */
            char reaction_id[CARRIER_MESSAGE_ID_LEN];    /* the reaction commit */
            char contact_uri[CARRIER_URI_LEN];           /* reactor */
            char text[CARRIER_REACTION_LEN];             /* emoji */
        } reaction;

        /* Continuous presence update for a subscribed contact. `status` is
         * one of "online" or "offline" today; "away"/"busy" are reserved
         * in the vocab for when libjami starts surfacing those line_status
         * values. Fires after carrier_subscribe_presence has been called
         * for `contact_uri`. */
        struct {
            char contact_uri[CARRIER_URI_LEN];
            char status[CARRIER_STATUS_LEN];
        } presence;

        /* Inbound file-transfer offer. Surfaced from a Swarm commit of
         * type `application/data-transfer+json` — the body carries the
         * file metadata and libjami enriches it with the canonical
         * fileId (`<commitId>_<tid>[.<ext>]`). The receiver must call
         * carrier_accept_file with `message_id` + `file_id` to actually
         * download; without that, the offer remains pending in the Swarm
         * but no bytes flow. */
        struct {
            char     conversation_id[CARRIER_CONVERSATION_ID_LEN];
            char     contact_uri[CARRIER_URI_LEN];      /* sender */
            char     message_id[CARRIER_MESSAGE_ID_LEN];/* offer commit id (= libjami interactionId) */
            char     file_id[CARRIER_FILE_ID_LEN];
            char     filename[CARRIER_NAME_LEN];        /* libjami displayName */
            uint64_t size;                              /* totalSize bytes (from offer) */
        } file_recv;

        /* Transfer-started signal. libjami emits at most one of these
         * per direction per transfer (incoming side only); outgoing
         * transfers do not surface a progress tick — their first user-
         * visible event is FILE_COMPLETE. Consumers that want fine-grained
         * progress must keep their own tick or extend the shim later. */
        struct {
            char conversation_id[CARRIER_CONVERSATION_ID_LEN];
            char file_id[CARRIER_FILE_ID_LEN];
        } file_progress;

        /* Terminal state for a file transfer. `status` collapses libjami's
         * DataTransferEventCode into one of: "finished", "closed_by_host",
         * "closed_by_peer". True errors (invalid path, unjoinable peer,
         * timeout) are surfaced via CARRIER_EVENT_ERROR with class
         * "FileTransfer", not here. */
        struct {
            char conversation_id[CARRIER_CONVERSATION_ID_LEN];
            char file_id[CARRIER_FILE_ID_LEN];
            char status[CARRIER_STATUS_LEN];
        } file_complete;

        /* Device-linking import token. Emitted on a new device after
         * carrier_create_linking_account, when libjami mints the
         * `jami-auth://...` URI. The URI is valid for ~5 minutes; pass it
         * to an already-linked device via carrier_authorize_device. */
        struct {
            char pin[CARRIER_PIN_LEN];
        } device_link_pin;

        /* A device joined the account's known-devices set. Fires on every
         * device that sees the change — on the source side after
         * carrier_authorize_device completes, and on a freshly-linked new
         * device when its first KnownDevicesChanged populates beyond the
         * initial seed. `device_id` is the 40- or 64-hex fingerprint. */
        struct {
            char device_id[CARRIER_DEVICE_ID_LEN];
        } device_linked;

        /* A device was revoked from the account. Fires on the revoking
         * device after libjami's DeviceRevocationEnded(SUCCESS). Other
         * observers learn of the revocation via certificate-revocation-list
         * propagation, not as a typed event today. */
        struct {
            char device_id[CARRIER_DEVICE_ID_LEN];
        } device_unlinked;

        /* A command was rejected. `command` is the RDF local name (e.g.
         * "SendTrustRequest") when triaged at the dispatch layer; otherwise
         * an internal tag. `class_` is a short symbolic category (e.g.
         * "NotTrusted", "NoSuchAccount", "InvalidUri"); `text` is human-
         * readable. */
        struct {
            char        command[64];
            char        class_[64];
            const char *text;   /* callback-scoped */
        } error;

        /* Operational notice (startup banner, shutdown, etc.). */
        struct {
            const char *text;   /* callback-scoped */
        } system;
    };
} CarrierEvent;

/* ---------------------------------------------------------------------------
 * Callback
 * ---------------------------------------------------------------------------*/

typedef void (*carrier_event_cb)(const CarrierEvent *event, void *userdata);

/* ---------------------------------------------------------------------------
 * Logging
 *
 * Operational DEBUG/INFO/WARN/ERROR records distinct from protocol events.
 * The library never writes to stdout or stderr itself — all log output is
 * delivered through a caller-registered callback. Embedders (e.g. Antenna)
 * route records into their own logger; carrier-cli provides a default sink
 * that writes to stderr in canonical format.
 * ---------------------------------------------------------------------------*/

typedef enum {
    CARRIER_LOG_ERROR = 0,
    CARRIER_LOG_WARN,
    CARRIER_LOG_INFO,
    CARRIER_LOG_DEBUG,
} CarrierLogLevel;

#define CARRIER_LOG_TAG_LEN     16
#define CARRIER_LOG_MESSAGE_LEN 512

typedef struct {
    CarrierLogLevel level;
    int64_t         timestamp_ms;                  /* CLOCK_REALTIME ms */
    char            tag[CARRIER_LOG_TAG_LEN];      /* e.g. "JAMI", "DHT", "SHIM" */
    char            message[CARRIER_LOG_MESSAGE_LEN]; /* single-line, no trailing newline */
} CarrierLogRecord;

typedef void (*carrier_log_cb)(const CarrierLogRecord *record, void *userdata);

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------*/

/*
 * Create a new Carrier instance.
 *
 * data_dir:     absolute path where libjami stores account archives and
 *               conversation history. The shim exports XDG_DATA_HOME=data_dir
 *               before calling libjami::init() so account data lands under
 *               this tree (D11). Pass NULL to accept the platform default
 *               (macOS: ~/Library/Application Support/jami; Linux:
 *               $XDG_DATA_HOME/jami).
 * log_cb:       log sink, or NULL to disable logging entirely. Captures
 *               records emitted during construction itself (e.g. libjami
 *               init failures, initial bootstrap).
 * log_userdata: opaque pointer passed back to log_cb on every record.
 *
 * Default log level after construction is CARRIER_LOG_ERROR — the callback
 * sees errors only. Call carrier_set_log_level() to widen the filter.
 *
 * On return, libjami is initialized and its signal handlers are registered,
 * but no account is loaded. Call carrier_create_account() or
 * carrier_load_account() next.
 *
 * One `Carrier*` per process. Returns NULL on libjami::init() failure.
 */
Carrier *carrier_new(const char    *data_dir,
                     carrier_log_cb log_cb,
                     void          *log_userdata);

/*
 * Free a Carrier instance and all associated resources.
 * Account archives are auto-persisted by libjami; no explicit save needed.
 */
void carrier_free(Carrier *c);

/*
 * Drain the event queue onto the caller's thread, invoking the event
 * callback per event. Must be called periodically, or whenever
 * carrier_clock_fd() becomes readable.
 * Returns 0 on success, -1 on error.
 */
int carrier_iterate(Carrier *c);

/*
 * Conservative failsafe interval (in ms) between carrier_iterate() calls
 * when the caller is not using the clock fd. Typical: ~5000.
 */
int carrier_iteration_interval(Carrier *c);

/*
 * Read-end of a self-pipe (macOS) or eventfd (Linux) that becomes readable
 * when the event queue is non-empty. Intended for poll()-based event loops
 * that want zero-idle-CPU wakeups instead of a polling timer.
 *
 * The caller must drain the fd (or call carrier_iterate, which drains it)
 * after each wake to reset the signal. Returns -1 if the fd is unavailable
 * (e.g. in degraded init).
 */
int carrier_clock_fd(Carrier *c);

/* ---------------------------------------------------------------------------
 * Event & log callback configuration
 * ---------------------------------------------------------------------------*/

void carrier_set_event_callback(Carrier *c, carrier_event_cb cb, void *userdata);

/* Register (or clear) the log sink. NULL cb disables all logging. */
void carrier_set_log_callback(Carrier *c, carrier_log_cb cb, void *userdata);

/* Set the minimum level. Records below this are discarded without formatting.
 * Default: CARRIER_LOG_ERROR. */
void carrier_set_log_level(Carrier *c, CarrierLogLevel level);

/* ---------------------------------------------------------------------------
 * Accounts
 *
 * Account provisioning is asynchronous. These calls enqueue the work with
 * libjami and return immediately; the caller waits for
 * CARRIER_EVENT_ACCOUNT_READY (success) or CARRIER_EVENT_ACCOUNT_ERROR
 * (failure) before issuing account-scoped API calls.
 * ---------------------------------------------------------------------------*/

/*
 * Create a fresh Jami account (new keypair, new 40-hex URI).
 *
 * display_name:    human-readable name; published to contacts. May be NULL
 *                  or "" to leave unset.
 * out_account_id:  buffer receiving the newly-assigned libjami account ID,
 *                  NUL-terminated. Must be at least CARRIER_ACCOUNT_ID_LEN
 *                  bytes. Populated synchronously before this call returns.
 *
 * The AccountReady event (once it fires) will carry this account_id in the
 * outer struct and the new self URI in `account_ready.self_uri`.
 *
 * Returns 0 on success, -1 on failure (check log for libjami cause).
 */
int carrier_create_account(Carrier    *c,
                           const char *display_name,
                           char        out_account_id[CARRIER_ACCOUNT_ID_LEN]);

/*
 * Attach an existing account that is already on disk under data_dir.
 *
 * account_id: the libjami account handle returned by a prior
 *             carrier_create_account call (or recorded from an earlier
 *             session).
 *
 * Same async semantics as carrier_create_account.
 *
 * Returns 0 on success, -1 if no such account exists under data_dir.
 */
int carrier_load_account(Carrier *c, const char *account_id);

/* ---------------------------------------------------------------------------
 * Identity & presence
 * ---------------------------------------------------------------------------*/

/*
 * Request the self URI for an account. Fires CARRIER_EVENT_SELF_ID.
 * Returns 0 on enqueue, -1 if account_id is unknown.
 *
 * (Callers that already have the self URI from AccountReady do not need
 * to call this — it exists so RDF-level `carrier:GetId` still round-trips.)
 */
int carrier_get_id(Carrier *c, const char *account_id);

/*
 * Update the account's published display name. Propagates to contacts on
 * next presence refresh. Returns 0/-1.
 */
int carrier_set_nick(Carrier *c, const char *account_id, const char *nick);

/* ---------------------------------------------------------------------------
 * Trust (contacts)
 *
 * Jami contacts are established via a two-sided trust exchange: one side
 * calls carrier_send_trust_request, the other sees CARRIER_EVENT_TRUST_REQUEST
 * and replies with carrier_accept_trust_request (or carrier_discard_trust_request).
 * Only after mutual trust does 1:1 messaging succeed (D10).
 * ---------------------------------------------------------------------------*/

/*
 * Send a trust request to `contact_uri`.
 *
 * message: optional human-readable greeting embedded in the request
 *          payload. May be NULL or "".
 *
 * Returns 0 on enqueue, -1 on invalid URI / unknown account.
 */
int carrier_send_trust_request(Carrier    *c,
                               const char *account_id,
                               const char *contact_uri,
                               const char *message);

/*
 * Accept an inbound trust request from `contact_uri`. After this call,
 * libjami creates a 1:1 Swarm between the two parties (emits
 * ConversationReady on the shim's internal channel; not surfaced until M3).
 */
int carrier_accept_trust_request(Carrier    *c,
                                 const char *account_id,
                                 const char *contact_uri);

/*
 * Reject an inbound trust request from `contact_uri`. Silently drops it;
 * the sender is not notified.
 */
int carrier_discard_trust_request(Carrier    *c,
                                  const char *account_id,
                                  const char *contact_uri);

/*
 * Remove an established contact. Matches libjami `removeContact(..., ban=false)`;
 * banning is deferred until a real use case demands it.
 */
int carrier_remove_contact(Carrier    *c,
                           const char *account_id,
                           const char *contact_uri);

/* ---------------------------------------------------------------------------
 * Messaging (1:1)
 *
 * Under the hood, 1:1 Jami messages travel over a one_to_one Swarm. The
 * shim resolves `(account_id, contact_uri) → conversation_id` via a cache
 * seeded from libjami's getConversations() on account load; on cache miss
 * it creates the 1:1 Swarm (requires pre-existing trust).
 *
 * For multi-party / group Swarms, address by conversation_id directly via
 * carrier_send_conversation_message (below).
 * ---------------------------------------------------------------------------*/

/*
 * Send a 1:1 text message to `contact_uri`. Fails if no trust relationship
 * is established — the caller must send (and have accepted) a trust request
 * first.
 *
 * Delivery status is reported asynchronously via CARRIER_EVENT_MESSAGE_SENT,
 * correlated by the libjami message_id that libjami assigns.
 *
 * Returns 0 on enqueue, -1 on immediate validation failure (invalid URI,
 * unknown account, not trusted). The NotTrusted case also emits
 * CARRIER_EVENT_ERROR with class "NotTrusted" for dispatch symmetry.
 */
int carrier_send_message(Carrier    *c,
                         const char *account_id,
                         const char *contact_uri,
                         const char *text);

/* ---------------------------------------------------------------------------
 * Swarm conversations (multi-party)
 *
 * Jami's Swarms back both 1:1 and multi-party conversations. The 1:1 case
 * is hidden behind the Messaging API above (caller addresses by contact
 * URI; the shim picks the Swarm). For everything else — admin-invites,
 * invites-only, public — addresses are explicit conversation IDs (Swarm
 * git tree hashes).
 *
 * Lifecycle:
 *   1. Either party calls carrier_create_conversation, OR a
 *      CARRIER_EVENT_CONVERSATION_REQUEST arrives and the local side
 *      replies with carrier_accept_conversation_request.
 *   2. Once the underlying Swarm has cloned/synced enough to be usable,
 *      CARRIER_EVENT_CONVERSATION_READY fires. Senders SHOULD wait for
 *      this event before issuing carrier_send_conversation_message —
 *      libjami may queue messages sent earlier, but delivery is not
 *      guaranteed until the Swarm is ready.
 *   3. Members can be added with carrier_invite_to_conversation;
 *      additions and departures surface as
 *      CARRIER_EVENT_GROUP_PEER_JOIN / CARRIER_EVENT_GROUP_PEER_EXIT.
 *   4. carrier_remove_conversation removes the conversation locally;
 *      libjami marks the conversation as removed (and ban-cleans Swarm
 *      state). The conversation_id may still appear in inbound peer
 *      events until the remote side notices.
 * ---------------------------------------------------------------------------*/

/*
 * Create a new Swarm conversation.
 *
 * privacy: one of "one_to_one", "admin_invites", "invites_only", "public".
 *          See arch/namespaces.md for the policy summary. NULL or "" is
 *          treated as "invites_only" (libjami's default mode).
 * out_conversation_id: buffer receiving the new conversation hash,
 *                      NUL-terminated. Must be at least
 *                      CARRIER_CONVERSATION_ID_LEN bytes. Populated
 *                      synchronously before this call returns.
 *
 * CARRIER_EVENT_CONVERSATION_READY will fire once the local Swarm has
 * been written to disk (typically immediate for self-created Swarms).
 *
 * Returns 0 on success, -1 on libjami failure.
 */
int carrier_create_conversation(Carrier    *c,
                                const char *account_id,
                                const char *privacy,
                                char        out_conversation_id[CARRIER_CONVERSATION_ID_LEN]);

/*
 * Send a text message to a Swarm conversation by ID. For 1:1 Swarms, the
 * caller-friendly entry point is carrier_send_message (which resolves the
 * conversation by contact URI). This call is for multi-party Swarms.
 *
 * Delivery status arrives asynchronously via CARRIER_EVENT_MESSAGE_SENT,
 * correlated by the libjami message_id.
 *
 * Returns 0 on enqueue, -1 on validation failure.
 */
int carrier_send_conversation_message(Carrier    *c,
                                      const char *account_id,
                                      const char *conversation_id,
                                      const char *text);

/*
 * Accept an inbound invitation to a Swarm conversation. The Swarm clones
 * and CARRIER_EVENT_CONVERSATION_READY fires once enough of the DAG is
 * available to participate.
 */
int carrier_accept_conversation_request(Carrier    *c,
                                        const char *account_id,
                                        const char *conversation_id);

/*
 * Decline an inbound Swarm invitation. The local-side state is cleared;
 * the inviting side is notified via ConversationRequestDeclined and may
 * stop trying to deliver to us.
 */
int carrier_decline_conversation_request(Carrier    *c,
                                         const char *account_id,
                                         const char *conversation_id);

/*
 * Add a contact to a Swarm conversation. The peer must already be a
 * trusted contact of `account_id`. The added member receives a
 * CARRIER_EVENT_CONVERSATION_REQUEST; on acceptance, both sides see
 * CARRIER_EVENT_GROUP_PEER_JOIN.
 *
 * Returns 0 on enqueue, -1 on invalid URI / unknown account /
 * unknown conversation.
 */
int carrier_invite_to_conversation(Carrier    *c,
                                   const char *account_id,
                                   const char *conversation_id,
                                   const char *contact_uri);

/*
 * Remove a Swarm conversation locally. For multi-party Swarms this drops
 * our view of the conversation; other members continue. For 1:1 Swarms
 * this also drops the underlying contact relationship (libjami semantics).
 *
 * Returns 0 on enqueue, -1 on unknown account / conversation.
 */
int carrier_remove_conversation(Carrier    *c,
                                const char *account_id,
                                const char *conversation_id);

/* ---------------------------------------------------------------------------
 * Reactions
 *
 * Reactions ride on libjami's sendMessage(flag=2) — the daemon writes a
 * Swarm commit with a `react-to` link to the target message. Inbound
 * reactions surface as CARRIER_EVENT_REACTION (typed event) rather than
 * as a separate ReactionAdded/Removed pair: the granularity matches the
 * vocab and lets receivers maintain a per-message reaction set without
 * mirroring libjami's internal commit DAG.
 * ---------------------------------------------------------------------------*/

/*
 * Add an emoji reaction to a Swarm message.
 *
 * conversation_id: conversation containing the reacted-to message.
 * message_id:      target message's Swarm commit hash (40-hex). Obtained
 *                  from CARRIER_EVENT_TEXT_MESSAGE / GROUP_MESSAGE.
 * reaction:        emoji body, typically a single grapheme.
 *
 * Returns 0 on enqueue, -1 on validation failure.
 */
int carrier_send_reaction(Carrier    *c,
                          const char *account_id,
                          const char *conversation_id,
                          const char *message_id,
                          const char *reaction);

/* ---------------------------------------------------------------------------
 * Presence
 *
 * Continuous presence ("online"/"offline") for individual contacts, distinct
 * from the binary CONTACT_ONLINE/CONTACT_OFFLINE pair which fires
 * unconditionally on DHT reachability. carrier_subscribe_presence registers
 * a buddy with libjami's PresenceManager; CARRIER_EVENT_PRESENCE fires on
 * every status change thereafter. PresenceManager binds to OpenDHT, so the
 * first event for a freshly-subscribed contact may take seconds.
 * ---------------------------------------------------------------------------*/

/*
 * Subscribe or unsubscribe from continuous presence updates for a contact.
 *
 * contact_uri: 40-hex Jami URI of the peer.
 * subscribe:   true to begin subscription; false to stop it. Idempotent
 *              in either direction — re-subscribing an already-subscribed
 *              buddy is a no-op, as is unsubscribing one we don't know.
 *
 * Returns 0 on enqueue, -1 on validation failure (missing account, bad URI).
 */
int carrier_subscribe_presence(Carrier    *c,
                               const char *account_id,
                               const char *contact_uri,
                               bool        subscribe);

/* ---------------------------------------------------------------------------
 * File transfer
 *
 * Files ride on Jami's Swarm DAG: the sender posts an
 * `application/data-transfer+json` commit (offer) and libjami serves the
 * file contents over a separate Swarm channel once a peer asks for them.
 * The receiver sees the offer as CARRIER_EVENT_FILE_RECV and chooses
 * whether to accept; carrier_accept_file then triggers libjami's
 * downloadFile and bytes start flowing. No auto-download — the embedder
 * decides destination paths.
 *
 * Lifecycle (sender):
 *   1. carrier_send_file with a local source path. Returns 0 on enqueue.
 *      libjami sha3-hashes the file off-thread, then commits the offer.
 *   2. When the peer pulls the file, FILE_COMPLETE fires with
 *      status="finished" (success), "closed_by_peer" (peer cancelled),
 *      or "closed_by_host" (we cancelled).
 *
 * Lifecycle (receiver):
 *   1. FILE_RECV arrives carrying file_id, message_id (the offer commit),
 *      filename, size, and the sender's contact_uri.
 *   2. To accept: carrier_accept_file(account, conv, message_id, file_id,
 *      destination_path). FILE_PROGRESS fires once when the download
 *      starts; FILE_COMPLETE fires with the terminal status when it ends.
 *   3. To decline: do nothing (or carrier_cancel_file to free local state).
 * ---------------------------------------------------------------------------*/

/*
 * Offer a local file as a Swarm attachment in `conversation_id`.
 *
 * path:         absolute path to a regular file the sender owns. libjami
 *               reads it lazily as the peer pulls; the file must remain
 *               readable for the lifetime of the transfer.
 * display_name: name shown to the peer in FILE_RECV.filename. NULL or ""
 *               falls back to basename(path).
 *
 * Returns 0 on enqueue (the actual commit is async — sha3sum runs on a
 * computation thread). Returns -1 on validation failure (NULL args).
 * libjami signals path-not-found via OnConversationError, surfaced as a
 * carrier:Error with class="FileTransfer".
 */
int carrier_send_file(Carrier    *c,
                      const char *account_id,
                      const char *conversation_id,
                      const char *path,
                      const char *display_name);

/*
 * Accept an inbound file offer. Triggers libjami's downloadFile, which
 * connects to the offering peer and streams bytes to `path`.
 *
 * message_id: offer commit id from FILE_RECV.message_id (libjami spells
 *             this `interactionId` at the API boundary).
 * file_id:    canonical file identifier from FILE_RECV.file_id.
 * path:       absolute destination path. The directory must exist; libjami
 *             writes to a `.tmp` sibling and renames on success.
 *
 * Returns 0 on enqueue, -1 on validation failure.
 */
int carrier_accept_file(Carrier    *c,
                        const char *account_id,
                        const char *conversation_id,
                        const char *message_id,
                        const char *file_id,
                        const char *path);

/*
 * Cancel an in-flight transfer (incoming or outgoing). libjami emits a
 * terminal DataTransferEvent that surfaces as FILE_COMPLETE with
 * status="closed_by_host" (we cancelled) or "closed_by_peer" (peer
 * cancelled before we got here).
 *
 * Returns 0 on enqueue, -1 on validation failure / unknown id.
 */
int carrier_cancel_file(Carrier    *c,
                        const char *account_id,
                        const char *conversation_id,
                        const char *file_id);

/* ---------------------------------------------------------------------------
 * Device linking
 *
 * libjami's flow is asymmetric:
 *   - The new device creates an account in linking-mode (archive_url =
 *     "jami-auth"), and libjami emits a `jami-auth://...` import URI via
 *     ConfigurationSignal::DeviceAuthStateChanged(TOKEN_AVAILABLE). The shim
 *     surfaces that as CARRIER_EVENT_DEVICE_LINK_PIN.
 *   - The user copies the URI to an already-linked device, which calls
 *     carrier_authorize_device. libjami's AddDeviceStateChanged hits
 *     AUTHENTICATING (the shim auto-confirms via confirmAddDevice), then
 *     DONE+success completes the auth channel. Both sides eventually see a
 *     KnownDevicesChanged carrying the new device, which the shim diffs
 *     against a per-account cache to emit CARRIER_EVENT_DEVICE_LINKED.
 *   - carrier_revoke_device runs the inverse on the source side. libjami's
 *     KnownDevicesChanged does NOT fire on remove, so the shim binds
 *     ConfigurationSignal::DeviceRevocationEnded and emits
 *     CARRIER_EVENT_DEVICE_UNLINKED on success.
 *
 * URI lifetime is ~5 minutes (libjami's OP_TIMEOUT). The shim assumes
 * archives have no password (matching carrier_create_account); password-
 * protected linking would need a follow-up API to feed the password back.
 * ---------------------------------------------------------------------------*/

/*
 * Begin device-linking on a NEW device. Creates a fresh account in
 * linking-mode and asks libjami to mint an import token. Once libjami
 * mints the URI, CARRIER_EVENT_DEVICE_LINK_PIN fires carrying it; once the
 * source device authorizes the token and the archive transfers, the
 * standard CARRIER_EVENT_ACCOUNT_READY path fires with the shared self URI.
 *
 * out_account_id: the linking-mode account handle, populated synchronously.
 *                 Subsequent commands (e.g. SubscribePresence on the
 *                 freshly-synced contact list) target this account_id.
 *                 Note that until ACCOUNT_READY arrives, calling
 *                 account-scoped APIs on this id will fail.
 *
 * Returns 0 on success, -1 on libjami failure.
 */
int carrier_create_linking_account(Carrier *c,
                                   char     out_account_id[CARRIER_ACCOUNT_ID_LEN]);

/*
 * Authorize a device-link import URI on the SOURCE (already-linked) side.
 *
 * pin: the `jami-auth://...` URI from a CARRIER_EVENT_DEVICE_LINK_PIN
 *      event on the new device. Validated by libjami; invalid URIs return
 *      a negative op_id and surface as CARRIER_EVENT_ERROR with
 *      class="DeviceLink".
 *
 * On success, libjami runs the auth channel; CARRIER_EVENT_DEVICE_LINKED
 * fires once the new device's fingerprint propagates into the source's
 * known-devices set. On failure the shim emits a DONE-state Error.
 *
 * Returns 0 on enqueue, -1 on validation / libjami failure.
 */
int carrier_authorize_device(Carrier    *c,
                             const char *account_id,
                             const char *pin);

/*
 * Revoke a previously-linked device. Removes the deviceId from the
 * account's certificate authority; KnownDevicesChanged fires with the
 * device gone, and CARRIER_EVENT_DEVICE_UNLINKED follows.
 *
 * device_id: 40- or 64-hex fingerprint, as seen in DEVICE_LINKED.device_id.
 *
 * Returns 0 on enqueue, -1 if libjami's revokeDevice rejects the id.
 */
int carrier_revoke_device(Carrier    *c,
                          const char *account_id,
                          const char *device_id);

/* ---------------------------------------------------------------------------
 * RDF canonicalization
 *
 * Implements the RDNA2015 subset needed for carrier's single-blank-node
 * Turtle events: parse to triples, replace blank nodes with _:c14n0, sort
 * N-Quads lines, SHA-256 the result. The hash is stable across all
 * serializations of the same RDF graph.
 * ---------------------------------------------------------------------------*/

/*
 * Compute the canonical RDNA2015 SHA-256 hash of a Turtle document.
 * out receives 32 bytes. Returns 0 on success, -1 on parse error.
 */
int carrier_rdf_hash(const char *turtle, size_t len, uint8_t out[32]);

/*
 * Copy the hash of the most recently sent message into out[32].
 * Returns 0 if a hash is available, -1 if no message has been sent yet.
 * The hash covers the RDF content of the message text as passed to
 * carrier_send_message / carrier_send_conversation_message.
 */
int carrier_last_send_hash(const Carrier *c, uint8_t out[32]);

/*
 * Send a large RDF object as a content-addressed file over a Swarm
 * conversation. The Turtle is written to <tmp_dir>/<hex_hash>.ttl, then
 * sent via carrier_send_file(). The filename IS the canonical hash, so
 * the receiver can verify integrity by hashing the received file.
 *
 * tmp_dir:  writable directory for the temp file (e.g. "/tmp").
 * out_hash: if non-NULL, receives the 32-byte canonical hash on success.
 *
 * Returns 0 on success, -1 on hash/write/send failure.
 */
int carrier_send_rdf_object(Carrier    *c,
                            const char *account_id,
                            const char *conversation_id,
                            const char *turtle,
                            size_t      turtle_len,
                            const char *tmp_dir,
                            uint8_t     out_hash[32]);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CARRIER_H */
