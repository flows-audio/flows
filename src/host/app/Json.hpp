#pragma once
#include <juce_core/juce_core.h>

#include <array>
#include <stdexcept>
namespace flows::app {
inline juce::var jsonValue(const std::string& text) {
    return juce::String(text);
}
inline juce::var jsonValue(std::string_view text) {
    return juce::String::fromUTF8(text.data(), static_cast<int>(text.size()));
}
template <class T>
juce::var jsonValue(const T& value) {
    return juce::var(value);
}
struct JsonField {
    juce::Identifier key;
    juce::var value;
    template <class T>
    JsonField(const char* name, const T& v) : key(name), value(jsonValue(v)) {}
};
inline juce::var object(std::initializer_list<JsonField> fields) {
    auto* result = new juce::DynamicObject;
    for (const auto& [key, value] : fields) result->setProperty(key, value);
    return juce::var(result);
}
inline juce::var get(const juce::var& v, const char* key) {
    return v.getProperty(key, {});
}
inline juce::String str(const juce::var& v, const char* key) {
    return get(v, key).toString();
}
// JUCE's JSON parser recurses without a nesting bound. Scan untrusted text
// before entering it; punctuation inside quoted source/code is not structure.
inline juce::var parseJSONBounded(const juce::String& text,
                                  std::size_t maxBytes = 3 * 1024 * 1024) {
    if (text.getNumBytesAsUTF8() > maxBytes)
        throw std::runtime_error("JSON exceeds its byte budget.");
    std::array<std::size_t, 128> separators{};
    std::size_t depth = 0, values = 0;
    juce::juce_wchar quote = 0;
    bool escaped = false;
    for (auto character : text) {
        if (quote != 0) {
            if (escaped)
                escaped = false;
            else if (character == '\\')
                escaped = true;
            else if (character == quote)
                quote = 0;
            continue;
        }
        // JUCE also accepts single-quoted value strings.
        if (character == '"' || character == '\'') {
            quote = character;
            continue;
        }
        if (character == '{' || character == '[') {
            if (depth == separators.size())
                throw std::runtime_error("JSON nesting exceeds 128 levels.");
            separators[depth++] = 0;
        } else if (character == '}' || character == ']') {
            if (depth > 0) --depth;
        } else if (character == ',' && depth > 0) {
            if (++separators[depth - 1] > 4096 || ++values > 32768)
                throw std::runtime_error("JSON exceeds its value budget.");
        }
    }
    juce::var result;
    auto parsed = juce::JSON::parse(text, result);
    if (parsed.failed())
        throw std::runtime_error(parsed.getErrorMessage().substring(0, 512).toStdString());
    return result;
}
inline int utf16Offset(const std::string& source, std::size_t bytes) {
    auto s =
        juce::String::fromUTF8(source.data(), static_cast<int>(std::min(bytes, source.size())));
    int n = 0;
    for (auto c : s) n += c > 0xffff ? 2 : 1;
    return n;
}
}  // namespace flows::app
