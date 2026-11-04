#include "AIEndpoint.hpp"

#include "Text.hpp"

#include <cctype>

namespace flows::app {
namespace {
// Index just after "scheme:" when the URL starts with "scheme://", else 0.
std::size_t endOfScheme(std::string_view url) {
    std::size_t i = 0;
    while (i < url.size() && (std::isalnum(static_cast<unsigned char>(url[i])) || url[i] == '+' ||
                              url[i] == '-' || url[i] == '.'))
        ++i;
    return url.substr(i).starts_with("://") ? i + 1 : 0;
}
}  // namespace

std::string urlScheme(std::string_view url) {
    const auto end = endOfScheme(url);
    return end == 0 ? std::string() : std::string(url.substr(0, end - 1));
}
std::string urlDomain(std::string_view url) {
    auto start = endOfScheme(url);
    while (start < url.size() && url[start] == '/') ++start;
    const auto end = url.find_first_of("/:", start);
    return std::string(url.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
}
bool isAllowedAIEndpoint(std::string_view endpoint) {
    if (endpoint.empty() || text::codePointCount(endpoint) > 2048 || text::containsAnyOf(endpoint, "\r\n\\"))
        return false;
    const auto scheme = urlScheme(endpoint);
    if (scheme != "https" && scheme != "http") return false;
    if (!endpoint.starts_with(scheme + "://")) return false;
    auto authority = endpoint.substr(scheme.size() + 3);
    if (const auto end = authority.find_first_of("/?#"); end != std::string_view::npos)
        authority = authority.substr(0, end);
    if (authority.empty() ||
        !text::containsOnly(authority, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:[]"))
        return false;
    const auto host = urlDomain(endpoint);
    if (host.empty()) return false;
    if (scheme == "https") return true;
    if (host != "localhost" && host != "127.0.0.1") return false;
    if (authority.size() < host.size()) return false;
    const auto suffix = authority.substr(host.size());
    if (suffix.empty()) return true;
    if (suffix.front() != ':') return false;
    const auto port = suffix.substr(1);
    if (port.empty() || port.size() > 5 || !text::containsOnly(port, "0123456789")) return false;
    const int value = std::stoi(std::string(port));
    return value > 0 && value <= 65535;
}
bool isLoopbackEndpoint(std::string_view endpoint) {
    const auto host = urlDomain(endpoint);
    return host == "localhost" || host == "127.0.0.1";
}
}  // namespace flows::app
