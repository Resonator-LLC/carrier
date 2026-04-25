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
#include <jami/presencemanager_interface.h>

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

        /* Synthetic ConversationReady for each existing Swarm.
         *
         * libjami's `ConversationReady` signal fires only on initial
         * creation (carrier_create_conversation) or on first remote clone
         * (after accept_trust_request / accept_conversation_request). When
         * an account is loaded with conversations already on disk, libjami
         * stays silent — it has nothing to clone or create. Consumers that
         * gate on ConversationReady would deadlock on restart.
         *
         * Enumerate the conversations and emit synthetic events so the
         * caller's wait_for(ConversationReady) terminates uniformly across
         * fresh-create, accepted-clone, and reloaded-from-disk paths. */
        const auto convs = libjami::getConversations(accountId);
        for (const auto &conv_id : convs) {
            QueuedEvent cev;
            stamp(cev.ev, CARRIER_EVENT_CONVERSATION_READY);
            set_account(cev.ev, accountId);
            copy_fixed(cev.ev.conversation_ready.conversation_id,
                       CARRIER_CONVERSATION_ID_LEN, conv_id);
            carrier_push_event(c, std::move(cev));
        }
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

/* Translate libjami's "mode" metadata (int as string per ConversationMode
 * enum: 0 ONE_TO_ONE, 1 ADMIN_INVITES_ONLY, 2 INVITES_ONLY, 3 PUBLIC) into
 * the carrier:privacy vocabulary string. Empty input or unknown values fall
 * back to "invites_only" (libjami's documented default mode). */
std::string privacy_from_mode_str(const std::string &s)
{
    if (s == "0") return "one_to_one";
    if (s == "1") return "admin_invites";
    if (s == "2") return "invites_only";
    if (s == "3") return "public";
    return "invites_only";
}

/* Resolve the privacy mode for a conversation. Hits the per-account cache
 * first; on miss queries libjami::conversationInfos and seeds the cache.
 * Returns "invites_only" if libjami has nothing — caller treats as
 * not-1:1, which is the safe default for routing. */
std::string resolve_conversation_mode(Carrier *c,
                                      const std::string &accountId,
                                      const std::string &conversationId)
{
    {
        std::lock_guard<std::mutex> lock(c->accounts_mtx);
        auto it = c->accounts.find(accountId);
        if (it != c->accounts.end()) {
            auto cm = it->second.conversation_modes.find(conversationId);
            if (cm != it->second.conversation_modes.end()) {
                return cm->second;
            }
        }
    }

    const auto infos = libjami::conversationInfos(accountId, conversationId);
    auto it = infos.find("mode");
    const std::string mode = (it == infos.end())
        ? "invites_only"
        : privacy_from_mode_str(it->second);

    {
        std::lock_guard<std::mutex> lock(c->accounts_mtx);
        if (auto acct = c->accounts.find(accountId); acct != c->accounts.end()) {
            acct->second.conversation_modes[conversationId] = mode;
        }
    }
    return mode;
}

void on_swarm_message_received(Carrier *c,
                               const std::string &accountId,
                               const std::string &conversationId,
                               const libjami::SwarmMessage &message)
{
    /* Only surface text messages here. Other commit types (member events
     * fire ConversationMemberEvent separately; profile updates and
     * reactions stay internal until they have a typed event in M4). */
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

    /* Discriminate 1:1 vs multi-party. ONE_TO_ONE → TextMessage,
     * everything else → GroupMessage. */
    const std::string mode = resolve_conversation_mode(c, accountId, conversationId);
    const bool is_one_to_one = (mode == "one_to_one");

    QueuedEvent qe;
    if (is_one_to_one) {
        stamp(qe.ev, CARRIER_EVENT_TEXT_MESSAGE);
        set_account(qe.ev, accountId);
        copy_fixed(qe.ev.text_message.contact_uri, CARRIER_URI_LEN, from);
        copy_fixed(qe.ev.text_message.conversation_id,
                   CARRIER_CONVERSATION_ID_LEN, conversationId);
        copy_fixed(qe.ev.text_message.message_id,
                   CARRIER_MESSAGE_ID_LEN, message.id);
        qe.message_text = std::move(body);
        qe.ev.text_message.text = qe.message_text.c_str();
        qe.ev.text_message.text_len = qe.message_text.size();
    } else {
        stamp(qe.ev, CARRIER_EVENT_GROUP_MESSAGE);
        set_account(qe.ev, accountId);
        copy_fixed(qe.ev.group_message.conversation_id,
                   CARRIER_CONVERSATION_ID_LEN, conversationId);
        copy_fixed(qe.ev.group_message.message_id,
                   CARRIER_MESSAGE_ID_LEN, message.id);
        copy_fixed(qe.ev.group_message.contact_uri, CARRIER_URI_LEN, from);
        /* libjami's SwarmMessage body has no display_name; leave empty.
         * Consumers can resolve via ContactName events or
         * libjami::getConversationMembers if needed. */
        qe.ev.group_message.display_name[0] = '\0';
        qe.message_text = std::move(body);
        qe.ev.group_message.text = qe.message_text.c_str();
        qe.ev.group_message.text_len = qe.message_text.size();
    }

    carrier_push_event(c, std::move(qe));
}

