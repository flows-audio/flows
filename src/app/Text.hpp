#pragma once
// Small, dependency-free text helpers for the portable app core. Strings are
// UTF-8 std::string. Functions named in code points say so; the UI protocol
// counts UTF-16 units (CodeMirror offsets), which utf16Offset converts to.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace flows::text {
std::string trim(std::string_view text);
std::string trimEnd(std::string_view text, std::string_view characters);
bool containsAnyOf(std::string_view text, std::string_view characters);
bool containsOnly(std::string_view text, std::string_view characters);
std::string toLowerAscii(std::string_view text);
bool equalsIgnoreCase(std::string_view a, std::string_view b);
bool endsWithIgnoreCase(std::string_view text, std::string_view suffix);
// ASCII case folding; other code points compare exactly.
std::size_t findIgnoreCase(std::string_view text, std::string_view needle);
bool containsIgnoreCase(std::string_view text, std::string_view needle);
std::string replaceAll(std::string text, std::string_view from, std::string_view to);
std::vector<std::string> split(std::string_view text, std::string_view separators,
                               bool keepEmpty = false);

// UTF-8 helpers. Invalid bytes count as one code point each.
bool isValidUtf8(std::string_view text);
std::size_t codePointCount(std::string_view text);
// Code-point substring [start, end), clamped like juce::String::substring.
std::string codePointSubstring(std::string_view text, std::int64_t start,
                               std::int64_t end = INT64_MAX);
// UTF-16 units in the first `bytes` bytes (clamped) of a UTF-8 string.
int utf16Offset(std::string_view source, std::size_t bytes);
// Byte offset of a code point index (clamped to the string length).
std::size_t byteOffsetOfCodePoint(std::string_view text, std::size_t codePoint);
// Decodes file bytes the way the previous host did: strips a UTF-8 BOM,
// decodes UTF-16 with a BOM, and replaces invalid UTF-8 with U+FFFD.
std::string decodeText(std::string_view bytes);
std::string sanitizeUtf8(std::string_view bytes);

std::string hex(std::uint64_t value);  // lowercase, no leading zeros
std::string hexBytes(std::span<const std::uint8_t> bytes);
std::string base64Encode(std::string_view bytes);
// Accepts standard Base64 with optional padding and ASCII whitespace.
std::optional<std::string> base64Decode(std::string_view text);
std::string sha256Hex(std::string_view bytes);
// FNV-1a 64-bit digest formatted as "fnv1a64:<hex>", used for AI source identity.
std::string fnv1aDigest(std::string_view bytes);
// 32 lowercase hex digits from a random version-4 UUID.
std::string randomId();
std::int64_t currentTimeMillis();
double steadyMillis();
std::string quoteJsonString(std::string_view text);
}  // namespace flows::text
