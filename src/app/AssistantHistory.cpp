#include "AssistantHistory.hpp"

#include "Files.hpp"
#include "Text.hpp"

#include <algorithm>
#include <cerrno>
#include <map>
#include <set>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace flows::app {
namespace {
constexpr std::size_t fileLimit = 8 * 1024 * 1024;
constexpr std::size_t turnReserve = 2 * 1024 * 1024 + 65536;
std::mutex historyMutex;
std::set<std::string> liveOwners;
std::map<std::string, std::set<std::string>> liveRequests;
[[noreturn]] void failHistory(const std::string& message) { throw std::runtime_error(message); }
std::int64_t currentPid() {
#if defined(_WIN32)
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}
bool ownerAlive(const std::string& owner, std::int64_t pid, const std::string& requestId) {
    if (owner.empty() || pid <= 0) return false;
    if (pid == currentPid()) return liveOwners.contains(owner) && liveRequests[owner].contains(requestId);
#if defined(_WIN32)
    auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!handle) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD status = 0;
    const bool alive = GetExitCodeProcess(handle, &status) && status == STILL_ACTIVE;
    CloseHandle(handle);
    return alive;
#elif defined(__EMSCRIPTEN__)
    return false;  // A browser tab has no other processes.
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}
std::string titleText(std::string value) {
    for (auto& c : value)
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    value = text::trim(value);
    if (value.empty() || text::codePointCount(value) > 160)
        failHistory("Conversation title must contain 1 to 160 characters.");
    return value;
}
}  // namespace