/* ---------------------------------------------------------------------------
 * ReactionAdded handler
 *
 * libjami emits this when a peer's `react-to` commit lands on one of our
 * Swarms. The reaction map carries libjami's full commit body — we surface
 * the fields the carrier:Reaction vocab calls for: messageId (the reacted-to
 * commit), reaction body (the emoji), and author. The reaction's own commit
 * id rides along as `reaction_id` for callers that want to address the
 * reaction itself (e.g. to mirror a remove/edit later).
 * ---------------------------------------------------------------------------*/

void on_reaction_added(Carrier *c,
                       const std::string &accountId,
                       const std::string &conversationId,
                       const std::string &messageId,
                       const std::map<std::string, std::string> &reaction)
{
    auto get = [&](const char *key) -> std::string {
        auto it = reaction.find(key);
        return it == reaction.end() ? std::string{} : it->second;
    };

    const std::string author = get("author");
    const std::string body   = get("body");
    const std::string rid    = get("id");

    /* Skip our own reactions echoed back via Swarm sync. Mirrors the
     * own-message filter in on_swarm_message_received. */
    {
        std::lock_guard<std::mutex> lock(c->accounts_mtx);
        auto it = c->accounts.find(accountId);
        if (it != c->accounts.end() && !it->second.self_uri.empty() &&
            author == it->second.self_uri) {
            return;
        }
    }

    QueuedEvent qe;
    stamp(qe.ev, CARRIER_EVENT_REACTION);
    set_account(qe.ev, accountId);
    copy_fixed(qe.ev.reaction.conversation_id,
               CARRIER_CONVERSATION_ID_LEN, conversationId);
    copy_fixed(qe.ev.reaction.message_id, CARRIER_MESSAGE_ID_LEN, messageId);
    copy_fixed(qe.ev.reaction.reaction_id, CARRIER_MESSAGE_ID_LEN, rid);
    copy_fixed(qe.ev.reaction.contact_uri, CARRIER_URI_LEN, author);
    copy_fixed(qe.ev.reaction.text, CARRIER_REACTION_LEN, body);
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
    copy_fixed(qe.ev.message_sent.message_id, CARRIER_MESSAGE_ID_LEN, message_id);
    qe.ev.message_sent.status = state;

    carrier_push_event(c, std::move(qe));
}

void on_conversation_ready(Carrier *c,
                           const std::string &accountId,
                           const std::string &conversationId)
{
    QueuedEvent qe;
    stamp(qe.ev, CARRIER_EVENT_CONVERSATION_READY);
    set_account(qe.ev, accountId);
    copy_fixed(qe.ev.conversation_ready.conversation_id,
               CARRIER_CONVERSATION_ID_LEN, conversationId);
    carrier_push_event(c, std::move(qe));
}

void on_conversation_request_received(Carrier *c,
                                      const std::string &accountId,
                                      const std::string &conversationId,
                                      const std::map<std::string, std::string> &metadatas)
{
    /* The metadata map carries `from` (inviter URI), `id` (==conversationId),
     * `received` (unix ts), plus any vCard metadata. Per
     * conversation_module.cpp:1957-1959, `from` is set by ConversationRequest::toMap.
     * The 1:1 case is dispatched separately as a trust request, so
     * surfacing this for multi-party invitations is the primary use case. */
    std::string inviter;
    if (auto it = metadatas.find("from"); it != metadatas.end()) {
        inviter = it->second;
    }

    QueuedEvent qe;
    stamp(qe.ev, CARRIER_EVENT_CONVERSATION_REQUEST);
    set_account(qe.ev, accountId);
    copy_fixed(qe.ev.conversation_request.conversation_id,
               CARRIER_CONVERSATION_ID_LEN, conversationId);
    copy_fixed(qe.ev.conversation_request.contact_uri,
               CARRIER_URI_LEN, inviter);
    carrier_push_event(c, std::move(qe));
}

