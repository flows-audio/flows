#pragma once
// Protocol facts shared by every host. The full contract (methods, params,
// results, events, capabilities) lives in protocol/flows-protocol.json; the
// UI's TypeScript types are generated from it.
#include "Json.hpp"

#include <map>
#include <optional>
#include <string>

namespace flows::app {
// Bump when a message changes incompatibly. Additive fields keep the version.
inline constexpr int protocolVersion = 1;

// Fills defaults for capabilities a host did not state, so the UI never has
// to guess from the transport which features exist.
json::Value protocolCapabilities(json::Value hostCapabilities);

// Splits snapshots into top-level slices and reports only the slices that
// changed since the previous call. One stream per connected UI.
class StateStream {
public:
    // Returns {"protocol":N,"full":bool,"slices":{...}} or nullopt if nothing changed.
    std::optional<json::Value> next(const json::Value& snapshot);
    // Records a snapshot the UI received another way (a request result), so
    // later deltas are computed against what the UI actually holds.
    void observe(const json::Value& snapshot);
    void reset() { previous_.clear(); }

private:
    std::map<std::string, std::string> previous_;
};
}  // namespace flows::app
