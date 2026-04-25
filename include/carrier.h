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
 *  Scope as of M4a (this header): lifecycle, account creation/loading,
 *  self-ID, trust requests, 1:1 text messaging, multi-party Swarm
 *  conversations (create, send, accept/decline invitation, invite member,
 *  remove), reactions, continuous presence, device linking, and Swarm file
 *  transfers. Calls (audio/video) deferred to M4b. Functions and event
 *  types are added in later milestones as they land — there are no
 *  forward-declared placeholders for unimplemented surface.
 *
 *  v3.2 break (M4a): `message_id` is now a 40-hex Swarm commit hash
 *  (`char[CARRIER_MESSAGE_ID_LEN]`) on every event that carries one,
 *  matching libjami's ground truth. The v3.1 `uint64_t message_id` was a
 *  Tox-era artifact — Jami never had numeric message IDs.
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
#define CARRIER_VERSION_STRING "3.2.0-dev"

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
#define CARRIER_NAME_LEN             128  /* display name */
#define CARRIER_REACTION_LEN          32  /* emoji grapheme cluster, generous cap */
#define CARRIER_STATUS_LEN            16  /* presence status string ("online", "offline", ...) */

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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CARRIER_H */
