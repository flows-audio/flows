#pragma once
#include "Json.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace flows::app {
// Workspace-local chat data. Instrument packages and DAW state never include it.
class AssistantHistory {
public:
    AssistantHistory();
    ~AssistantHistory();
    void setWorkspace(const std::filesystem::path&);
    std::string activeId() const { return activeId_; }
    std::string error() const { return error_; }
    json::Value view();
    std::string create(std::string title = {});
    void select(const std::string&);
    void rename(const std::string&, std::string title);
    void archive(const std::string&, bool value = true);
    void remove(const std::string&);
    void restore(const std::string&);
    void purge(const std::string&);
    std::string begin(const std::string& conversationId, const std::string& requestId, const std::string& prompt,
                      const std::string& documentId, std::uint64_t revision);
    void finish(const std::string& conversationId, const std::string& requestId, const std::string& status,
                const std::string& text, const std::string& proposal = {}, const std::string& targetDocument = {},
                std::uint64_t targetRevision = 0, bool retarget = false);
    json::Array context(const std::string&) const;
    void endRequest(const std::string& requestId);
    bool proposalAvailable(const std::string& conversationId, const std::string& requestId,
                           const std::string& source);

private:
    struct Turn {
        std::string id, prompt, text, status, documentId, proposal, owner;
        std::uint64_t revision = 0;
        std::int64_t ownerPid = 0;
    };
    struct Conversation {
        std::string id, title;
        bool archived = false, deleted = false;
        std::int64_t updated = 0;
        std::vector<Turn> messages;
    };
    std::filesystem::path file_;
    std::string activeId_, ownerId_, error_;
    std::vector<Conversation> conversations_;
    std::string lockName() const;
    void transact(const std::function<void()>&);
    bool read();
    json::Value serialized() const;
    void save(bool reservePending = false);
    Conversation& find(const std::string&);
    const Conversation& find(const std::string&) const;
    static void requireIdle(const Conversation&);
};
}  // namespace flows::app
