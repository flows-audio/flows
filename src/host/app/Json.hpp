#pragma once
// JUCE-side JSON helpers. The portable core speaks flows::json::Value; this
// header converts at the JUCE boundary (WebView bridge, tests, plugin state).
#include "app/Json.hpp"

#include <juce_core/juce_core.h>

#include <stdexcept>

namespace flows::app {
inline juce::var toJuce(const json::Value& value) {
    switch (value.type()) {
        case json::Value::Type::null: return {};
        case json::Value::Type::boolean: return value.asBool();
        case json::Value::Type::integer: {
            const auto number = value.asInt();
            if (number >= std::numeric_limits<int>::min() && number <= std::numeric_limits<int>::max())
                return static_cast<int>(number);
            return static_cast<juce::int64>(number);
        }
        case json::Value::Type::real: return value.asDouble();
        case json::Value::Type::string: return juce::String::fromUTF8(value.string().data(), static_cast<int>(value.string().size()));
        case json::Value::Type::array: {
            juce::Array<juce::var> items;
            items.ensureStorageAllocated(static_cast<int>(value.size()));
            for (const auto& item : *value.getArray()) items.add(toJuce(item));
            return items;
        }
        case json::Value::Type::object: {
            auto* object = new juce::DynamicObject;
            for (const auto& [key, item] : *value.getObject())
                object->setProperty(juce::Identifier(juce::String::fromUTF8(key.data(), static_cast<int>(key.size()))),
                                    toJuce(item));
            return juce::var(object);
        }
    }
    return {};
}
inline json::Value fromJuce(const juce::var& value) {
    if (value.isVoid() || value.isUndefined()) return {};
    if (value.isBool()) return static_cast<bool>(value);
    if (value.isInt()) return static_cast<int>(value);
    if (value.isInt64()) return static_cast<std::int64_t>(static_cast<juce::int64>(value));
    if (value.isDouble()) return static_cast<double>(value);
    if (value.isString()) return value.toString().toStdString();
    if (const auto* items = value.getArray()) {
        json::Array result;
        result.reserve(static_cast<std::size_t>(items->size()));
        for (const auto& item : *items) result.push_back(fromJuce(item));
        return json::Value(std::move(result));
    }
    if (const auto* object = value.getDynamicObject()) {
        json::Object result;
        const auto& properties = object->getProperties();
        for (int i = 0; i < properties.size(); ++i)
            result.set(properties.getName(i).toString().toStdString(), fromJuce(properties.getValueAt(i)));
        return json::Value(std::move(result));
    }
    return {};
}

// Builders used by the JUCE host and its tests.
inline juce::var jsonValue(const std::string& text) { return juce::String(text); }
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
inline juce::var get(const juce::var& v, const char* key) { return v.getProperty(key, {}); }
inline juce::String str(const juce::var& v, const char* key) { return get(v, key).toString(); }
// Bounded parse through the portable parser (depth, separator and byte budgets).
inline juce::var parseJSONBounded(const juce::String& text, std::size_t maxBytes = 3 * 1024 * 1024) {
    return toJuce(json::parseBounded(text.toStdString(), maxBytes));
}
}  // namespace flows::app
