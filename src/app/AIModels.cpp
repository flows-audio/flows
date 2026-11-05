#include "AIModels.hpp"

#include "AIEndpoint.hpp"
#include "Files.hpp"
#include "Text.hpp"

#include <map>
#include <set>
#include <stdexcept>

namespace flows::app {
namespace {
std::mutex modelMutex;
[[noreturn]] void failure(const std::string& message) { throw std::runtime_error(message); }
struct Lock {
    std::lock_guard<std::mutex> local{modelMutex};
    files::ProcessLock process;
    explicit Lock(const std::string& name) : process(name) {
        if (!process.enter(2000)) failure("Model settings are busy. Try again.");
    }
};
json::Value emptyModels() {
    return json::object({{"version", 1}, {"selectedId", ""}, {"profiles", json::Value::array()}});
}
}  // namespace

AISecrets memoryAISecrets(bool persistent) {
    auto keys = std::make_shared<std::map<std::string, std::string>>();
    auto mutex = std::make_shared<std::mutex>();
    AISecrets secrets;
    secrets.read = [keys, mutex](const std::string& id) {
        std::lock_guard lock(*mutex);
        const auto found = keys->find(id);
        return found == keys->end() ? std::string() : found->second;
    };
    secrets.write = [keys, mutex](const std::string& id, const std::string& key) {
        std::lock_guard lock(*mutex);
        (*keys)[id] = key;
    };
    secrets.remove = [keys, mutex](const std::string& id) {
        std::lock_guard lock(*mutex);
        keys->erase(id);
    };
    secrets.persistent = persistent;
    return secrets;
}

AIModels::AIModels(std::filesystem::path configDirectory, AISecrets secrets)
    : secrets_(secrets.read ? std::move(secrets) : memoryAISecrets()),
      file_(std::move(configDirectory) / "ai-models.json"),
      lockName_("flows-ai-models-" + text::fnv1aDigest(file_.string()).substr(8)) {}

void AIModels::read() {
    if (!files::isFile(file_)) {
        data_ = emptyModels();
        return;
    }
    if (files::size(file_) > 262144) failure("Global model settings exceed their size limit.");
    const auto data = json::parseBounded(files::read(file_, 262144), 262144);
    const auto& profiles = data["profiles"];
    if (data["version"].asInt() != 1 || !profiles.isArray() || profiles.size() > 64)
        failure("Global model settings are invalid. The saved file was left intact.");
    std::set<std::string> ids;
    for (const auto& p : *profiles.getArray()) {
        const auto id = json::str(p, "id");
        if (id.empty() || !ids.insert(id).second || !isAllowedAIEndpoint(json::str(p, "endpoint")) ||
            json::str(p, "model").empty())
            failure("Global model settings contain an invalid profile.");
    }
    data_ = data;
}
void AIModels::commit(const json::Value& data) {
    if (!files::createDirectories(file_.parent_path()))
        failure("Could not create the global model settings directory.");
    try {
        files::writeAtomic(file_, json::stringifyPretty(data));
    } catch (const std::exception&) {
        failure("Could not save global model settings.");
    }
    data_ = data;
}
json::Value AIModels::selected() const {
    if (const auto* profiles = data_["profiles"].getArray())
        for (const auto& p : *profiles)
            if (json::str(p, "id") == json::str(data_, "selectedId")) return p;
    return {};
}
void AIModels::migrate(const std::string& endpoint, const std::string& model) {
    Lock guard(lockName_);
    read();
    // Only the first launch imports legacy workspace settings; never overwrite
    // an existing global choice or import credentials from an instrument.
    if (files::isFile(file_) || model.empty() || !isAllowedAIEndpoint(endpoint)) return;
    const auto id = text::randomId();
    json::Array profiles;
    profiles.push_back(json::object(
        {{"id", id}, {"name", model}, {"endpoint", endpoint}, {"model", model}, {"credential", ""}}));
    commit(json::object({{"version", 1}, {"selectedId", id}, {"profiles", std::move(profiles)}}));
}
json::Value AIModels::snapshot() {
    Lock guard(lockName_);
    read();
    return metadata();
}
json::Value AIModels::metadata() const {
    json::Array profiles;
    if (const auto* list = data_["profiles"].getArray())
        for (const auto& p : *list) {
            const auto credential = json::str(p, "credential");
            // Session-only secrets disappear on reload; report what is usable now.
            bool hasKey = !credential.empty();
            if (hasKey && !secrets_.persistent) {
                try {
                    hasKey = !secrets_.read(credential).empty();
                } catch (...) {
                    hasKey = false;
                }
            }
            profiles.push_back(json::object({{"id", json::str(p, "id")},
                                             {"name", json::str(p, "name")},
                                             {"model", json::str(p, "model")},
                                             {"endpoint", json::str(p, "endpoint")},
                                             {"hasKey", hasKey}}));
        }
    return json::object({{"profiles", std::move(profiles)}, {"selectedId", json::str(data_, "selectedId")}});
}
AIModels::Connection AIModels::connection(json::Value* publicMetadata) {
    Lock guard(lockName_);
    read();
    if (publicMetadata) *publicMetadata = metadata();
    const auto p = selected();
    const auto credential = json::str(p, "credential");
    if (credential != cachedCredential_ || !secrets_.persistent) {
        // Never keep a prior endpoint's key after a failed credential lookup.
        cachedCredential_.clear();
        cachedKey_.clear();
        cachedError_.clear();
        cachedCredential_ = credential;
        if (!credential.empty()) try {
                cachedKey_ = secrets_.read(credential);
            } catch (const std::exception& e) {
                cachedError_ = e.what();
            }
    }
    if (!cachedError_.empty()) failure(cachedError_);
    return {json::str(p, "endpoint"), json::str(p, "model"), cachedKey_};
}
void AIModels::save(const json::Value& params) {
    auto endpoint = text::trimEnd(text::trim(json::str(params, "endpoint")), "/");
    auto model = text::trim(json::str(params, "model"));
    auto name = text::trim(json::str(params, "name"));
    const auto key = json::str(params, "apiKey");
    if (!isAllowedAIEndpoint(endpoint)) failure("Use an HTTPS URL without user information, or HTTP on localhost.");
    if (model.empty() || text::codePointCount(model) > 200 || text::containsAnyOf(model, "\r\n"))
        failure("Enter a model identifier of at most 200 characters.");
    if (name.empty()) name = model;
    if (text::codePointCount(name) > 100 || text::codePointCount(key) > 16384 || text::containsAnyOf(key, "\r\n"))
        failure("Invalid model name or API key.");
    Lock guard(lockName_);
    read();
    auto profiles = *data_["profiles"].getArray();
    auto id = json::str(params, "id");
    int index = -1;
    for (int i = 0; i < static_cast<int>(profiles.size()); ++i)
        if (!id.empty() ? json::str(profiles[i], "id") == id
                        : json::str(profiles[i], "endpoint") == endpoint && json::str(profiles[i], "model") == model) {
            index = i;
            break;
        }
    if (!id.empty() && index < 0) failure("This model was removed. Refresh its settings.");
    if (index < 0 && profiles.size() >= 64) failure("Remove a saved model before adding another (limit 64).");
    const auto old = index >= 0 ? profiles[index] : json::Value();
    if (id.empty()) id = index >= 0 ? json::str(old, "id") : text::randomId();
    const auto oldCredential = json::str(old, "credential");
    auto credential = json::str(old, "endpoint") == endpoint ? oldCredential : std::string();
    if (!key.empty()) {
        credential = text::randomId();
        secrets_.write(credential, key);
    }
    auto p = json::object(
        {{"id", id}, {"name", name}, {"endpoint", endpoint}, {"model", model}, {"credential", credential}});
    if (index < 0)
        profiles.push_back(std::move(p));
    else
        profiles[index] = std::move(p);
    try {
        commit(json::object({{"version", 1}, {"selectedId", id}, {"profiles", std::move(profiles)}}));
    } catch (...) {
        if (!key.empty()) try {
                secrets_.remove(credential);
            } catch (...) {
            }
        throw;
    }
    cachedCredential_.clear();
    cachedKey_.clear();
    cachedError_.clear();
    if (!oldCredential.empty() && oldCredential != credential) try {
            secrets_.remove(oldCredential);
        } catch (...) {
        }  // Orphan cleanup must not undo a successful save.
}
void AIModels::select(const std::string& id) {
    Lock guard(lockName_);
    read();
    const auto profiles = data_["profiles"];
    bool found = false;
    for (const auto& p : *profiles.getArray()) found |= json::str(p, "id") == id;
    if (!found) failure("This model is no longer available.");
    commit(json::object({{"version", 1}, {"selectedId", id}, {"profiles", profiles}}));
    cachedCredential_.clear();
    cachedKey_.clear();
    cachedError_.clear();
}
void AIModels::remove(const std::string& id) {
    Lock guard(lockName_);
    read();
    auto profiles = *data_["profiles"].getArray();
    std::string credential;
    bool found = false;
    for (int i = static_cast<int>(profiles.size()); --i >= 0;)
        if (json::str(profiles[i], "id") == id) {
            credential = json::str(profiles[i], "credential");
            profiles.erase(profiles.begin() + i);
            found = true;
        }
    if (!found) failure("This model is no longer available.");
    auto selectedId = json::str(data_, "selectedId");
    if (selectedId == id) selectedId = profiles.empty() ? std::string() : json::str(profiles[0], "id");
    commit(json::object({{"version", 1}, {"selectedId", selectedId}, {"profiles", std::move(profiles)}}));
    if (!credential.empty()) try {
            secrets_.remove(credential);
        } catch (...) {
        }
    cachedCredential_.clear();
    cachedKey_.clear();
    cachedError_.clear();
}
}  // namespace flows::app
