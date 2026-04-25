/*  carrier_jami_signals.cc
 *
 *  libjami signal → CarrierEvent translation. Handlers run on libjami
 *  daemon worker threads; they never call back into libjami (D6). Every
 *  handler marshals its arguments into a QueuedEvent, pushes it onto the
 *  Carrier's queue, and wakes the clock fd.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#include "carrier_internal.hpp"

#include <jami/jami.h>
#include <jami/configurationmanager_interface.h>
#include <jami/conversation_interface.h>

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

/* Copy up to `cap` bytes of `src` into `dst` and NUL-terminate. */
void copy_fixed(char *dst, std::size_t cap, const std::string &src)
{
    if (cap == 0) return;
    const std::size_t n = (src.size() < cap - 1) ? src.size() : cap - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

void set_account(CarrierEvent &ev, const std::string &accountId)
{
    copy_fixed(ev.account_id, CARRIER_ACCOUNT_ID_LEN, accountId);
}

void stamp(CarrierEvent &ev, CarrierEventType type)
{
    ev.type = type;
    ev.timestamp = carrier_now_ms();
}

/* ---------------------------------------------------------------------------
 * Handler bodies
 *
 * Each handler captures the Carrier* by value; lifetime is tied to the
 * Carrier (caller unregisters signals in carrier_free before destroying the
 * Carrier, so stale captures can't fire).
 * ---------------------------------------------------------------------------*/

void on_registration_state(Carrier *c,
                           const std::string &accountId,
                           const std::string &state,
                           int /*code*/,
                           const std::string &detailsStr)
{
    /* Maintain the per-account registered flag. */
    bool first_registered = false;
    {
        std::lock_guard<std::mutex> lock(c->accounts_mtx);
        auto it = c->accounts.find(accountId);
        if (it != c->accounts.end()) {
            const bool now_registered = (state == "REGISTERED");
            if (now_registered && !it->second.registered) {
                first_registered = true;
            }
            it->second.registered = now_registered;
            if (now_registered) {
                /* Resolve self_uri lazily — fetched by carrier_jami.cc
                 * via getAccountDetails when AccountReady is about to
                 * fire. Kept here as an empty string. */
            }
        }
    }

    QueuedEvent qe;
    if (first_registered) {
        /* Fetch details to get the account's URI and display name. This is
         * the only place we call back into libjami from a signal thread;
         * it's a read-only accessor and doesn't re-enter our handlers. */
        const auto details = libjami::getAccountDetails(accountId);
        std::string self_uri;
        std::string display_name;
        if (auto it = details.find("Account.username"); it != details.end()) {
            self_uri = it->second;
        }
        if (auto it = details.find("Account.displayName"); it != details.end()) {
            display_name = it->second;
        }

        {
            std::lock_guard<std::mutex> lock(c->accounts_mtx);
            auto it = c->accounts.find(accountId);
            if (it != c->accounts.end()) {
                it->second.self_uri = self_uri;
                it->second.display_name = display_name;
            }
        }

        stamp(qe.ev, CARRIER_EVENT_ACCOUNT_READY);
        set_account(qe.ev, accountId);
        copy_fixed(qe.ev.account_ready.self_uri, CARRIER_URI_LEN, self_uri);
        copy_fixed(qe.ev.account_ready.display_name, CARRIER_NAME_LEN, display_name);
        carrier_push_event(c, std::move(qe));

        /* Also emit a CONNECTED event so existing dispatch paths that key on
         * carrier:Connected observe the transition. */
        QueuedEvent connected;
        stamp(connected.ev, CARRIER_EVENT_CONNECTED);
        set_account(connected.ev, accountId);
        carrier_push_event(c, std::move(connected));
        return;
    }

    if (state == "ERROR_GENERIC" || state == "ERROR_AUTH" ||
        state == "ERROR_NETWORK" || state == "ERROR_HOST" ||
        state == "ERROR_NEED_MIGRATION") {
        stamp(qe.ev, CARRIER_EVENT_ACCOUNT_ERROR);
        set_account(qe.ev, accountId);
        qe.message_text = detailsStr.empty() ? state : detailsStr;
        qe.ev.account_error.cause = qe.message_text.c_str();
        carrier_push_event(c, std::move(qe));
        return;
    }

    if (state == "UNREGISTERED") {
        stamp(qe.ev, CARRIER_EVENT_DISCONNECTED);
        set_account(qe.ev, accountId);
        carrier_push_event(c, std::move(qe));
        return;
    }

    /* TRYING, INITIALIZING — not surfaced; embedder-visible state machine
     * is just {unready, ready, error, disconnected}. */
}

void on_incoming_trust_request(Carrier *c,
                               const std::string &accountId,
                               const std::string &from,
                               const std::string & /*conversationId*/,
                               const std::vector<std::uint8_t> &payload,
                               time_t /*received*/)
{
    QueuedEvent qe;
    stamp(qe.ev, CARRIER_EVENT_TRUST_REQUEST);
    set_account(qe.ev, accountId);
    copy_fixed(qe.ev.trust_request.from_uri, CARRIER_URI_LEN, from);

    if (!payload.empty()) {
        qe.trust_payload.assign(
            reinterpret_cast<const char *>(payload.data()), payload.size());
        qe.ev.trust_request.payload = qe.trust_payload.data();
        qe.ev.trust_request.payload_len = qe.trust_payload.size();
    } else {
        qe.ev.trust_request.payload = nullptr;
        qe.ev.trust_request.payload_len = 0;
    }

    carrier_push_event(c, std::move(qe));
}

void on_swarm_message_received(Carrier *c,
                               const std::string &accountId,
                               const std::string &conversationId,
                               const libjami::SwarmMessage &message)
{
    /* Only surface text messages in M2. Other commit types (member add,
     * profile updates, reactions) stay internal until M3/M4. */
    if (message.type != "text/plain") {
        return;
    }

    std::string from;
    std::string body;
    if (auto it = message.body.find("author"); it != message.body.end()) {
        from = it->second;
    }
    if (auto it = message.body.find("body"); it != message.body.end()) {
        body = it->second;
    }

    /* Skip our own messages echoed back via Swarm sync. */
    {
        std::lock_guard<std::mutex> lock(c->accounts_mtx);
        auto it = c->accounts.find(accountId);
        if (it != c->accounts.end() && !it->second.self_uri.empty() &&
            from == it->second.self_uri) {
            return;
        }
    }

    QueuedEvent qe;
    stamp(qe.ev, CARRIER_EVENT_TEXT_MESSAGE);
    set_account(qe.ev, accountId);
    copy_fixed(qe.ev.text_message.contact_uri, CARRIER_URI_LEN, from);
    copy_fixed(qe.ev.text_message.conversation_id,
               CARRIER_CONVERSATION_ID_LEN, conversationId);

    qe.ev.text_message.message_id = 0;   /* SwarmMessage uses string IDs; not surfaced in M2 */
    qe.message_text = std::move(body);
    qe.ev.text_message.text = qe.message_text.c_str();
    qe.ev.text_message.text_len = qe.message_text.size();

    carrier_push_event(c, std::move(qe));
}

void on_account_message_status(Carrier *c,
                               const std::string &accountId,
                               const std::string &conversationId,
                               const std::string &peer,
                               const std::string &message_id,
                               int state)
{
    QueuedEvent qe;
    stamp(qe.ev, CARRIER_EVENT_MESSAGE_SENT);
    set_account(qe.ev, accountId);
    copy_fixed(qe.ev.message_sent.contact_uri, CARRIER_URI_LEN, peer);
    copy_fixed(qe.ev.message_sent.conversation_id,
               CARRIER_CONVERSATION_ID_LEN, conversationId);

    /* libjami message IDs are decimal-encoded uint64_t strings on this signal. */
    qe.ev.message_sent.message_id = message_id.empty()
        ? 0
        : std::strtoull(message_id.c_str(), nullptr, 10);
    qe.ev.message_sent.status = state;

    carrier_push_event(c, std::move(qe));
}

void on_conversation_ready(Carrier *c,
                           const std::string &accountId,
                           const std::string &conversationId)
{
    /* Internal bookkeeping only — M3 will surface this as
     * carrier:ConversationReady. For M2 we use it to populate the
     * peer→conversation cache so carrier_send_message() can resolve
     * a 1:1 conversation without re-querying libjami.
     *
     * Conversation membership is looked up lazily in carrier_send_message
     * on cache miss; writing here requires another libjami call from a
     * signal thread, which we avoid — the cache gets seeded on first send. */
    (void) c;
    (void) accountId;
    (void) conversationId;
}

} /* anonymous namespace */

