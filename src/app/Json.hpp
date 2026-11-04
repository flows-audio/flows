#pragma once
// Portable JSON value used by the Flows app core and every host bridge.
//
// The core speaks this type on all platforms; the JUCE host converts to and
// from juce::var at its boundary, the web host passes UTF-8 JSON text. Objects
// keep insertion order so serialized protocol messages stay stable.
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace flows::json {
class Value;
class Object;
using Array = std::vector<Value>;

namespace detail {
// Deep-copying heap box. Lets Value hold an Object before Object is complete.
template <class T>
class Box {
public:
    Box();
    explicit Box(T value);
    Box(const Box& other);
    Box(Box&& other) noexcept;
    Box& operator=(const Box& other);
    Box& operator=(Box&& other) noexcept;
    ~Box();
    T& get() { return *pointer_; }
    const T& get() const { return *pointer_; }

private:
    std::unique_ptr<T> pointer_;
};
}  // namespace detail

class Value {
public:
    enum class Type : std::uint8_t { null, boolean, integer, real, string, array, object };

    Value();
    Value(std::nullptr_t);
    Value(bool value);
    Value(int value);
    Value(unsigned value);
    Value(long value);
    Value(long long value);
    Value(unsigned long value);
    Value(unsigned long long value);
    Value(double value);
    Value(float value);
    Value(const char* text);
    Value(std::string text);
    Value(std::string_view text);
    Value(Array values);
    Value(Object values);
    // Prevent accidental pointer-to-bool conversions.
    template <class T>
    Value(const T*) = delete;
    Value(const Value&);
    Value(Value&&) noexcept;
    Value& operator=(const Value&);
    Value& operator=(Value&&) noexcept;
    ~Value();

    static Value array();
    static Value object();

    // Index 7 stores a float; it reads as a real number but serializes at
    // single precision (telemetry arrays are float sample data).
    Type type() const { return data_.index() == 7 ? Type::real : static_cast<Type>(data_.index()); }
    bool isNull() const { return type() == Type::null; }
    bool isBool() const { return type() == Type::boolean; }
    bool isInt() const { return type() == Type::integer; }
    bool isDouble() const { return type() == Type::real; }
    bool isNumber() const { return isInt() || isDouble(); }
    bool isString() const { return type() == Type::string; }
    bool isArray() const { return type() == Type::array; }
    bool isObject() const { return type() == Type::object; }

    // Lenient conversions with the same intent as the previous juce::var host:
    // missing values read as empty/zero/false rather than throwing.
    bool asBool() const;
    std::int64_t asInt() const;
    double asDouble() const;
    std::string asString() const;
    // Direct access for values already known to be strings.
    const std::string& string() const;

    const Array* getArray() const;
    Array* getArray();
    const Object* getObject() const;
    Object* getObject();

    // Missing keys and non-objects read as null.
    const Value& operator[](std::string_view key) const;
    bool has(std::string_view key) const;
    // Converts null to an object; throws for other non-object values.
    Value& set(std::string_view key, Value value);
    // Converts null to an array; throws for other non-array values.
    Value& push(Value value);
    // Element count for arrays and objects; zero otherwise.
    std::size_t size() const;

    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const { return !(*this == other); }

private:
    std::variant<std::monostate, bool, std::int64_t, double, std::string, Array, detail::Box<Object>, float> data_;
    friend std::string stringify(const Value&);
    friend std::string stringifyPretty(const Value&);
    friend void writeValue(std::string&, const Value&, int, int);
};

// Insertion-ordered object. Lookups are linear: protocol objects are small.
class Object {
public:
    using Entry = std::pair<std::string, Value>;
    Object() = default;
    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    // Replaces an existing key in place, otherwise appends it.
    Value& set(std::string_view key, Value value);
    bool remove(std::string_view key);
    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    std::vector<Entry>::iterator begin() { return entries_.begin(); }
    std::vector<Entry>::iterator end() { return entries_.end(); }
    std::vector<Entry>::const_iterator begin() const { return entries_.begin(); }
    std::vector<Entry>::const_iterator end() const { return entries_.end(); }
    bool operator==(const Object& other) const { return entries_ == other.entries_; }

private:
    std::vector<Entry> entries_;
};

namespace detail {
template <class T>
Box<T>::Box() : pointer_(std::make_unique<T>()) {}
template <class T>
Box<T>::Box(T value) : pointer_(std::make_unique<T>(std::move(value))) {}
template <class T>
Box<T>::Box(const Box& other) : pointer_(std::make_unique<T>(*other.pointer_)) {}
template <class T>
Box<T>::Box(Box&& other) noexcept : pointer_(std::move(other.pointer_)) {}
template <class T>
Box<T>& Box<T>::operator=(const Box& other) {
    if (this != &other) pointer_ = std::make_unique<T>(*other.pointer_);
    return *this;
}
template <class T>
Box<T>& Box<T>::operator=(Box&& other) noexcept {
    pointer_ = std::move(other.pointer_);
    return *this;
}
template <class T>
Box<T>::~Box() = default;
template <class T>
bool operator==(const Box<T>& a, const Box<T>& b) {
    return a.get() == b.get();
}
}  // namespace detail

const Value& null();

// Builder for literal objects: object({{"key", value}, ...}). Field values are
// moved into the object, so large strings are not copied twice.
struct Field {
    std::string_view key;
    mutable Value value;
    template <class T>
    Field(std::string_view name, T&& v) : key(name), value(std::forward<T>(v)) {}
};
Value object(std::initializer_list<Field> fields);
template <class T>
Value array(const std::vector<T>& values) {
    Array result;
    result.reserve(values.size());
    for (const auto& value : values) result.emplace_back(value);
    return Value(std::move(result));
}

inline const Value& get(const Value& value, std::string_view key) { return value[key]; }
inline std::string str(const Value& value, std::string_view key) { return value[key].asString(); }

struct ParseLimits {
    std::size_t maxBytes = 3 * 1024 * 1024;
    std::size_t maxDepth = 128;
    // Separators per container and in total, matching the previous host.
    std::size_t maxSeparatorsPerContainer = 4096;
    std::size_t maxSeparators = 32768;
};
// Strict RFC 8259 JSON. Throws std::runtime_error with a short message.
Value parse(std::string_view text, ParseLimits limits = {});
inline Value parseBounded(std::string_view text, std::size_t maxBytes) {
    ParseLimits limits;
    limits.maxBytes = maxBytes;
    return parse(text, limits);
}

// Compact UTF-8 JSON. Non-finite numbers serialize as null.
std::string stringify(const Value& value);
// Two-space indented JSON for files people may open.
std::string stringifyPretty(const Value& value);
std::string formatNumber(double value);
std::string formatFloat(float value);
void writeValue(std::string& out, const Value& value, int indent, int depth);
}  // namespace flows::json
