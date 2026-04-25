/*  carrier_jami.cc
 *
 *  Jami-backed implementation of carrier.h. Companion to
 *  carrier_jami_signals.cc (signal handlers) and carrier_internal.hpp
 *  (state). See arch/jami-migration.md for the full migration plan.
 *
 *  This file is part of Carrier. Carrier is free software licensed
 *  under the MIT License.
 */

#include "carrier_internal.hpp"
#include "carrier_log.h"

#include <jami/jami.h>
#include <jami/configurationmanager_interface.h>
#include <jami/conversation_interface.h>
#include <jami/datatransfer_interface.h>
#include <jami/presencemanager_interface.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

/* ---------------------------------------------------------------------------
 * Utilities
 * ---------------------------------------------------------------------------*/

std::int64_t carrier_now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

namespace {

/* Non-blocking self-pipe. Used on macOS (no eventfd); works on Linux too. */
bool make_wakefd(int &read_fd, int &write_fd)
{
    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }
    for (int fd : fds) {
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int fd_fl = fcntl(fd, F_GETFD, 0);
        if (fd_fl >= 0) fcntl(fd, F_SETFD, fd_fl | FD_CLOEXEC);
    }
    read_fd = fds[0];
    write_fd = fds[1];
    return true;
}

/* Drain the read end of the self-pipe. Called at the top of carrier_iterate. */
void drain_wakefd(int fd)
{
    if (fd < 0) return;
    char buf[64];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
    }
}

/* Process-global singleton: libjami::init is process-scoped, so only one
 * Carrier* may exist at a time. Tracked here so we can refuse a second
 * carrier_new() while one is live. */
std::mutex g_carrier_mtx;
Carrier   *g_carrier = nullptr;

/* Copy a std::string into a caller-supplied fixed buffer, NUL-terminating. */
void copy_to(char *dst, std::size_t cap, const std::string &src)
{
    if (cap == 0) return;
    const std::size_t n = (src.size() < cap - 1) ? src.size() : cap - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

/* Given an accountId, return the AccountState entry (or nullptr if absent).
 * Caller must hold accounts_mtx for the lifetime of the returned pointer. */
AccountState *find_account(Carrier *c, const std::string &accountId)
{
    auto it = c->accounts.find(accountId);
    return it == c->accounts.end() ? nullptr : &it->second;
}

/* Resolve the 1:1 conversation ID for (accountId, peer_uri). Hits the cache
 * first; on miss, asks libjami for the contact's conversationId field, which
 * is set by addContact / sendTrustRequest. On further miss returns empty
 * (caller treats as NotTrusted). */
std::string resolve_one_to_one(Carrier *c,
                               const std::string &accountId,
                               const std::string &peer_uri)
{
    {
        std::lock_guard<std::mutex> lock(c->accounts_mtx);
        auto *acct = find_account(c, accountId);
        if (acct) {
            auto it = acct->peer_conversations.find(peer_uri);
            if (it != acct->peer_conversations.end()) {
                return it->second;
            }
        }
    }

    const auto details = libjami::getContactDetails(accountId, peer_uri);
    auto cv = details.find("conversationId");
    if (cv == details.end() || cv->second.empty()) {
        return {};
    }

    {
        std::lock_guard<std::mutex> lock(c->accounts_mtx);
        if (auto *acct = find_account(c, accountId)) {
            acct->peer_conversations[peer_uri] = cv->second;
        }
    }
    return cv->second;
}

void emit_error(Carrier *c,
                const std::string &accountId,
                const char *command,
                const char *klass,
                std::string text)
{
    QueuedEvent qe;
    qe.ev.type = CARRIER_EVENT_ERROR;
    qe.ev.timestamp = carrier_now_ms();
    copy_to(qe.ev.account_id, CARRIER_ACCOUNT_ID_LEN, accountId);
    copy_to(qe.ev.error.command, sizeof(qe.ev.error.command),
            command ? command : "");
    copy_to(qe.ev.error.class_, sizeof(qe.ev.error.class_),
            klass ? klass : "");
    qe.message_text = std::move(text);
    qe.ev.error.text = qe.message_text.c_str();
    carrier_push_event(c, std::move(qe));
}

} /* anonymous namespace */

