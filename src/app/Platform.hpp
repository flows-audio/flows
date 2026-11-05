#pragma once
// Ports between the portable core and its hosts. A host (JUCE on macOS, the
// browser via Emscripten) implements these; the core never calls a platform
// API directly. Files use std::filesystem against host-provided directories.
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flows::app {
struct HttpResult {
    enum class Error { none, cancelled, timeout, connect, tooLarge };
    Error error = Error::none;
    int status = 0;
    std::string body;
};
// Blocking HTTPS client used from background threads only.
class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpResult post(const std::string& url, const std::string& body,
                            const std::vector<std::pair<std::string, std::string>>& headers,
                            const std::function<bool()>& cancelled, int timeoutMs, std::size_t responseLimit) = 0;
};

// Factory files the host ships with the app: example instruments, samples and
// language reference. Names are file names (no folders).
struct BundledFile {
    std::string name;
    std::string_view bytes;
};
}  // namespace flows::app
