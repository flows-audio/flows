#include "Text.hpp"

#include "Json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>

namespace flows::text {
namespace {
bool isSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Length of the UTF-8 sequence at text[i], or 0 if it is invalid there.
std::size_t sequenceLength(std::string_view text, std::size_t i) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) return 1;
    std::size_t length = 0;
    std::uint32_t code = 0;
    if ((c & 0xe0) == 0xc0) {
        length = 2;
        code = c & 0x1f;
    } else if ((c & 0xf0) == 0xe0) {
        length = 3;
        code = c & 0x0f;
    } else if ((c & 0xf8) == 0xf0) {
        length = 4;
        code = c & 0x07;
    } else
        return 0;
    if (i + length > text.size()) return 0;
    for (std::size_t k = 1; k < length; ++k) {
        const auto next = static_cast<unsigned char>(text[i + k]);
        if ((next & 0xc0) != 0x80) return 0;
        code = (code << 6) | (next & 0x3f);
    }
    // Reject overlong forms, surrogates and values above U+10FFFF.
    if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) ||
        (length == 4 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        return 0;
    return length;
}
void appendUtf8(std::string& out, std::uint32_t code) {
    if (code < 0x80)
        out += static_cast<char>(code);
    else if (code < 0x800) {
        out += static_cast<char>(0xc0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xe0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code & 0x3f));
    }
}
}  // namespace

std::string trim(std::string_view text) {
    std::size_t start = 0, end = text.size();
    while (start < end && isSpace(static_cast<unsigned char>(text[start]))) ++start;
    while (end > start && isSpace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(start, end - start));
}
std::string trimEnd(std::string_view text, std::string_view characters) {
    auto end = text.size();
    while (end > 0 && characters.find(text[end - 1]) != std::string_view::npos) --end;
    return std::string(text.substr(0, end));
}
bool containsAnyOf(std::string_view text, std::string_view characters) {
    return text.find_first_of(characters) != std::string_view::npos;
}
bool containsOnly(std::string_view text, std::string_view characters) {
    return text.find_first_not_of(characters) == std::string_view::npos;
}
std::string toLowerAscii(std::string_view text) {
    std::string result(text);
    for (auto& c : result) c = lower(c);
    return result;
}
bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
bool endsWithIgnoreCase(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           equalsIgnoreCase(text.substr(text.size() - suffix.size()), suffix);
}
std::size_t findIgnoreCase(std::string_view text, std::string_view needle) {
    if (needle.empty()) return 0;
    if (needle.size() > text.size()) return std::string_view::npos;
    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i)
        if (equalsIgnoreCase(text.substr(i, needle.size()), needle)) return i;
    return std::string_view::npos;
}
bool containsIgnoreCase(std::string_view text, std::string_view needle) {
    return findIgnoreCase(text, needle) != std::string_view::npos;
}
std::string replaceAll(std::string text, std::string_view from, std::string_view to) {
    if (from.empty()) return text;
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}
std::vector<std::string> split(std::string_view text, std::string_view separators, bool keepEmpty) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || separators.find(text[i]) != std::string_view::npos) {
            if (keepEmpty || i > start) parts.emplace_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

bool isValidUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto length = sequenceLength(text, i);
        if (length == 0) return false;
        i += length;
    }
    return true;
}
std::size_t codePointCount(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++count) i += std::max<std::size_t>(1, sequenceLength(text, i));
    return count;
}
std::size_t byteOffsetOfCodePoint(std::string_view text, std::size_t codePoint) {
    std::size_t i = 0;
    for (std::size_t count = 0; i < text.size() && count < codePoint; ++count)
        i += std::max<std::size_t>(1, sequenceLength(text, i));
    return std::min(i, text.size());
}
std::string codePointSubstring(std::string_view text, std::int64_t start, std::int64_t end) {
    if (start < 0) start = 0;
    if (end <= start) return {};
    const auto from = byteOffsetOfCodePoint(text, static_cast<std::size_t>(start));
    const auto to = byteOffsetOfCodePoint(text, static_cast<std::size_t>(end));
    return std::string(text.substr(from, to - from));
}
int utf16Offset(std::string_view source, std::size_t bytes) {
    bytes = std::min(bytes, source.size());
    int units = 0;
    for (std::size_t i = 0; i < bytes;) {
        const auto length = sequenceLength(source, i);
        if (length == 0) {
            ++units;
            ++i;
            continue;
        }
        units += length == 4 ? 2 : 1;
        i += length;
    }
    return units;
}
std::string sanitizeUtf8(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size();) {
        const auto length = sequenceLength(bytes, i);
        if (length == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(bytes.substr(i, length));
            i += length;
        }
    }
    return out;
}
std::string decodeText(std::string_view bytes) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(bytes[i]); };
    if (bytes.size() >= 3 && byte(0) == 0xef && byte(1) == 0xbb && byte(2) == 0xbf)
        return sanitizeUtf8(bytes.substr(3));
    if (bytes.size() >= 2 && ((byte(0) == 0xff && byte(1) == 0xfe) || (byte(0) == 0xfe && byte(1) == 0xff))) {
        const bool little = byte(0) == 0xff;
        std::string out;
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
            std::uint32_t unit = little ? (byte(i) | (byte(i + 1) << 8)) : ((byte(i) << 8) | byte(i + 1));
            if (unit >= 0xd800 && unit <= 0xdbff && i + 3 < bytes.size()) {
                const std::uint32_t low = little ? (byte(i + 2) | (byte(i + 3) << 8)) : ((byte(i + 2) << 8) | byte(i + 3));
                if (low >= 0xdc00 && low <= 0xdfff) {
                    unit = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
                    i += 2;
                } else
                    unit = 0xfffd;
            } else if (unit >= 0xd800 && unit <= 0xdfff)
                unit = 0xfffd;
            appendUtf8(out, unit);
        }
        return out;
    }
    return sanitizeUtf8(bytes);
}