/* ---------------------------------------------------------------------------
 * Shared queue helpers
 * ---------------------------------------------------------------------------*/

void carrier_push_event(Carrier *c, QueuedEvent &&qe)
{
    if (!c) return;
    {
        std::lock_guard<std::mutex> lock(c->queue_mtx);
        c->queue.emplace_back(std::move(qe));

        /* Re-anchor const-char pointers to the queued QueuedEvent's owned
         * std::strings. The pointers in `ev` were set by the caller against
         * the soon-to-be-moved-from local QueuedEvent; after the move,
         * std::string's data() differs, so we rebind to the queue copy.
         * std::deque preserves element addresses across further pushes, so
         * these pointers stay valid until carrier_iterate drains them. */
        QueuedEvent &q = c->queue.back();
        switch (q.ev.type) {
            case CARRIER_EVENT_ACCOUNT_ERROR:
                q.ev.account_error.cause = q.message_text.c_str();
                break;
            case CARRIER_EVENT_TRUST_REQUEST:
                if (q.ev.trust_request.payload_len > 0) {
                    q.ev.trust_request.payload = q.trust_payload.data();
                }
                break;
            case CARRIER_EVENT_TEXT_MESSAGE:
                q.ev.text_message.text = q.message_text.c_str();
                q.ev.text_message.text_len = q.message_text.size();
                break;
            case CARRIER_EVENT_GROUP_MESSAGE:
                q.ev.group_message.text = q.message_text.c_str();
                q.ev.group_message.text_len = q.message_text.size();
                break;
            case CARRIER_EVENT_ERROR:
                q.ev.error.text = q.message_text.c_str();
                break;
            case CARRIER_EVENT_SYSTEM:
                q.ev.system.text = q.message_text.c_str();
                break;
            default:
                break;
        }
    }
    if (c->wake_write_fd >= 0) {
        const char byte = 'x';
        /* Best-effort; EAGAIN is fine (a pending byte is already buffered). */
        (void) write(c->wake_write_fd, &byte, 1);
    }
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------*/

extern "C" Carrier *carrier_new(const char    *data_dir,
                                carrier_log_cb log_cb,
                                void          *log_userdata)
{
    {
        std::lock_guard<std::mutex> lock(g_carrier_mtx);
        if (g_carrier != nullptr) {
            /* libjami is process-scoped; only one Carrier* is allowed. */
            return nullptr;
        }
    }

    auto *c = new Carrier();
    c->log_cb = log_cb;
    c->log_userdata = log_userdata;
    c->log_level = CARRIER_LOG_ERROR;

    if (data_dir && *data_dir) {
        c->data_dir = data_dir;
        /* libjami's get_data_dir() is platform-specific (D11):
         *   Linux : reads XDG_DATA_HOME (falls back to $HOME/.local/share)
         *   macOS : hard-codes $HOME/Library/Application Support/jami
         *           (XDG_* is ignored). The home dir is cached in a static
         *           on first call, so HOME must be set before libjami::init.
         * Set both so each Carrier process gets an isolated data tree. */
        setenv("HOME", data_dir, 1);
        setenv("XDG_DATA_HOME", data_dir, 1);
    }

    if (!make_wakefd(c->wake_read_fd, c->wake_write_fd)) {
        CLOG_ERROR(c, "SHIM", "pipe() failed: %s", std::strerror(errno));
        delete c;
        return nullptr;
    }

    if (!libjami::init(static_cast<libjami::InitFlag>(
            libjami::LIBJAMI_FLAG_NO_LOCAL_AUDIO |
            libjami::LIBJAMI_FLAG_NO_AUTOLOAD))) {
        CLOG_ERROR(c, "JAMI", "libjami::init() failed");
        close(c->wake_read_fd);
        close(c->wake_write_fd);
        delete c;
        return nullptr;
    }
    c->libjami_initialized = true;

    /* Register signal handlers BEFORE start(), so we don't miss early
     * RegistrationStateChanged events from pre-loaded accounts. */
    carrier_register_signals(c);

    if (!libjami::start()) {
        CLOG_ERROR(c, "JAMI", "libjami::start() failed");
        libjami::unregisterSignalHandlers();
        libjami::fini();
        close(c->wake_read_fd);
        close(c->wake_write_fd);
        delete c;
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_carrier_mtx);
        g_carrier = c;
    }

    CLOG_INFO(c, "SHIM", "Carrier initialized (libjami %s)", libjami::version());
    return c;
}

