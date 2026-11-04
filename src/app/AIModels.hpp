#pragma once
#include "Json.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace flows::app {
// Secret storage port. macOS uses Keychain; the browser keeps keys only for
// the current session (persistent = false), so they never reach disk.
struct AISecrets {
    std::function<std::string(const std::string&)> read;
    std::function<void(const std::string&, const std::string&)> write;
    std::function<void(const std::string&)> remove;
    bool persistent = true;
};
// In-memory secrets for tests and for hosts without a credential store.
AISecrets memoryAISecrets(bool persistent = false);

// Global model profiles (endpoint, model, credential id) shared by every
// instance on the machine. Keys live in AISecrets, never in the JSON file.
class AIModels {
public:
    AIModels(std::filesystem::path configDirectory, AISecrets secrets);
    void migrate(const std::string& endpoint, const std::string& model);
    json::Value snapshot();
    void save(const json::Value& params);
    void select(const std::string& id);
    void remove(const std::string& id);
    struct Connection {
        std::string endpoint, model, key;
    };
    Connection connection(json::Value* metadata = nullptr);
    bool persistentSecrets() const { return secrets_.persistent; }

private:
    AISecrets secrets_;
    std::filesystem::path file_;
    std::string lockName_;
    json::Value data_;
    std::string cachedCredential_, cachedKey_, cachedError_;
    json::Value metadata() const;
    void read();
    void commit(const json::Value& data);
    json::Value selected() const;
};
}  // namespace flows::app