AssistantHistory::AssistantHistory() : ownerId_(text::randomId()) {
    std::lock_guard lock(historyMutex);
    liveOwners.insert(ownerId_);
}
AssistantHistory::~AssistantHistory() {
    std::lock_guard lock(historyMutex);
    liveOwners.erase(ownerId_);
    liveRequests.erase(ownerId_);
}
std::string AssistantHistory::lockName() const {
    // FNV keeps the lock name identical across processes and builds.
    return "flows-assistant-" + text::fnv1aDigest(file_.string()).substr(8);
}
void AssistantHistory::setWorkspace(const std::filesystem::path& workspace) {
    file_ = workspace / ".assistant-history.json";
    try {
        transact([] {});
    } catch (const std::exception& exception) {
        error_ = exception.what();
    }
}
AssistantHistory::Conversation& AssistantHistory::find(const std::string& id) {
    for (auto& conversation : conversations_)
        if (conversation.id == id) return conversation;
    failHistory("Conversation no longer exists. Refresh the history list.");
}
const AssistantHistory::Conversation& AssistantHistory::find(const std::string& id) const {
    for (const auto& conversation : conversations_)
        if (conversation.id == id) return conversation;
    failHistory("Conversation no longer exists. Refresh the history list.");
}
void AssistantHistory::requireIdle(const Conversation& conversation) {
    for (const auto& turn : conversation.messages)
        if (turn.status == "working")
            failHistory("Cancel the running request before archiving or deleting this conversation.");
}
bool AssistantHistory::read() {
    conversations_.clear();
    // Selection belongs to each processor/editor. Reading another instance's
    // latest file merges its conversations without changing this view's focus.
    if (!files::exists(file_)) return false;
    if (!files::isFile(file_) || files::size(file_) > static_cast<std::int64_t>(fileLimit))
        failHistory("Assistant history cannot be read. Preserve .assistant-history.json and restore a valid backup.");
    json::Value data;
    try {
        data = json::parseBounded(files::read(file_, fileLimit), fileLimit);
    } catch (...) {
        failHistory("Assistant history is invalid. Preserve .assistant-history.json and restore a valid backup.");
    }
    const auto& list = data["conversations"];
    if (!data.isObject() || data["version"].asInt() != 1 || !list.isArray() || list.size() > 200)
        failHistory("Assistant history format is unsupported or over its limit. The original file was preserved.");
    std::set<std::string> ids, messageIds;
    std::size_t totalTurns = 0;
    bool recovered = false;
    for (const auto& entry : *list.getArray()) {
        const auto id = json::str(entry, "id"), title = json::str(entry, "title");
        const auto& messages = entry["messages"];
        if (!entry.isObject() || id.empty() || text::codePointCount(id) > 200 || title.empty() ||
            text::codePointCount(title) > 160 || !messages.isArray() || messages.size() > 100 ||
            !ids.insert(id).second)
            failHistory("Assistant history contains invalid conversations. The original file was preserved.");
        Conversation conversation{id, title, entry["archived"].asBool(), entry["deleted"].asBool(),
                                  entry["updatedAt"].asInt(), {}};
        for (const auto& item : *messages.getArray()) {
            Turn turn{json::str(item, "id"),
                      json::str(item, "prompt"),
                      json::str(item, "text"),
                      json::str(item, "status"),
                      json::str(item, "documentId"),
                      json::str(item, "proposedSource"),
                      json::str(item, "ownerId"),
                      static_cast<std::uint64_t>(item["baseRevision"].asInt()),
                      item["ownerPid"].asInt()};
            if (!item.isObject() || turn.id.empty() || text::codePointCount(turn.id) > 200 ||
                !messageIds.insert(turn.id).second || turn.prompt.size() > 16384 || turn.text.size() > 1024 * 1024 ||
                turn.proposal.size() > 1024 * 1024 ||
                (turn.status != "working" && turn.status != "done" && turn.status != "failed" &&
                 turn.status != "cancelled" && turn.status != "interrupted"))
                failHistory("Assistant history contains invalid messages. The original file was preserved.");
            if (turn.status == "working" && !ownerAlive(turn.owner, turn.ownerPid, turn.id)) {
                turn.status = "interrupted";
                turn.text = "The app closed before this request finished. Send a new message to continue.";
                turn.owner.clear();
                turn.ownerPid = 0;
                conversation.updated = text::currentTimeMillis();
                recovered = true;
            }
            conversation.messages.push_back(std::move(turn));
        }
        totalTurns += conversation.messages.size();
        conversations_.push_back(std::move(conversation));
    }
    if (totalTurns > 2000) failHistory("Assistant history exceeds 2000 saved turns. The original file was preserved.");
    if (activeId_.empty()) activeId_ = json::str(data, "activeConversationId");
    if (!activeId_.empty() && !ids.contains(activeId_)) activeId_.clear();
    return recovered;
}
json::Value AssistantHistory::serialized() const {
    json::Array list;
    for (const auto& conversation : conversations_) {
        json::Array messages;
        for (const auto& turn : conversation.messages) {
            auto item = json::object({{"id", turn.id},
                                      {"prompt", turn.prompt},
                                      {"text", turn.text},
                                      {"status", turn.status},
                                      {"baseRevision", static_cast<std::int64_t>(turn.revision)},
                                      {"documentId", turn.documentId},
                                      {"documentPath", turn.documentId}});
            if (!turn.proposal.empty()) item.set("proposedSource", turn.proposal);
            if (turn.status == "working") {
                item.set("ownerId", turn.owner);
                item.set("ownerPid", turn.ownerPid);
            }
            messages.push_back(std::move(item));
        }
        list.push_back(json::object({{"id", conversation.id},
                                     {"title", conversation.title},
                                     {"archived", conversation.archived},
                                     {"deleted", conversation.deleted},
                                     {"updatedAt", conversation.updated},
                                     {"messageCount", static_cast<int>(conversation.messages.size())},
                                     {"messages", std::move(messages)}}));
    }
    return json::object({{"version", 1}, {"activeConversationId", activeId_}, {"conversations", std::move(list)}});
}
void AssistantHistory::save(bool reservePending) {
    const auto content = json::stringifyPretty(serialized()) + "\n";
    std::size_t required = content.size();
    if (reservePending)
        for (const auto& conversation : conversations_)
            for (const auto& turn : conversation.messages)
                if (turn.status == "working") required += turnReserve;
    if (required > fileLimit)
        failHistory("Assistant history is full (8 MiB). Permanently delete a conversation from Trash before sending again.");
    try {
        files::writeAtomic(file_, content);
    } catch (const std::exception&) {
        failHistory("Could not save assistant history. Check workspace permissions and available disk space.");
    }
}
void AssistantHistory::transact(const std::function<void()>& operation) {
    std::lock_guard local(historyMutex);
    files::ProcessLock fileLock(lockName());
    if (!fileLock.enter(3000)) failHistory("Assistant history is busy in another app instance. Retry shortly.");
    const auto oldSelection = activeId_;
    try {
        read();
        operation();
        save(true);
        error_.clear();
    } catch (const std::exception& exception) {
        activeId_ = oldSelection;
        error_ = exception.what();
        try {
            read();
        } catch (...) {
            conversations_.clear();
        }
        throw;
    }
}
json::Value AssistantHistory::view() {
    // Read/merge without rewriting every UI refresh. Recovery is saved once.
    {
        std::lock_guard local(historyMutex);
        files::ProcessLock fileLock(lockName());
        if (!fileLock.enter(3000)) failHistory("Assistant history is busy in another app instance. Retry shortly.");
        try {
            if (read()) save(true);
            error_.clear();
        } catch (const std::exception& exception) {
            conversations_.clear();
            error_ = exception.what();
        }
    }
    json::Array summaries;
    json::Value selected;
    const auto all = serialized();
    for (const auto& entry : *all["conversations"].getArray()) {
        auto summary = entry;
        summary.getObject()->remove("messages");
        summaries.push_back(std::move(summary));
        if (json::str(entry, "id") == activeId_) selected = entry;
    }
    std::stable_sort(summaries.begin(), summaries.end(), [](const json::Value& a, const json::Value& b) {
        return a["updatedAt"].asInt() > b["updatedAt"].asInt();
    });
    auto result = json::object({{"conversations", std::move(summaries)},
                                {"activeConversationId", activeId_},
                                {"conversation", std::move(selected)}});
    if (!error_.empty()) result.set("historyError", error_);
    return result;
}
std::string AssistantHistory::create(std::string title) {
    std::string id;
    transact([&] {
        if (conversations_.size() >= 200)
            failHistory("Assistant history contains 200 conversations. Permanently delete a conversation from Trash first.");
        id = text::randomId();
        conversations_.push_back(
            {id, title.empty() ? std::string("New conversation") : titleText(title), false, false, text::currentTimeMillis(), {}});
        activeId_ = id;
    });
    return id;
}
void AssistantHistory::select(const std::string& id) {
    transact([&] {
        find(id);
        activeId_ = id;
    });
}
void AssistantHistory::rename(const std::string& id, std::string title) {
    transact([&] {
        auto& conversation = find(id);
        conversation.title = titleText(title);
        conversation.updated = text::currentTimeMillis();
    });
}
void AssistantHistory::archive(const std::string& id, bool value) {
    transact([&] {
        auto& conversation = find(id);
        requireIdle(conversation);
        conversation.archived = value;
        conversation.updated = text::currentTimeMillis();
    });
}
void AssistantHistory::remove(const std::string& id) {
    transact([&] {
        auto& conversation = find(id);
        requireIdle(conversation);
        conversation.deleted = true;
        conversation.updated = text::currentTimeMillis();
    });
}
void AssistantHistory::restore(const std::string& id) {
    transact([&] {
        auto& conversation = find(id);
        conversation.deleted = conversation.archived = false;
        conversation.updated = text::currentTimeMillis();
    });
}
void AssistantHistory::purge(const std::string& id) {
    transact([&] {
        auto& conversation = find(id);
        requireIdle(conversation);
        if (!conversation.deleted) failHistory("Move a conversation to Trash before deleting it permanently.");
        std::erase_if(conversations_, [&](const auto& item) { return item.id == id; });
        if (activeId_ == id) activeId_.clear();
    });
}
std::string AssistantHistory::begin(const std::string& requested, const std::string& requestId,
                                    const std::string& prompt, const std::string& document, std::uint64_t revision) {
    if (requestId.empty() || text::codePointCount(requestId) > 200 || prompt.empty() || prompt.size() > 16384)
        failHistory("AI needs a unique request ID and a prompt of at most 16 KiB.");
    std::string id;
    bool registeredThisBegin = false;
    try {
        transact([&] {
            id = !requested.empty() ? requested : activeId_;
            if (id.empty()) {
                if (conversations_.size() >= 200)
                    failHistory("Assistant history contains 200 conversations. Permanently delete a conversation from Trash first.");
                id = text::randomId();
                conversations_.push_back({id, "New conversation", false, false, text::currentTimeMillis(), {}});
            }
            auto& conversation = find(id);
            requireIdle(conversation);
            if (conversation.archived || conversation.deleted)
                failHistory("Restore this conversation before sending a message.");
            std::size_t count = 0;
            for (const auto& item : conversations_)
                for (const auto& turn : item.messages) {
                    ++count;
                    if (turn.id == requestId)
                        failHistory("This AI request ID already exists. Send with a new request ID.");
                }
            if (conversation.messages.size() >= 100 || count >= 2000)
                failHistory("Assistant history reached its saved-turn limit. Start a new conversation or permanently delete one from Trash.");
            if (conversation.messages.empty() && conversation.title == "New conversation")
                conversation.title = titleText(text::codePointSubstring(prompt, 0, 72));
            conversation.messages.push_back(
                {requestId, prompt, {}, "working", document, {}, ownerId_, revision, currentPid()});
            conversation.updated = text::currentTimeMillis();
            activeId_ = id;
            registeredThisBegin = liveRequests[ownerId_].insert(requestId).second;
        });
    } catch (...) {
        if (registeredThisBegin) endRequest(requestId);
        throw;
    }
    return id;
}
void AssistantHistory::finish(const std::string& id, const std::string& requestId, const std::string& status,
                              const std::string& message, const std::string& proposal, const std::string& targetDocument,
                              std::uint64_t targetRevision, bool retarget) {
    if (status != "done" && status != "failed" && status != "cancelled") failHistory("Invalid completed assistant status.");
    transact([&] {
        auto& conversation = find(id);
        for (auto& turn : conversation.messages)
            if (turn.id == requestId) {
                if (turn.status != "working" || turn.owner != ownerId_)
                    failHistory("This assistant request no longer belongs to the running worker.");
                if (message.size() > 1024 * 1024 || proposal.size() > 1024 * 1024)
                    failHistory("Assistant output exceeds its 1 MiB text/source history limit.");
                if (retarget) {
                    turn.documentId = targetDocument;
                    turn.revision = targetRevision;
                }
                turn.status = status;
                turn.text = message;
                turn.proposal = proposal;
                turn.owner.clear();
                turn.ownerPid = 0;
                conversation.updated = text::currentTimeMillis();
                return;
            }
        failHistory("Assistant request no longer exists in history.");
    });
    endRequest(requestId);
}
void AssistantHistory::endRequest(const std::string& requestId) {
    std::lock_guard lock(historyMutex);
    liveRequests[ownerId_].erase(requestId);
}
bool AssistantHistory::proposalAvailable(const std::string& conversationId, const std::string& requestId,
                                         const std::string& source) {
    (void)view();
    if (!error_.empty()) return false;
    for (const auto& conversation : conversations_)
        if (conversation.id == conversationId) {
            if (conversation.archived || conversation.deleted) return false;
            for (const auto& turn : conversation.messages)
                if (turn.id == requestId) return turn.status == "done" && turn.proposal == source;
        }
    return false;
}
json::Array AssistantHistory::context(const std::string& id) const {
    json::Array turns;
    std::size_t bytes = 0;
    int count = 0;
    const auto& conversation = find(id);
    for (auto it = conversation.messages.rbegin(); it != conversation.messages.rend(); ++it) {
        if (it->status != "done") continue;
        const auto user = it->prompt + "\n\n[Earlier instrument: " + it->documentId + ", revision " +
                          std::to_string(static_cast<std::int64_t>(it->revision)) + ". Historical context only.]";
        const auto size = user.size() + it->text.size();
        if (count == 16 || bytes + size > 65536) break;
        turns.insert(turns.begin(), json::object({{"role", "assistant"}, {"content", it->text}}));
        turns.insert(turns.begin(), json::object({{"role", "user"}, {"content", user}}));
        bytes += size;
        ++count;
    }
    return turns;
}
}  // namespace flows::app
