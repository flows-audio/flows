#include "AssistantHistory.hpp"

#include <algorithm>
#include <cerrno>
#include <map>
#include <set>
#if JUCE_WINDOWS
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace flows::app {
namespace {
using namespace juce;
constexpr std::size_t fileLimit = 8 * 1024 * 1024;
constexpr std::size_t turnReserve = 2 * 1024 * 1024 + 65536;
std::mutex historyMutex;
std::set<String> liveOwners;
std::map<String, std::set<String>> liveRequests;
[[noreturn]] void failHistory(const String& text) {
    throw std::runtime_error(text.toStdString());
}
int64 currentPid() {
#if JUCE_WINDOWS
    return static_cast<int64>(GetCurrentProcessId());
#else
    return static_cast<int64>(getpid());
#endif
}
bool ownerAlive(const String& owner, int64 pid, const String& requestId) {
    if (owner.isEmpty() || pid <= 0) return false;
    if (pid == currentPid())
        return liveOwners.contains(owner) && liveRequests[owner].contains(requestId);
#if JUCE_WINDOWS
    auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!handle) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD status = 0;
    const bool alive = GetExitCodeProcess(handle, &status) && status == STILL_ACTIVE;
    CloseHandle(handle);
    return alive;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}
String titleText(String text) {
    text = text.replaceCharacters("\r\n\t", "   ").trim();
    if (text.isEmpty() || text.length() > 160)
        failHistory("Conversation title must contain 1 to 160 characters.");
    return text;
}
}  // namespace
AssistantHistory::AssistantHistory() : ownerId_(juce::Uuid().toString()) {
    std::lock_guard lock(historyMutex);
    liveOwners.insert(ownerId_);
}
AssistantHistory::~AssistantHistory() {
    std::lock_guard lock(historyMutex);
    liveOwners.erase(ownerId_);
    liveRequests.erase(ownerId_);
}
void AssistantHistory::setWorkspace(const juce::File& workspace) {
    file_ = workspace.getChildFile(".assistant-history.json");
    try {
        transact([] {});
    } catch (const std::exception& exception) {
        error_ = exception.what();
    }
}
AssistantHistory::Conversation& AssistantHistory::find(const juce::String& id) {
    for (auto& conversation : conversations_)
        if (conversation.id == id) return conversation;
    failHistory("Conversation no longer exists. Refresh the history list.");
}
const AssistantHistory::Conversation& AssistantHistory::find(const juce::String& id) const {
    for (const auto& conversation : conversations_)
        if (conversation.id == id) return conversation;
    failHistory("Conversation no longer exists. Refresh the history list.");
}
void AssistantHistory::requireIdle(const Conversation& conversation) {
    for (const auto& turn : conversation.messages)
        if (turn.status == "working")
            failHistory(
                "Cancel the running request before archiving or deleting this conversation.");
}
bool AssistantHistory::read() {
    using namespace juce;
    conversations_.clear();
    // Selection belongs to each processor/editor. Reading another instance's
    // latest file merges its conversations without changing this view's focus.
    if (!file_.exists()) return false;
    if (!file_.existsAsFile() || file_.getSize() > static_cast<int64>(fileLimit))
        failHistory(
            "Assistant history cannot be read. Preserve .assistant-history.json and restore a "
            "valid backup.");
    var data;
    try {
        data = parseJSONBounded(file_.loadFileAsString(), fileLimit);
    } catch (...) {
        failHistory(
            "Assistant history is invalid. Preserve .assistant-history.json and restore a valid "
            "backup.");
    }
    auto list = get(data, "conversations");
    if (!data.isObject() || static_cast<int>(get(data, "version")) != 1 || !list.isArray() ||
        list.getArray()->size() > 200)
        failHistory(
            "Assistant history format is unsupported or over its limit. The original file was "
            "preserved.");
    std::set<String> ids, messageIds;
    std::size_t totalTurns = 0;
    bool recovered = false;
    for (const auto& entry : *list.getArray()) {
        const auto id = str(entry, "id"), title = str(entry, "title");
        const auto messages = get(entry, "messages");
        if (!entry.isObject() || id.isEmpty() || id.length() > 200 || title.isEmpty() ||
            title.length() > 160 || !messages.isArray() || messages.getArray()->size() > 100 ||
            !ids.insert(id).second)
            failHistory(
                "Assistant history contains invalid conversations. The original file was "
                "preserved.");
        Conversation conversation{id,
                                  title,
                                  static_cast<bool>(get(entry, "archived")),
                                  static_cast<bool>(get(entry, "deleted")),
                                  static_cast<int64>(get(entry, "updatedAt")),
                                  {}};
        for (const auto& item : *messages.getArray()) {
            Turn turn{str(item, "id"),
                      str(item, "prompt"),
                      str(item, "text"),
                      str(item, "status"),
                      str(item, "documentId"),
                      str(item, "proposedSource"),
                      str(item, "ownerId"),
                      static_cast<std::uint64_t>(static_cast<int64>(get(item, "baseRevision"))),
                      static_cast<int64>(get(item, "ownerPid"))};
            if (!item.isObject() || turn.id.isEmpty() || turn.id.length() > 200 ||
                !messageIds.insert(turn.id).second || turn.prompt.getNumBytesAsUTF8() > 16384 ||
                turn.text.getNumBytesAsUTF8() > 1024 * 1024 ||
                turn.proposal.getNumBytesAsUTF8() > 1024 * 1024 ||
                (turn.status != "working" && turn.status != "done" && turn.status != "failed" &&
                 turn.status != "cancelled" && turn.status != "interrupted"))
                failHistory(
                    "Assistant history contains invalid messages. The original file was "
                    "preserved.");
            if (turn.status == "working" && !ownerAlive(turn.owner, turn.ownerPid, turn.id)) {
                turn.status = "interrupted";
                turn.text =
                    "The app closed before this request finished. Send a new message to continue.";
                turn.owner.clear();
                turn.ownerPid = 0;
                conversation.updated = Time::currentTimeMillis();
                recovered = true;
            }
            conversation.messages.push_back(std::move(turn));
        }
        totalTurns += conversation.messages.size();
        conversations_.push_back(std::move(conversation));
    }
    if (totalTurns > 2000)
        failHistory("Assistant history exceeds 2000 saved turns. The original file was preserved.");
    if (activeId_.isEmpty()) activeId_ = str(data, "activeConversationId");
    if (activeId_.isNotEmpty() && !ids.contains(activeId_)) activeId_.clear();
    return recovered;
}
juce::var AssistantHistory::serialized() const {
    using namespace juce;
    Array<var> list;
    for (const auto& conversation : conversations_) {
        Array<var> messages;
        for (const auto& turn : conversation.messages) {
            auto item = object({{"id", turn.id},
                                {"prompt", turn.prompt},
                                {"text", turn.text},
                                {"status", turn.status},
                                {"baseRevision", static_cast<int64>(turn.revision)},
                                {"documentId", turn.documentId},
                                {"documentPath", turn.documentId}});
            if (turn.proposal.isNotEmpty())
                item.getDynamicObject()->setProperty("proposedSource", turn.proposal);
            if (turn.status == "working") {
                item.getDynamicObject()->setProperty("ownerId", turn.owner);
                item.getDynamicObject()->setProperty("ownerPid", turn.ownerPid);
            }
            messages.add(item);
        }
        list.add(object({{"id", conversation.id},
                         {"title", conversation.title},
                         {"archived", conversation.archived},
                         {"deleted", conversation.deleted},
                         {"updatedAt", conversation.updated},
                         {"messageCount", static_cast<int>(conversation.messages.size())},
                         {"messages", messages}}));
    }
    return object({{"version", 1}, {"activeConversationId", activeId_}, {"conversations", list}});
}
void AssistantHistory::save(bool reservePending) {
    using namespace juce;
    auto text = JSON::toString(serialized(), true);
    std::size_t required = text.getNumBytesAsUTF8();
    if (reservePending)
        for (const auto& conversation : conversations_)
            for (const auto& turn : conversation.messages)
                if (turn.status == "working") required += turnReserve;
    if (required > fileLimit)
        failHistory(
            "Assistant history is full (8 MiB). Permanently delete a conversation from Trash "
            "before sending again.");
    TemporaryFile temporary(file_, TemporaryFile::useHiddenFile);
    if (!temporary.getFile().replaceWithText(text, false, false, "\n") ||
        !temporary.overwriteTargetFileWithTemporary())
        failHistory(
            "Could not save assistant history. Check workspace permissions and available disk "
            "space.");
}
void AssistantHistory::transact(const std::function<void()>& operation) {
    std::lock_guard local(historyMutex);
    juce::InterProcessLock fileLock(
        "flows-assistant-" + juce::String::toHexString(file_.getFullPathName().hashCode64()));
    if (!fileLock.enter(3000))
        failHistory("Assistant history is busy in another app instance. Retry shortly.");
    struct Unlock {
        juce::InterProcessLock& lock;
        ~Unlock() { lock.exit(); }
    } unlock{fileLock};
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
juce::var AssistantHistory::view() {
    using namespace juce;
    // Read/merge without rewriting every UI refresh. Recovery is saved once.
    {
        std::lock_guard local(historyMutex);
        InterProcessLock fileLock("flows-assistant-" +
                                  String::toHexString(file_.getFullPathName().hashCode64()));
        if (!fileLock.enter(3000))
            failHistory("Assistant history is busy in another app instance. Retry shortly.");
        struct Unlock {
            InterProcessLock& lock;
            ~Unlock() { lock.exit(); }
        } unlock{fileLock};
        try {
            if (read()) save(true);
            error_.clear();
        } catch (const std::exception& exception) {
            conversations_.clear();
            error_ = exception.what();
        }
    }
    Array<var> summaries;
    var selected;
    auto all = get(serialized(), "conversations");
    for (const auto& entry : *all.getArray()) {
        auto summary = entry.clone();
        summary.getDynamicObject()->removeProperty("messages");
        summaries.add(summary);
        if (str(entry, "id") == activeId_) selected = entry;
    }
    std::sort(summaries.begin(), summaries.end(), [](const var& a, const var& b) {
        return static_cast<int64>(get(a, "updatedAt")) > static_cast<int64>(get(b, "updatedAt"));
    });
    auto result = object({{"conversations", summaries},
                          {"activeConversationId", activeId_},
                          {"conversation", selected}});
    if (error_.isNotEmpty()) result.getDynamicObject()->setProperty("historyError", error_);
    return result;
}
juce::String AssistantHistory::create(juce::String title) {
    juce::String id;
    transact([&] {
        if (conversations_.size() >= 200)
            failHistory(
                "Assistant history contains 200 conversations. Permanently delete a conversation "
                "from Trash first.");
        id = juce::Uuid().toString();
        conversations_.push_back(
            {id,
             title.isEmpty() ? juce::String("New conversation") : titleText(title),
             false,
             false,
             juce::Time::currentTimeMillis(),
             {}});
        activeId_ = id;
    });
    return id;
}
void AssistantHistory::select(const juce::String& id) {
    transact([&] {
        find(id);
        activeId_ = id;
    });
}
void AssistantHistory::rename(const juce::String& id, juce::String title) {
    transact([&] {
        auto& conversation = find(id);
        conversation.title = titleText(title);
        conversation.updated = juce::Time::currentTimeMillis();
    });
}
void AssistantHistory::archive(const juce::String& id, bool value) {
    transact([&] {
        auto& conversation = find(id);
        requireIdle(conversation);
        conversation.archived = value;
        conversation.updated = juce::Time::currentTimeMillis();
    });
}
void AssistantHistory::remove(const juce::String& id) {
    transact([&] {
        auto& conversation = find(id);
        requireIdle(conversation);
        conversation.deleted = true;
        conversation.updated = juce::Time::currentTimeMillis();
    });
}
void AssistantHistory::restore(const juce::String& id) {
    transact([&] {
        auto& conversation = find(id);
        conversation.deleted = conversation.archived = false;
        conversation.updated = juce::Time::currentTimeMillis();
    });
}
void AssistantHistory::purge(const juce::String& id) {
    transact([&] {
        auto& conversation = find(id);
        requireIdle(conversation);
        if (!conversation.deleted)
            failHistory("Move a conversation to Trash before deleting it permanently.");
        std::erase_if(conversations_, [&](const auto& item) { return item.id == id; });
        if (activeId_ == id) activeId_.clear();
    });
}
juce::String AssistantHistory::begin(const juce::String& requested, const juce::String& requestId,
                                     const juce::String& prompt, const juce::String& document,
                                     std::uint64_t revision) {
    if (requestId.isEmpty() || requestId.length() > 200 || prompt.isEmpty() ||
        prompt.getNumBytesAsUTF8() > 16384)
        failHistory("AI needs a unique request ID and a prompt of at most 16 KiB.");
    juce::String id;
    bool registeredThisBegin = false;
    try {
        transact([&] {
            id = requested.isNotEmpty() ? requested : activeId_;
            if (id.isEmpty()) {
                if (conversations_.size() >= 200)
                    failHistory(
                        "Assistant history contains 200 conversations. Permanently delete a "
                        "conversation from Trash first.");
                id = juce::Uuid().toString();
                conversations_.push_back(
                    {id, "New conversation", false, false, juce::Time::currentTimeMillis(), {}});
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
                        failHistory(
                            "This AI request ID already exists. Send with a new request ID.");
                }
            if (conversation.messages.size() >= 100 || count >= 2000)
                failHistory(
                    "Assistant history reached its saved-turn limit. Start a new conversation or "
                    "permanently delete one from Trash.");
            if (conversation.messages.empty() && conversation.title == "New conversation")
                conversation.title = titleText(prompt.substring(0, 72));
            conversation.messages.push_back(
                {requestId, prompt, {}, "working", document, {}, ownerId_, revision, currentPid()});
            conversation.updated = juce::Time::currentTimeMillis();
            activeId_ = id;
            registeredThisBegin = liveRequests[ownerId_].insert(requestId).second;
        });
    } catch (...) {
        if (registeredThisBegin) endRequest(requestId);
        throw;
    }
    return id;
}
void AssistantHistory::finish(const juce::String& id, const juce::String& requestId,
                              const juce::String& status, const juce::String& text,
                              const juce::String& proposal, const juce::String& targetDocument,
                              std::uint64_t targetRevision, bool retarget) {
    if (status != "done" && status != "failed" && status != "cancelled")
        failHistory("Invalid completed assistant status.");
    transact([&] {
        auto& conversation = find(id);
        for (auto& turn : conversation.messages)
            if (turn.id == requestId) {
                if (turn.status != "working" || turn.owner != ownerId_)
                    failHistory("This assistant request no longer belongs to the running worker.");
                if (text.getNumBytesAsUTF8() > 1024 * 1024 ||
                    proposal.getNumBytesAsUTF8() > 1024 * 1024)
                    failHistory("Assistant output exceeds its 1 MiB text/source history limit.");
                if (retarget) {
                    turn.documentId = targetDocument;
                    turn.revision = targetRevision;
                }
                turn.status = status;
                turn.text = text;
                turn.proposal = proposal;
                turn.owner.clear();
                turn.ownerPid = 0;
                conversation.updated = juce::Time::currentTimeMillis();
                return;
            }
        failHistory("Assistant request no longer exists in history.");
    });
    endRequest(requestId);
}
void AssistantHistory::endRequest(const juce::String& requestId) {
    std::lock_guard lock(historyMutex);
    liveRequests[ownerId_].erase(requestId);
}
bool AssistantHistory::proposalAvailable(const juce::String& conversationId,
                                         const juce::String& requestId,
                                         const juce::String& source) {
    (void)view();
    if (error_.isNotEmpty()) return false;
    for (const auto& conversation : conversations_)
        if (conversation.id == conversationId) {
            if (conversation.archived || conversation.deleted) return false;
            for (const auto& turn : conversation.messages)
                if (turn.id == requestId) return turn.status == "done" && turn.proposal == source;
        }
    return false;
}
juce::Array<juce::var> AssistantHistory::context(const juce::String& id) const {
    using namespace juce;
    Array<var> turns;
    std::size_t bytes = 0;
    int count = 0;
    const auto& conversation = find(id);
    for (auto it = conversation.messages.rbegin(); it != conversation.messages.rend(); ++it) {
        if (it->status != "done") continue;
        const auto user = it->prompt + "\n\n[Earlier instrument: " + it->documentId +
                          ", revision " + String(static_cast<int64>(it->revision)) +
                          ". Historical context only.]";
        const auto size = user.getNumBytesAsUTF8() + it->text.getNumBytesAsUTF8();
        if (count == 16 || bytes + size > 65536) break;
        turns.insert(0, object({{"role", "assistant"}, {"content", it->text}}));
        turns.insert(0, object({{"role", "user"}, {"content", user}}));
        bytes += size;
        ++count;
    }
    return turns;
}
}  // namespace flows::app