std::string hex(std::uint64_t value) {
    if (value == 0) return "0";
    std::string out;
    while (value) {
        out += "0123456789abcdef"[value & 15];
        value >>= 4;
    }
    return {out.rbegin(), out.rend()};
}
std::string hexBytes(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        out += "0123456789abcdef"[b >> 4];
        out += "0123456789abcdef"[b & 15];
    }
    return out;
}

std::string base64Encode(std::string_view bytes) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const std::uint32_t n = (static_cast<unsigned char>(bytes[i]) << 16) |
                                (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                                static_cast<unsigned char>(bytes[i + 2]);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += table[n & 63];
    }
    if (i < bytes.size()) {
        std::uint32_t n = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) n |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}
std::optional<std::string> base64Decode(std::string_view text) {
    std::string out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0, padding = 0;
    for (char c : text) {
        int value;
        if (c >= 'A' && c <= 'Z')
            value = c - 'A';
        else if (c >= 'a' && c <= 'z')
            value = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            value = c - '0' + 52;
        else if (c == '+')
            value = 62;
        else if (c == '/')
            value = 63;
        else if (c == '=') {
            ++padding;
            continue;
        } else if (isSpace(static_cast<unsigned char>(c)))
            continue;
        else
            return std::nullopt;
        if (padding) return std::nullopt;  // data after padding
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xff);
        }
    }
    if (padding > 2 || bits >= 6) return std::nullopt;
    return out;
}

namespace {
struct Sha256 {
    std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    static constexpr std::array<std::uint32_t, 64> k{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    static std::uint32_t rotate(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const unsigned char* p) {
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
                   (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const auto s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const auto s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, hh] = h;
        for (int i = 0; i < 64; ++i) {
            const auto s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
            const auto ch = (e & f) ^ (~e & g);
            const auto t1 = hh + s1 + ch + k[i] + w[i];
            const auto s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
};
}  // namespace

std::string sha256Hex(std::string_view bytes) {
    Sha256 state;
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t i = 0;
    for (; i + 64 <= bytes.size(); i += 64) state.block(data + i);
    std::array<unsigned char, 128> tail{};
    const auto rest = bytes.size() - i;
    std::memcpy(tail.data(), data + i, rest);
    tail[rest] = 0x80;
    const std::size_t blocks = rest + 9 <= 64 ? 1 : 2;
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8;
    for (int b = 0; b < 8; ++b) tail[blocks * 64 - 1 - b] = static_cast<unsigned char>(bits >> (8 * b));
    for (std::size_t b = 0; b < blocks; ++b) state.block(tail.data() + 64 * b);
    std::array<std::uint8_t, 32> digest{};
    for (int w = 0; w < 8; ++w)
        for (int b = 0; b < 4; ++b) digest[4 * w + b] = static_cast<std::uint8_t>(state.h[w] >> (24 - 8 * b));
    return hexBytes(digest);
}
std::string fnv1aDigest(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return "fnv1a64:" + hex(hash);
}
std::string randomId() {
    static std::mutex mutex;
    static std::mt19937_64 generator = [] {
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device(), device(), device(), device(), device()};
        return std::mt19937_64(seed);
    }();
    std::array<std::uint8_t, 16> bytes{};
    {
        std::lock_guard lock(mutex);
        for (std::size_t i = 0; i < bytes.size(); i += 8) {
            const auto value = generator();
            for (int b = 0; b < 8; ++b) bytes[i + b] = static_cast<std::uint8_t>(value >> (8 * b));
        }
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
    return hexBytes(bytes);
}
std::int64_t currentTimeMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
double steadyMillis() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
std::string quoteJsonString(std::string_view value) { return json::stringify(json::Value(value)); }
}  // namespace flows::text