void on_conversation_member_event(Carrier *c,
                                  const std::string &accountId,
                                  const std::string &conversationId,
                                  const std::string &memberUri,
                                  int action)
{
    /* libjami action codes (conversation.cpp:413-432):
     *   0 add, 1 joins, 2 leave, 3 ban, 4 unban
     * The "add" notification fires before the member has actually joined
     * (admin invitation enqueued); we surface only on actual join/exit
     * transitions. Banned counts as exit; unban as join. */
    CarrierEventType ev_type;
    switch (action) {
        case 1: ev_type = CARRIER_EVENT_GROUP_PEER_JOIN; break;
        case 4: ev_type = CARRIER_EVENT_GROUP_PEER_JOIN; break;
        case 2: ev_type = CARRIER_EVENT_GROUP_PEER_EXIT; break;
        case 3: ev_type = CARRIER_EVENT_GROUP_PEER_EXIT; break;
        default: return;   /* 0 (add): not yet present, skip */
    }

    QueuedEvent qe;
    stamp(qe.ev, ev_type);
    set_account(qe.ev, accountId);
    if (ev_type == CARRIER_EVENT_GROUP_PEER_JOIN) {
        copy_fixed(qe.ev.group_peer_join.conversation_id,
                   CARRIER_CONVERSATION_ID_LEN, conversationId);
        copy_fixed(qe.ev.group_peer_join.member_uri,
                   CARRIER_URI_LEN, memberUri);
    } else {
        copy_fixed(qe.ev.group_peer_exit.conversation_id,
                   CARRIER_CONVERSATION_ID_LEN, conversationId);
        copy_fixed(qe.ev.group_peer_exit.member_uri,
                   CARRIER_URI_LEN, memberUri);
    }
    carrier_push_event(c, std::move(qe));
}

void on_conversation_sync_finished(Carrier *c,
                                   const std::string &accountId)
{
    /* libjami's signal is account-scoped (no conversationId), so the
     * emitted event carries no conversationId either. */
    QueuedEvent qe;
    stamp(qe.ev, CARRIER_EVENT_CONVERSATION_SYNC_FINISHED);
    set_account(qe.ev, accountId);
    qe.ev.conversation_sync_finished._placeholder = '\0';
    carrier_push_event(c, std::move(qe));
}

void on_conversation_removed(Carrier *c,
                             const std::string &accountId,
                             const std::string &conversationId)
{
    /* No event surfaces yet (vocabulary doesn't define one). Drop the
     * mode + peer caches so a fresh conversation by the same id reseeds
     * cleanly. */
    std::lock_guard<std::mutex> lock(c->accounts_mtx);
    auto it = c->accounts.find(accountId);
    if (it == c->accounts.end()) return;
    it->second.conversation_modes.erase(conversationId);
    for (auto pc = it->second.peer_conversations.begin();
         pc != it->second.peer_conversations.end(); ) {
        if (pc->second == conversationId) {
            pc = it->second.peer_conversations.erase(pc);
        } else {
            ++pc;
        }
    }
}

void on_presence_changed(Carrier *c,
                         const std::string &accountId,
                         const std::string &buddyUri,
                         int status,
                         const std::string &/*line_status*/)
{
    /* libjami's PresenceState (jamidht/jamiaccount.h) is
     *   DISCONNECTED = 0, AVAILABLE = 1, CONNECTED = 2.
     * Anything > 0 is reachable; only DISCONNECTED is offline. We expose
     * a binary "online"/"offline" today; "away"/"busy" are reserved in the
     * vocab for when libjami starts surfacing line_status worth mapping. */
    QueuedEvent qe;
    stamp(qe.ev, CARRIER_EVENT_PRESENCE);
    set_account(qe.ev, accountId);
    copy_fixed(qe.ev.presence.contact_uri, CARRIER_URI_LEN, buddyUri);
    std::string state = (status > 0) ? "online" : "offline";
    copy_fixed(qe.ev.presence.status, CARRIER_STATUS_LEN, state);
    carrier_push_event(c, std::move(qe));
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
    using PS  = libjami::PresenceSignal;

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

    handlers.insert(exportable_callback<VS::ConversationRequestReceived>(
        [c](const std::string &accountId, const std::string &conversationId,
            std::map<std::string, std::string> metadatas) {
            on_conversation_request_received(c, accountId, conversationId, metadatas);
        }));

    handlers.insert(exportable_callback<VS::ConversationMemberEvent>(
        [c](const std::string &accountId, const std::string &conversationId,
            const std::string &memberUri, int action) {
            on_conversation_member_event(c, accountId, conversationId, memberUri, action);
        }));

    handlers.insert(exportable_callback<VS::ConversationSyncFinished>(
        [c](const std::string &accountId) {
            on_conversation_sync_finished(c, accountId);
        }));

    handlers.insert(exportable_callback<VS::ConversationRemoved>(
        [c](const std::string &accountId, const std::string &conversationId) {
            on_conversation_removed(c, accountId, conversationId);
        }));

    handlers.insert(exportable_callback<VS::ReactionAdded>(
        [c](const std::string &accountId, const std::string &conversationId,
            const std::string &messageId,
            std::map<std::string, std::string> reaction) {
            on_reaction_added(c, accountId, conversationId, messageId, reaction);
        }));

    handlers.insert(exportable_callback<PS::NewBuddyNotification>(
        [c](const std::string &accountId, const std::string &buddyUri,
            int status, const std::string &lineStatus) {
            on_presence_changed(c, accountId, buddyUri, status, lineStatus);
        }));

    libjami::registerSignalHandlers(handlers);
}
