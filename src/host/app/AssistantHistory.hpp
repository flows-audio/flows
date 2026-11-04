#pragma once
#include <functional>
#include <mutex>

#include "Json.hpp"

namespace flows::app {
// Workspace-local chat data. Instrument packages and DAW state never include it.
class AssistantHistory {
public:
    AssistantHistory();
    ~AssistantHistory();
    void setWorkspace(const juce::File&);
    juce::String activeId() const { return activeId_; }
    juce::String error() const { return error_; }
    juce::var view();
    juce::String create(juce::String title = {});
    void select(const juce::String&);
    void rename(const juce::String&, juce::String title);
    void archive(const juce::String&, bool value = true);
    void remove(const juce::String&);
    void restore(const juce::String&);
    void purge(const juce::String&);
    juce::String begin(const juce::String& conversationId, const juce::String& requestId,
                       const juce::String& prompt, const juce::String& documentId,
                       std::uint64_t revision);
    void finish(const juce::String& conversationId, const juce::String& requestId,
                const juce::String& status, const juce::String& text,
                const juce::String& proposal = {}, const juce::String& targetDocument = {},
                std::uint64_t targetRevision = 0, bool retarget = false);
    juce::Array<juce::var> context(const juce::String&) const;
    void endRequest(const juce::String& requestId);
    bool proposalAvailable(const juce::String& conversationId, const juce::String& requestId,
                           const juce::String& source);

private:
    struct Turn {
        juce::String id, prompt, text, status, documentId, proposal, owner;
        std::uint64_t revision = 0;
        juce::int64 ownerPid = 0;
    };
    struct Conversation {
        juce::String id, title;
        bool archived = false, deleted = false;
        juce::int64 updated = 0;
        std::vector<Turn> messages;
    };
    juce::File file_;
    juce::String activeId_, ownerId_, error_;
    std::vector<Conversation> conversations_;
    void transact(const std::function<void()>&);
    bool read();
    juce::var serialized() const;
    void save(bool reservePending = false);
    Conversation& find(const juce::String&);
    const Conversation& find(const juce::String&) const;
    static void requireIdle(const Conversation&);
};
}  // namespace flows::app
