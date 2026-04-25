/*  carrier_internal.hpp
 *
 *  Internal state for the Jami-backed Carrier shim. Not part of the public
 *  API. Visible only to C++ translation units; C TUs (turtle_*, main_cli)
 *  include carrier.h and interact through the C API.
 *
 *  Internal C files (carrier_events, carrier_log) compile as .cc so they
 *  can read this header directly.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#ifndef CARRIER_INTERNAL_HPP
#define CARRIER_INTERNAL_HPP

#include "carrier.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

/* ---------------------------------------------------------------------------
 * Event queue
 *
 * libjami signal handlers fire on daemon worker threads. Each handler
 * translates its arguments into a CarrierEvent, appends it to `queue`, and
 * writes one byte to `wake_write_fd` so carrier_iterate() wakes (either via
 * its own drain loop or via an embedder polling `wake_read_fd`). Handlers
 * never call back into libjami — push-and-return only (D6).
 *
 * CarrierEvent payloads include pointer fields (message text, error text,
 * trust-request payload) scoped to the callback invocation. The shim holds
 * the backing storage alongside each queued event in a parallel structure
 * so the pointer remains valid for the duration of the callback.
 * ---------------------------------------------------------------------------*/

struct QueuedEvent
{
    CarrierEvent ev{};

    /* Backing storage for pointer fields in the event. Populated when the
     * corresponding CarrierEvent union arm carries a const char* / const
     * uint8_t* pointer. Left empty otherwise. */
    std::string message_text;     /* text_message.text / system.text / error.text / account_error.cause */
    std::string trust_payload;    /* trust_request.payload — libjami returns bytes, stored as std::string */
};

/* ---------------------------------------------------------------------------
 * Per-account state
 *
 * One entry per libjami account the shim is tracking. Populated at
 * carrier_create_account / carrier_load_account; refreshed when signals
 * fire (RegistrationStateChanged, ConversationReady).
 * ---------------------------------------------------------------------------*/

struct AccountState
{
    std::string self_uri;      /* 40-hex Jami URI, filled in on AccountReady */
    std::string display_name;
    bool        registered = false;

    /* Cache: peer_uri → conversation_id for 1:1 Swarms. Seeded from
     * libjami::getConversations() on account load; updated on
     * ConversationReady signals. Guarded by Carrier::accounts_mtx. */
    std::unordered_map<std::string, std::string> peer_conversations;

    /* Cache: conversation_id → privacy mode ("one_to_one", "admin_invites",
     * "invites_only", "public"). Used to discriminate inbound 1:1 vs
     * multi-party SwarmMessageReceived signals (1:1 → TextMessage,
     * everything else → GroupMessage). Populated lazily on first signal
     * for a given conversation, and on carrier_create_conversation /
     * carrier_accept_conversation_request. Guarded by accounts_mtx. */
    std::unordered_map<std::string, std::string> conversation_modes;
};

/* ---------------------------------------------------------------------------
 * Carrier instance
 *
 * One per process (libjami::init/fini is process-scoped). Multi-account is
 * supported within a single Carrier* — each account-scoped API call takes
 * an account_id that indexes `accounts`.
 * ---------------------------------------------------------------------------*/

struct Carrier
{
    /* --- Callbacks ------------------------------------------------------ */

    carrier_event_cb event_cb = nullptr;
    void            *event_userdata = nullptr;

    carrier_log_cb   log_cb = nullptr;
    void            *log_userdata = nullptr;
    CarrierLogLevel  log_level = CARRIER_LOG_ERROR;

    /* --- Event queue ---------------------------------------------------- */

    std::mutex              queue_mtx;
    std::deque<QueuedEvent> queue;

    /* Self-pipe (macOS) / eventfd (Linux) used to wake carrier_iterate()
     * and any embedder polling on the read end. */
    int wake_read_fd  = -1;   /* exposed via carrier_clock_fd() */
    int wake_write_fd = -1;   /* written by signal handlers; on Linux, same as read fd (eventfd) */

    /* --- Accounts ------------------------------------------------------- */

    std::mutex                                  accounts_mtx;
    std::unordered_map<std::string, AccountState> accounts;

    /* --- libjami init state --------------------------------------------- */

    std::string data_dir;            /* absolute path; exported as XDG_DATA_HOME */
    bool        libjami_initialized = false;
};

/* ---------------------------------------------------------------------------
 * Shared helpers (defined in carrier_jami.cc or carrier_jami_signals.cc)
 * ---------------------------------------------------------------------------*/

/* Push an event onto the queue and signal the clock fd. Thread-safe.
 * Takes ownership of `qe` (moved in). */
void carrier_push_event(Carrier *c, QueuedEvent &&qe);

/* CLOCK_REALTIME milliseconds. Used for event timestamps. */
std::int64_t carrier_now_ms();

/* Register libjami signal handlers against `c`. Called from carrier_new
 * after libjami::init + libjami::start. */
void carrier_register_signals(Carrier *c);

#endif /* CARRIER_INTERNAL_HPP */