extern "C" void carrier_free(Carrier *c)
{
    if (!c) return;

    if (c->libjami_initialized) {
        libjami::unregisterSignalHandlers();
        libjami::fini();
    }

    if (c->wake_read_fd  >= 0) close(c->wake_read_fd);
    if (c->wake_write_fd >= 0) close(c->wake_write_fd);

    {
        std::lock_guard<std::mutex> lock(g_carrier_mtx);
        if (g_carrier == c) g_carrier = nullptr;
    }

    delete c;
}

extern "C" int carrier_iterate(Carrier *c)
{
    if (!c) return -1;

    drain_wakefd(c->wake_read_fd);

    std::deque<QueuedEvent> drained;
    {
        std::lock_guard<std::mutex> lock(c->queue_mtx);
        drained.swap(c->queue);
    }

    for (auto &qe : drained) {
        if (c->event_cb) {
            c->event_cb(&qe.ev, c->event_userdata);
        }
    }

    return 0;
}

extern "C" int carrier_iteration_interval(Carrier * /*c*/)
{
    /* Conservative failsafe; callers using clock_fd wake much sooner. */
    return 5000;
}

extern "C" int carrier_clock_fd(Carrier *c)
{
    if (!c) return -1;
    return c->wake_read_fd;
}

/* ---------------------------------------------------------------------------
 * Event & log callback configuration
 * ---------------------------------------------------------------------------*/

extern "C" void carrier_set_event_callback(Carrier *c, carrier_event_cb cb, void *userdata)
{
    if (!c) return;
    c->event_cb = cb;
    c->event_userdata = userdata;
}

/* ---------------------------------------------------------------------------
 * Accounts
 * ---------------------------------------------------------------------------*/

extern "C" int carrier_create_account(Carrier    *c,
                                      const char *display_name,
                                      char        out_account_id[CARRIER_ACCOUNT_ID_LEN])
{
    if (!c || !out_account_id) return -1;

    std::map<std::string, std::string> details;
    details["Account.type"] = "RING";
    details["Account.archiveHasPassword"] = "false";
    details["Account.archivePassword"] = "";
    if (display_name && *display_name) {
        details["Account.alias"] = display_name;
        details["Account.displayName"] = display_name;
    }

    const std::string accountId = libjami::addAccount(details);
    if (accountId.empty()) {
        emit_error(c, "", "CreateAccount", "LibjamiFailure",
                   "addAccount returned empty id");
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(c->accounts_mtx);
        AccountState &st = c->accounts[accountId];
        if (display_name) st.display_name = display_name;
    }

    copy_to(out_account_id, CARRIER_ACCOUNT_ID_LEN, accountId);
    CLOG_INFO(c, "SHIM", "created account %s", accountId.c_str());
    return 0;
}

