#pragma once
#include <juce_core/juce_core.h>

namespace flows::app {
// Reject userinfo and ambiguous authority spellings before using JUCE's parsed
// scheme/domain. A prefix such as 127.0.0.1:80@remote.invalid is not loopback.
inline bool isAllowedAIEndpoint(const juce::String& endpoint) {
    if (endpoint.isEmpty() || endpoint.length() > 2048 || endpoint.containsAnyOf("\r\n\\"))
        return false;
    const juce::URL url(endpoint);
    const auto scheme = url.getScheme();
    if (scheme != "https" && scheme != "http") return false;
    if (!endpoint.startsWith(scheme + "://")) return false;
    auto authority = endpoint.substring(scheme.length() + 3);
    const int end = authority.indexOfAnyOf("/?#");
    if (end >= 0) authority = authority.substring(0, end);
    if (authority.isEmpty() ||
        !authority.containsOnly(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:[]"))
        return false;
    const auto host = url.getDomain();
    if (host.isEmpty()) return false;
    if (scheme == "https") return true;
    if (host != "localhost" && host != "127.0.0.1") return false;
    auto suffix = authority.substring(host.length());
    if (suffix.isEmpty()) return true;
    if (!suffix.startsWithChar(':')) return false;
    auto port = suffix.substring(1);
    return port.isNotEmpty() && port.containsOnly("0123456789") && port.length() <= 5 &&
           port.getIntValue() > 0 && port.getIntValue() <= 65535;
}
}  // namespace flows::app