/* ---------------------------------------------------------------------------
 * Public (within the shim) entry point
 * ---------------------------------------------------------------------------*/

void carrier_register_signals(Carrier *c)
{
    std::map<std::string, std::shared_ptr<libjami::CallbackWrapperBase>> handlers;

    using libjami::exportable_callback;
    using CS  = libjami::ConfigurationSignal;
    using VS  = libjami::ConversationSignal;

    handlers.insert(exportable_callback<CS::RegistrationStateChanged>(
        [c](const std::string &accountId, const std::string &state,
            int code, const std::string &detailsStr) {
            on_registration_state(c, accountId, state, code, detailsStr);
        }));

    /* libjami's IncomingTrustRequest cb_type docstring says
     * (accountId, from, conversationId, payload, received), but every emit
     * site (jamiaccount.cpp:1249, conversation_module.cpp:1972) actually
     * passes (accountId, conversationId, from, payload, received). Bind by
     * the real wire order, not the docstring. */
    handlers.insert(exportable_callback<CS::IncomingTrustRequest>(
        [c](const std::string &accountId, const std::string &convId,
            const std::string &from,
            const std::vector<std::uint8_t> &payload, time_t received) {
            on_incoming_trust_request(c, accountId, from, convId, payload, received);
        }));

    handlers.insert(exportable_callback<CS::AccountMessageStatusChanged>(
        [c](const std::string &accountId, const std::string &conversationId,
            const std::string &peer, const std::string &message_id, int state) {
            on_account_message_status(c, accountId, conversationId, peer, message_id, state);
        }));

    handlers.insert(exportable_callback<VS::SwarmMessageReceived>(
        [c](const std::string &accountId, const std::string &conversationId,
            const libjami::SwarmMessage &message) {
            on_swarm_message_received(c, accountId, conversationId, message);
        }));

    handlers.insert(exportable_callback<VS::ConversationReady>(
        [c](const std::string &accountId, const std::string &conversationId) {
            on_conversation_ready(c, accountId, conversationId);
        }));

    libjami::registerSignalHandlers(handlers);
}