extern "C" int carrier_load_account(Carrier *c, const char *account_id)
{
    if (!c || !account_id) return -1;

    /* carrier_new() passes LIBJAMI_FLAG_NO_AUTOLOAD so libjami doesn't pick
     * up on-disk archives implicitly at init time — we want explicit
     * lifecycle. With that flag set, getAccountDetails() returns empty for
     * accounts that exist on disk but haven't been activated. Call
     * loadAccountAndConversation first to materialize the account (and its
     * conversations) before querying. */
    libjami::loadAccountAndConversation(account_id, /*loadAll=*/true, /*convId=*/"");

    const auto details = libjami::getAccountDetails(account_id);
    if (details.empty()) {
        CLOG_WARN(c, "SHIM", "load_account: no such account %s", account_id);
        return -1;
    }

    std::lock_guard<std::mutex> lock(c->accounts_mtx);
    AccountState &st = c->accounts[account_id];
    if (auto it = details.find("Account.username"); it != details.end()) {
        st.self_uri = it->second;
    }
    if (auto it = details.find("Account.displayName"); it != details.end()) {
        st.display_name = it->second;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Identity & presence
 * ---------------------------------------------------------------------------*/

extern "C" int carrier_get_id(Carrier *c, const char *account_id)
{
    if (!c || !account_id) return -1;

    std::string self_uri;
    {
        std::lock_guard<std::mutex> lock(c->accounts_mtx);
        auto *acct = find_account(c, account_id);
        if (!acct) return -1;
        self_uri = acct->self_uri;
    }

    if (self_uri.empty()) {
        /* Account loaded but not yet REGISTERED — fetch details directly. */
        const auto details = libjami::getAccountDetails(account_id);
        if (auto it = details.find("Account.username"); it != details.end()) {
            self_uri = it->second;
        }
    }

    QueuedEvent qe;
    qe.ev.type = CARRIER_EVENT_SELF_ID;
    qe.ev.timestamp = carrier_now_ms();
    copy_to(qe.ev.account_id, CARRIER_ACCOUNT_ID_LEN, account_id);
    copy_to(qe.ev.self_id.self_uri, CARRIER_URI_LEN, self_uri);
    carrier_push_event(c, std::move(qe));
    return 0;
}

extern "C" int carrier_set_nick(Carrier *c, const char *account_id, const char *nick)
{
    if (!c || !account_id) return -1;

    std::map<std::string, std::string> update;
    update["Account.displayName"] = nick ? nick : "";
    libjami::setAccountDetails(account_id, update);

    std::lock_guard<std::mutex> lock(c->accounts_mtx);
    if (auto *acct = find_account(c, account_id)) {
        acct->display_name = update["Account.displayName"];
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Trust
 * ---------------------------------------------------------------------------*/

extern "C" int carrier_send_trust_request(Carrier    *c,
                                          const char *account_id,
                                          const char *contact_uri,
                                          const char *message)
{
    if (!c || !account_id || !contact_uri) return -1;

    std::vector<std::uint8_t> payload;
    if (message && *message) {
        payload.assign(message, message + std::strlen(message));
    }
    libjami::sendTrustRequest(account_id, contact_uri, payload);
    return 0;
}

extern "C" int carrier_accept_trust_request(Carrier    *c,
                                            const char *account_id,
                                            const char *contact_uri)
{
    if (!c || !account_id || !contact_uri) return -1;
    return libjami::acceptTrustRequest(account_id, contact_uri) ? 0 : -1;
}

extern "C" int carrier_discard_trust_request(Carrier    *c,
                                             const char *account_id,
                                             const char *contact_uri)
{
    if (!c || !account_id || !contact_uri) return -1;
    return libjami::discardTrustRequest(account_id, contact_uri) ? 0 : -1;
}

extern "C" int carrier_remove_contact(Carrier    *c,
                                      const char *account_id,
                                      const char *contact_uri)
{
    if (!c || !account_id || !contact_uri) return -1;
    libjami::removeContact(account_id, contact_uri, /*ban=*/false);

    std::lock_guard<std::mutex> lock(c->accounts_mtx);
    if (auto *acct = find_account(c, account_id)) {
        acct->peer_conversations.erase(contact_uri);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Messaging
 * ---------------------------------------------------------------------------*/

extern "C" int carrier_send_message(Carrier    *c,
                                    const char *account_id,
                                    const char *contact_uri,
                                    const char *text)
{
    if (!c || !account_id || !contact_uri || !text) return -1;

    const std::string convId = resolve_one_to_one(c, account_id, contact_uri);
    if (convId.empty()) {
        emit_error(c, account_id, "SendMsg", "NotTrusted",
                   std::string("no 1:1 conversation with ") + contact_uri +
                   " (trust not established)");
        return -1;
    }

    libjami::sendMessage(account_id, convId, text, /*replyTo=*/"", /*flag=*/0);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Swarm conversations (multi-party)
 * ---------------------------------------------------------------------------*/

extern "C" int carrier_create_conversation(Carrier    *c,
                                           const char *account_id,
                                           const char *privacy,
                                           char        out_conversation_id[CARRIER_CONVERSATION_ID_LEN])
{
    if (!c || !account_id || !out_conversation_id) return -1;

    /* libjami's public API only exposes startConversation(accountId), which
     * defaults to ConversationMode::INVITES_ONLY. The internal
     * convModule->startConversation(mode, otherMember) is private. Until the
     * shim grows a path to the internal API, only "invites_only" (and NULL,
     * which is treated as default) are honored. Other modes log a warning
     * and fall through to invites_only. */
    const std::string mode = (privacy && *privacy) ? privacy : "invites_only";
    if (mode != "invites_only") {
        CLOG_WARN(c, "SHIM",
                  "create_conversation: privacy=%s not supported by public "
                  "libjami API; using invites_only", mode.c_str());
    }

    const std::string conv_id = libjami::startConversation(account_id);
    if (conv_id.empty()) {
        emit_error(c, account_id, "CreateConversation", "LibjamiFailure",
                   "startConversation returned empty id");
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(c->accounts_mtx);
        if (auto *acct = find_account(c, account_id)) {
            acct->conversation_modes[conv_id] = "invites_only";
        }
    }

    copy_to(out_conversation_id, CARRIER_CONVERSATION_ID_LEN, conv_id);
    return 0;
}

extern "C" int carrier_send_conversation_message(Carrier    *c,
                                                 const char *account_id,
                                                 const char *conversation_id,
                                                 const char *text)
{
    if (!c || !account_id || !conversation_id || !text) return -1;
    libjami::sendMessage(account_id, conversation_id, text,
                         /*replyTo=*/"", /*flag=*/0);
    return 0;
}

extern "C" int carrier_accept_conversation_request(Carrier    *c,
                                                   const char *account_id,
                                                   const char *conversation_id)
{
    if (!c || !account_id || !conversation_id) return -1;
    libjami::acceptConversationRequest(account_id, conversation_id);
    return 0;
}

extern "C" int carrier_decline_conversation_request(Carrier    *c,
                                                    const char *account_id,
                                                    const char *conversation_id)
{
    if (!c || !account_id || !conversation_id) return -1;
    libjami::declineConversationRequest(account_id, conversation_id);
    return 0;
}

extern "C" int carrier_invite_to_conversation(Carrier    *c,
                                              const char *account_id,
                                              const char *conversation_id,
                                              const char *contact_uri)
{
    if (!c || !account_id || !conversation_id || !contact_uri) return -1;
    libjami::addConversationMember(account_id, conversation_id, contact_uri);
    return 0;
}

extern "C" int carrier_remove_conversation(Carrier    *c,
                                           const char *account_id,
                                           const char *conversation_id)
{
    if (!c || !account_id || !conversation_id) return -1;
    const bool ok = libjami::removeConversation(account_id, conversation_id);

    {
        std::lock_guard<std::mutex> lock(c->accounts_mtx);
        if (auto *acct = find_account(c, account_id)) {
            acct->conversation_modes.erase(conversation_id);
            /* Drop any peer_conversations entry pointing at this id. */
            for (auto it = acct->peer_conversations.begin();
                 it != acct->peer_conversations.end(); ) {
                if (it->second == conversation_id) {
                    it = acct->peer_conversations.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    return ok ? 0 : -1;
}

/* ---------------------------------------------------------------------------
 * Reactions
 * ---------------------------------------------------------------------------*/

extern "C" int carrier_send_reaction(Carrier    *c,
                                     const char *account_id,
                                     const char *conversation_id,
                                     const char *message_id,
                                     const char *reaction)
{
    if (!c || !account_id || !conversation_id || !message_id || !reaction) {
        return -1;
    }
    /* libjami sendMessage(flag=2) routes to ConversationModule::reactToMessage,
     * which commits a `text/plain` message with body=reaction and a
     * `react-to: <message_id>` link. The peer side surfaces it through
     * the ReactionAdded signal (see on_reaction_added). */
    libjami::sendMessage(account_id, conversation_id, reaction,
                         /*replyTo=*/message_id, /*flag=*/2);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Presence
 * ---------------------------------------------------------------------------*/

extern "C" int carrier_subscribe_presence(Carrier    *c,
                                          const char *account_id,
                                          const char *contact_uri,
                                          bool        subscribe)
{
    if (!c || !account_id || !contact_uri) {
        return -1;
    }
    /* libjami::subscribeBuddy registers the buddy with the account's
     * PresenceManager. Status changes thereafter surface via
     * PresenceSignal::NewBuddyNotification (see on_presence_changed). The
     * call is idempotent on libjami's side — re-subscribing a tracked buddy
     * is a no-op aside from a fresh notification. */
    libjami::subscribeBuddy(account_id, contact_uri, subscribe);
    return 0;
}

/* ---------------------------------------------------------------------------
 * File transfer
 * ---------------------------------------------------------------------------*/

extern "C" int carrier_send_file(Carrier    *c,
                                 const char *account_id,
                                 const char *conversation_id,
                                 const char *path,
                                 const char *display_name)
{
    if (!c || !account_id || !conversation_id || !path) {
        return -1;
    }
    /* libjami::sendFile is void noexcept — it dispatches the sha3 hash and
     * commit to a computation thread, then a callback writes the file
     * symlink into conversation_data/. Path validation (regular file,
     * readable size) happens inside JamiAccount::sendFile and surfaces via
     * OnConversationError, which we don't currently bind — a missing file
     * fails silently aside from libjami's own log. Acceptable for M4a;
     * revisit if a consumer needs synchronous validation. */
    const std::string name = (display_name && *display_name) ? display_name : "";
    libjami::sendFile(account_id, conversation_id, path, name, /*replyTo=*/"");
    return 0;
}

extern "C" int carrier_accept_file(Carrier    *c,
                                   const char *account_id,
                                   const char *conversation_id,
                                   const char *message_id,
                                   const char *file_id,
                                   const char *path)
{
    if (!c || !account_id || !conversation_id || !message_id || !file_id || !path) {
        return -1;
    }
    /* libjami::downloadFile is bool noexcept; false signals invalid args
     * (unknown conversation/file or io failure preparing the destination).
     * The asynchronous transfer surfaces via DataTransferEvent thereafter. */
    const bool ok = libjami::downloadFile(account_id, conversation_id,
                                          message_id, file_id, path);
    return ok ? 0 : -1;
}

extern "C" int carrier_cancel_file(Carrier    *c,
                                   const char *account_id,
                                   const char *conversation_id,
                                   const char *file_id)
{
    if (!c || !account_id || !conversation_id || !file_id) {
        return -1;
    }
    const auto err = libjami::cancelDataTransfer(account_id, conversation_id, file_id);
    return (err == libjami::DataTransferError::success) ? 0 : -1;
}
