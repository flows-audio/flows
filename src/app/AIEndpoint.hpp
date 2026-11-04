#pragma once
#include <string>
#include <string_view>

namespace flows::app {
// Scheme as written before "://", or empty.
std::string urlScheme(std::string_view url);
// Host part after "://", ending at the first '/' or ':' (as the previous host read it).
std::string urlDomain(std::string_view url);
// Reject userinfo and ambiguous authority spellings before trusting the
// scheme/domain. A prefix such as 127.0.0.1:80@remote.invalid is not loopback.
bool isAllowedAIEndpoint(std::string_view endpoint);
bool isLoopbackEndpoint(std::string_view endpoint);
}  // namespace flows::app
