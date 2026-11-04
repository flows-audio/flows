#include "AIModels.hpp"

#include <set>

#include "AIEndpoint.hpp"

namespace flows::app {
namespace {
using namespace juce;
std::mutex modelMutex;
[[noreturn]] void failure(const String& message) {
    throw std::runtime_error(message.toStdString());
}
struct Lock {
    std::lock_guard<std::mutex> local{modelMutex};
    InterProcessLock& process;
    explicit Lock(InterProcessLock& p) : process(p) {
        if (!process.enter(2000)) failure("Model settings are busy. Try again.");
    }
    ~Lock() { process.exit(); }
};
var emptyModels() {
    return object({{"version", 1}, {"selectedId", ""}, {"profiles", Array<var>()}});
}
}  // namespace
AIModels::AIModels(AISecrets secrets)
    : secrets_(secrets.read ? std::move(secrets) : systemAISecrets()) {
    const auto overridePath = juce::SystemStats::getEnvironmentVariable("FLOWS_AI_CONFIG_DIR", {});
    auto root = overridePath.isNotEmpty()
                    ? juce::File(overridePath)
                    : juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                          .getChildFile("Flows");
    file_ = root.getChildFile("ai-models.json");
    lock_ = std::make_unique<juce::InterProcessLock>(
        "flows-ai-models-" + juce::String(file_.getFullPathName().hashCode64()));
}
void AIModels::read() {
    if (!file_.existsAsFile()) {
        data_ = emptyModels();
        return;
    }
    if (file_.getSize() > 262144) failure("Global model settings exceed their size limit.");
    const auto data = parseJSONBounded(file_.loadFileAsString(), 262144);
    const auto profiles = get(data, "profiles");
    if (static_cast<int>(get(data, "version")) != 1 || !profiles.isArray() || profiles.size() > 64)
        failure("Global model settings are invalid. The saved file was left intact.");
    std::set<juce::String> ids;
    for (const auto& p : *profiles.getArray()) {
        const auto id = str(p, "id");
        if (id.isEmpty() || !ids.insert(id).second || !isAllowedAIEndpoint(str(p, "endpoint")) ||
            str(p, "model").isEmpty())
            failure("Global model settings contain an invalid profile.");
    }
    data_ = data;
}
void AIModels::commit(const juce::var& data) {
    if (file_.getParentDirectory().createDirectory().failed())
        failure("Could not create the global model settings directory.");
    juce::TemporaryFile temporary(file_);
    if (!temporary.getFile().replaceWithText(juce::JSON::toString(data, true)) ||
        !temporary.overwriteTargetFileWithTemporary())
        failure("Could not save global model settings.");
    data_ = data;
}
juce::var AIModels::selected() const {
    const auto profiles = get(data_, "profiles");
    if (profiles.isArray())
        for (const auto& p : *profiles.getArray())
            if (str(p, "id") == str(data_, "selectedId")) return p;
    return {};
}
void AIModels::migrate(const juce::String& endpoint, const juce::String& model) {
    Lock guard(*lock_);
    read();
    // Only the first launch imports legacy workspace settings; never overwrite
    // an existing global choice or import credentials from an instrument.
    if (file_.existsAsFile() || model.isEmpty() || !isAllowedAIEndpoint(endpoint)) return;
    const auto id = juce::Uuid().toString();
    commit(object({{"version", 1},
                   {"selectedId", id},
                   {"profiles", juce::Array<juce::var>{object({{"id", id},
                                                               {"name", model},
                                                               {"endpoint", endpoint},
                                                               {"model", model},
                                                               {"credential", ""}})}}}));
}
juce::var AIModels::snapshot() {
    Lock guard(*lock_);
    read();
    return metadata();
}
juce::var AIModels::metadata() const {
    juce::Array<juce::var> profiles;
    for (const auto& p : *get(data_, "profiles").getArray())
        profiles.add(object({{"id", str(p, "id")},
                             {"name", str(p, "name")},
                             {"model", str(p, "model")},
                             {"endpoint", str(p, "endpoint")},
                             {"hasKey", str(p, "credential").isNotEmpty()}}));
    return object({{"profiles", profiles}, {"selectedId", str(data_, "selectedId")}});
}
AIModels::Connection AIModels::connection(juce::var* publicMetadata) {
    Lock guard(*lock_);
    read();
    if (publicMetadata) *publicMetadata = metadata();
    const auto p = selected();
    const auto credential = str(p, "credential");
    if (credential != cachedCredential_) {
        // Never keep a prior endpoint's key after a failed Keychain lookup.
        cachedCredential_.clear();
        cachedKey_.clear();
        cachedError_.clear();
        cachedCredential_ = credential;
        if (credential.isNotEmpty()) try {
                cachedKey_ = secrets_.read(credential);
            } catch (const std::exception& e) {
                cachedError_ = e.what();
            }
    }
    if (cachedError_.isNotEmpty()) failure(cachedError_);
    return {str(p, "endpoint"), str(p, "model"), cachedKey_};
}
void AIModels::save(const juce::var& params) {
    auto endpoint = str(params, "endpoint").trim().trimCharactersAtEnd("/"),
         model = str(params, "model").trim(), name = str(params, "name").trim();
    const auto key = str(params, "apiKey");
    if (!isAllowedAIEndpoint(endpoint))
        failure("Use an HTTPS URL without user information, or HTTP on localhost.");
    if (model.isEmpty() || model.length() > 200 || model.containsAnyOf("\r\n"))
        failure("Enter a model identifier of at most 200 characters.");
    if (name.isEmpty()) name = model;
    if (name.length() > 100 || key.length() > 16384 || key.containsAnyOf("\r\n"))
        failure("Invalid model name or API key.");
    Lock guard(*lock_);
    read();
    auto profiles = *get(data_, "profiles").getArray();
    auto id = str(params, "id");
    int index = -1;
    for (int i = 0; i < profiles.size(); ++i)
        if (id.isNotEmpty()
                ? str(profiles[i], "id") == id
                : str(profiles[i], "endpoint") == endpoint && str(profiles[i], "model") == model) {
            index = i;
            break;
        }
    if (id.isNotEmpty() && index < 0) failure("This model was removed. Refresh its settings.");
    if (index < 0 && profiles.size() >= 64)
        failure("Remove a saved model before adding another (limit 64).");
    const auto old = index >= 0 ? profiles[index] : juce::var();
    if (id.isEmpty()) id = index >= 0 ? str(old, "id") : juce::Uuid().toString();
    const auto oldCredential = str(old, "credential");
    auto credential = str(old, "endpoint") == endpoint ? oldCredential : juce::String();
    if (key.isNotEmpty()) {
        credential = juce::Uuid().toString();
        secrets_.write(credential, key);
    }
    const auto p = object({{"id", id},
                           {"name", name},
                           {"endpoint", endpoint},
                           {"model", model},
                           {"credential", credential}});
    if (index < 0)
        profiles.add(p);
    else
        profiles.set(index, p);
    try {
        commit(object({{"version", 1}, {"selectedId", id}, {"profiles", profiles}}));
    } catch (...) {
        if (key.isNotEmpty()) try {
                secrets_.remove(credential);
            } catch (...) {
            }
        throw;
    }
    cachedCredential_.clear();
    cachedKey_.clear();
    cachedError_.clear();
    if (oldCredential.isNotEmpty() && oldCredential != credential) try {
            secrets_.remove(oldCredential);
        } catch (...) {
        }  // Orphan cleanup must not undo a successful save.
}
void AIModels::select(const juce::String& id) {
    Lock guard(*lock_);
    read();
    const auto profiles = get(data_, "profiles");
    bool found = false;
    for (const auto& p : *profiles.getArray()) found |= str(p, "id") == id;
    if (!found) failure("This model is no longer available.");
    commit(object({{"version", 1}, {"selectedId", id}, {"profiles", profiles}}));
    cachedCredential_.clear();
    cachedKey_.clear();
    cachedError_.clear();
}
void AIModels::remove(const juce::String& id) {
    Lock guard(*lock_);
    read();
    auto profiles = *get(data_, "profiles").getArray();
    juce::String credential;
    bool found = false;
    for (int i = profiles.size(); --i >= 0;)
        if (str(profiles[i], "id") == id) {
            credential = str(profiles[i], "credential");
            profiles.remove(i);
            found = true;
        }
    if (!found) failure("This model is no longer available.");
    auto selectedId = str(data_, "selectedId");
    if (selectedId == id) selectedId = profiles.isEmpty() ? juce::String() : str(profiles[0], "id");
    commit(object({{"version", 1}, {"selectedId", selectedId}, {"profiles", profiles}}));
    if (credential.isNotEmpty()) try {
            secrets_.remove(credential);
        } catch (...) {
        }
    cachedCredential_.clear();
    cachedKey_.clear();
    cachedError_.clear();
}
}  // namespace flows::app
