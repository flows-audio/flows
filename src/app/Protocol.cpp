#include "Protocol.hpp"

#ifndef FLOWS_VERSION
#define FLOWS_VERSION ""
#endif

namespace flows::app {
json::Value protocolCapabilities(json::Value host) {
    if (!host.isObject()) host = json::Value::object();
    const auto fill = [&](const char* name, json::Value value) {
        if (!host.has(name)) host.set(name, std::move(value));
    };
    fill("protocol", protocolVersion);
    fill("host", "embedded");
    fill("windowTransparency", false);
    fill("audioSettings", "none");
    fill("midiInput", "none");
    fill("storage", "filesystem");
    fill("secrets", "none");
    fill("fileDialogs", "none");
    fill("clipboard", "none");
    fill("externalLinks", "none");
    fill("liveBridge", false);
    fill("ai", false);
    // AGPL-3.0 section 13: network users get the corresponding source.
    const std::string version = FLOWS_VERSION;
    fill("version", version);
    fill("sourceUrl", version.empty() ? std::string("https://github.com/flows-audio/flows")
                                      : "https://github.com/flows-audio/flows/tree/v" + version);
    return host;
}

std::optional<json::Value> StateStream::next(const json::Value& snapshot) {
    const auto* object = snapshot.getObject();
    if (!object) return std::nullopt;
    const bool full = previous_.empty();
    json::Value slices = json::Value::object();
    std::map<std::string, std::string> current;
    for (const auto& [key, value] : *object) {
        auto text = json::stringify(value);
        const auto found = previous_.find(key);
        if (full || found == previous_.end() || found->second != text) slices.set(key, value);
        current.emplace(key, std::move(text));
    }
    // Keys that disappeared are reported as null so the UI can drop them.
    for (const auto& [key, text] : previous_)
        if (!current.contains(key)) slices.set(key, json::Value());
    previous_ = std::move(current);
    if (!full && slices.size() == 0) return std::nullopt;
    return json::object({{"protocol", protocolVersion}, {"full", full}, {"slices", std::move(slices)}});
}
}  // namespace flows::app
