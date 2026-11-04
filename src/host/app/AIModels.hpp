#pragma once
#include <functional>
#include <mutex>

#include "Json.hpp"

namespace flows::app {
// Injected in tests so persistence can be exercised without touching Keychain.
struct AISecrets {
    std::function<juce::String(const juce::String&)> read;
    std::function<void(const juce::String&, const juce::String&)> write;
    std::function<void(const juce::String&)> remove;
};
AISecrets systemAISecrets();
class AIModels {
public:
    explicit AIModels(AISecrets secrets = {});
    void migrate(const juce::String& endpoint, const juce::String& model);
    juce::var snapshot();
    void save(const juce::var& params);
    void select(const juce::String& id);
    void remove(const juce::String& id);
    struct Connection {
        juce::String endpoint, model, key;
    };
    Connection connection(juce::var* metadata = nullptr);

private:
    AISecrets secrets_;
    juce::File file_;
    std::unique_ptr<juce::InterProcessLock> lock_;
    juce::var data_;
    juce::String cachedCredential_, cachedKey_, cachedError_;
    juce::var metadata() const;
    void read();
    void commit(const juce::var& data);
    juce::var selected() const;
};
}  // namespace flows::app
