#include "Json.hpp"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace flows::json {
namespace {
// Number formatting and parsing must not follow a host-selected decimal
// separator. A per-thread C locale keeps output identical on every platform.
locale_t cLocale() {
    static const locale_t locale = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
    return locale;
}
struct ScopedCLocale {
    locale_t previous;
    ScopedCLocale() : previous(uselocale(cLocale())) {}
    ~ScopedCLocale() { uselocale(previous); }
};
double parseDouble(const std::string& text, bool& ok) {
    ScopedCLocale scope;
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    ok = end == text.c_str() + text.size() && std::isfinite(value);
    return value;
}
}  // namespace

const Value& null() {
    static const Value value;
    return value;
}

Value::Value() = default;
Value::Value(std::nullptr_t) {}
Value::Value(bool value) : data_(value) {}
Value::Value(int value) : data_(static_cast<std::int64_t>(value)) {}
Value::Value(unsigned value) : data_(static_cast<std::int64_t>(value)) {}
Value::Value(long value) : data_(static_cast<std::int64_t>(value)) {}
Value::Value(long long value) : data_(static_cast<std::int64_t>(value)) {}
Value::Value(double value) : data_(value) {}
Value::Value(float value) : data_(value) {}
Value::Value(const char* text) : data_(std::string(text ? text : "")) {}
Value::Value(std::string text) : data_(std::move(text)) {}
Value::Value(std::string_view text) : data_(std::string(text)) {}
Value::Value(Array values) : data_(std::move(values)) {}
Value::Value(Object values) : data_(detail::Box<Object>(std::move(values))) {}
Value::Value(const Value&) = default;
Value::Value(Value&&) noexcept = default;
Value& Value::operator=(const Value&) = default;
Value& Value::operator=(Value&&) noexcept = default;
Value::~Value() = default;
Value Value::array() { return Value(Array{}); }
Value Value::object() { return Value(Object{}); }
bool Value::operator==(const Value& other) const { return data_ == other.data_; }
const Array* Value::getArray() const { return std::get_if<Array>(&data_); }
Array* Value::getArray() { return std::get_if<Array>(&data_); }
const Object* Value::getObject() const {
    const auto* box = std::get_if<detail::Box<Object>>(&data_);
    return box ? &box->get() : nullptr;
}
Object* Value::getObject() {
    auto* box = std::get_if<detail::Box<Object>>(&data_);
    return box ? &box->get() : nullptr;
}
Value::Value(unsigned long value) : Value(static_cast<unsigned long long>(value)) {}
Value::Value(unsigned long long value) {
    if (value <= static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max()))
        data_ = static_cast<std::int64_t>(value);
    else
        data_ = static_cast<double>(value);
}

const Value* Object::find(std::string_view key) const {
    for (const auto& entry : entries_)
        if (entry.first == key) return &entry.second;
    return nullptr;
}
Value* Object::find(std::string_view key) {
    for (auto& entry : entries_)
        if (entry.first == key) return &entry.second;
    return nullptr;
}
Value& Object::set(std::string_view key, Value value) {
    if (auto* existing = find(key)) {
        *existing = std::move(value);
        return *existing;
    }
    entries_.emplace_back(std::string(key), std::move(value));
    return entries_.back().second;
}
bool Object::remove(std::string_view key) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
        if (it->first == key) {
            entries_.erase(it);
            return true;
        }
    return false;
}

bool Value::asBool() const {
    switch (type()) {
        case Type::null: return false;
        case Type::boolean: return std::get<bool>(data_);
        case Type::integer: return std::get<std::int64_t>(data_) != 0;
        case Type::real: return asDouble() != 0;
        case Type::string: {
            const auto& text = std::get<std::string>(data_);
            std::size_t start = 0, end = text.size();
            while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
            while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
            const auto trimmed = text.substr(start, end - start);
            if (trimmed.size() == 4) {
                std::string lower;
                for (char c : trimmed) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lower == "true") return true;
            }
            return asInt() != 0;
        }
        case Type::array:
        case Type::object: return true;
    }
    return false;
}
std::int64_t Value::asInt() const {
    switch (type()) {
        case Type::boolean: return std::get<bool>(data_) ? 1 : 0;
        case Type::integer: return std::get<std::int64_t>(data_);
        case Type::real: {
            const double value = asDouble();
            if (!std::isfinite(value)) return 0;
            if (value >= 9.2233720368547758e18) return std::numeric_limits<std::int64_t>::max();
            if (value <= -9.2233720368547758e18) return std::numeric_limits<std::int64_t>::min();
            return static_cast<std::int64_t>(value);
        }
        case Type::string: {
            // Leading optional sign and digits, like a lenient integer read.
            const auto& text = std::get<std::string>(data_);
            std::size_t i = 0;
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
            bool negative = false;
            if (i < text.size() && (text[i] == '-' || text[i] == '+')) negative = text[i++] == '-';
            std::int64_t result = 0;
            for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
                const int digit = text[i] - '0';
                if (result > (std::numeric_limits<std::int64_t>::max() - digit) / 10) break;
                result = result * 10 + digit;
            }
            return negative ? -result : result;
        }
        default: return 0;
    }
}
double Value::asDouble() const {
    switch (type()) {
        case Type::boolean: return std::get<bool>(data_) ? 1 : 0;
        case Type::integer: return static_cast<double>(std::get<std::int64_t>(data_));
        case Type::real: {
            if (const auto* single = std::get_if<float>(&data_)) return static_cast<double>(*single);
            return std::get<double>(data_);
        }
        case Type::string: {
            ScopedCLocale scope;
            return std::strtod(std::get<std::string>(data_).c_str(), nullptr);
        }
        default: return 0;
    }
}
std::string Value::asString() const {
    switch (type()) {
        case Type::boolean: return std::get<bool>(data_) ? "1" : "0";
        case Type::integer: return std::to_string(std::get<std::int64_t>(data_));
        case Type::real: {
            if (const auto* single = std::get_if<float>(&data_)) return formatFloat(*single);
            return formatNumber(std::get<double>(data_));
        }
        case Type::string: return std::get<std::string>(data_);
        default: return {};
    }
}
const std::string& Value::string() const {
    static const std::string empty;
    if (const auto* text = std::get_if<std::string>(&data_)) return *text;
    return empty;
}
const Value& Value::operator[](std::string_view key) const {
    if (const auto* object = getObject())
        if (const auto* value = object->find(key)) return *value;
    return null();
}
bool Value::has(std::string_view key) const {
    const auto* object = getObject();
    return object && object->contains(key);
}
Value& Value::set(std::string_view key, Value value) {
    if (isNull()) data_ = detail::Box<Object>();
    auto* object = getObject();
    if (!object) throw std::logic_error("JSON value is not an object");
    return object->set(key, std::move(value));
}
Value& Value::push(Value value) {
    if (isNull()) data_ = Array{};
    auto* array = getArray();
    if (!array) throw std::logic_error("JSON value is not an array");
    array->push_back(std::move(value));
    return array->back();
}
std::size_t Value::size() const {
    if (const auto* array = getArray()) return array->size();
    if (const auto* object = getObject()) return object->size();
    return 0;
}

Value object(std::initializer_list<Field> fields) {
    Object result;
    for (const auto& field : fields) result.set(field.key, std::move(field.value));
    return Value(std::move(result));
}

std::string formatFloat(float value) {
    if (!std::isfinite(value)) return "null";
    const double wide = static_cast<double>(value);
    if (wide == 0) return std::signbit(value) ? "-0.0" : "0.0";
    if (std::floor(wide) == wide && std::abs(wide) < 1e7) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%lld.0", static_cast<long long>(wide));
        return buffer;
    }
    ScopedCLocale scope;
    char buffer[32];
    // Shortest decimal that reads back as the same float.
    for (int precision = 6; precision <= 9; ++precision) {
        std::snprintf(buffer, sizeof buffer, "%.*g", precision, wide);
        if (std::strtof(buffer, nullptr) == value) break;
    }
    return buffer;
}
std::string formatNumber(double value) {
    if (!std::isfinite(value)) return "null";
    if (value == 0) return std::signbit(value) ? "-0.0" : "0.0";
    // Integral doubles keep a ".0" suffix (as JUCE wrote them), so a value
    // that was a double reads back as a double after a save and reload.
    if (std::floor(value) == value && std::abs(value) < 1e15) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%lld.0", static_cast<long long>(value));
        return buffer;
    }
    ScopedCLocale scope;
    char buffer[40];
    // Shortest of 15/16/17 significant digits that reads back exactly.
    for (int precision : {15, 16, 17}) {
        std::snprintf(buffer, sizeof buffer, "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value) break;
    }
    return buffer;
}

namespace {
void appendEscaped(std::string& out, std::string_view text) {
    out += '"';
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                    out += buffer;
                } else if (c == 0xe2 && i + 2 < text.size() &&
                           static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                           (static_cast<unsigned char>(text[i + 2]) == 0xa8 ||
                            static_cast<unsigned char>(text[i + 2]) == 0xa9)) {
                    // U+2028/U+2029 are valid JSON but break JavaScript string literals.
                    out += static_cast<unsigned char>(text[i + 2]) == 0xa8 ? "\\u2028" : "\\u2029";
                    i += 2;
                } else
                    out += static_cast<char>(c);
        }
    }
    out += '"';
}
}  // namespace
void writeValue(std::string& out, const Value& value, int indent, int depth) {
    const auto newline = [&](int level) {
        if (indent < 0) return;
        out += '\n';
        out.append(static_cast<std::size_t>(level * indent), ' ');
    };
    switch (value.type()) {
        case Value::Type::null: out += "null"; break;
        case Value::Type::boolean: out += value.asBool() ? "true" : "false"; break;
        case Value::Type::integer: out += std::to_string(value.asInt()); break;
        case Value::Type::real:
            if (const auto* single = std::get_if<float>(&value.data_))
                out += formatFloat(*single);
            else
                out += formatNumber(std::get<double>(value.data_));
            break;
        case Value::Type::string: appendEscaped(out, value.string()); break;
        case Value::Type::array: {
            const auto& array = *value.getArray();
            out += '[';
            bool first = true;
            for (const auto& item : array) {
                if (!first) out += ',';
                first = false;
                newline(depth + 1);
                writeValue(out, item, indent, depth + 1);
            }
            if (!array.empty()) newline(depth);
            out += ']';
            break;
        }
        case Value::Type::object: {
            const auto& object = *value.getObject();
            out += '{';
            bool first = true;
            for (const auto& [key, item] : object) {
                if (!first) out += ',';
                first = false;
                newline(depth + 1);
                appendEscaped(out, key);
                out += indent < 0 ? ":" : ": ";
                writeValue(out, item, indent, depth + 1);
            }
            if (!object.empty()) newline(depth);
            out += '}';
            break;
        }
    }
}

namespace {
class Parser {
public:
    Parser(std::string_view text, const ParseLimits& limits) : text_(text), limits_(limits) {}
    Value run() {
        skipSpace();
        auto value = parseValue(0);
        skipSpace();
        if (position_ != text_.size()) fail("Unexpected text after the JSON value");
        return value;
    }

private:
    std::string_view text_;
    const ParseLimits& limits_;
    std::size_t position_ = 0, separators_ = 0;

    [[noreturn]] void fail(const char* message) const {
        std::size_t line = 1, column = 1;
        for (std::size_t i = 0; i < position_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else
                ++column;
        }
        throw std::runtime_error("Line " + std::to_string(line) + ", column " +
                                 std::to_string(column) + ": " + message);
    }
    void skipSpace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++position_;
        }
    }
    bool consume(char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }
    void separator(std::size_t& local) {
        if (++local > limits_.maxSeparatorsPerContainer || ++separators_ > limits_.maxSeparators)
            throw std::runtime_error("JSON exceeds its value budget.");
    }
    Value parseValue(std::size_t depth) {
        if (position_ >= text_.size()) fail("Unexpected end of JSON");
        const char c = text_[position_];
        if (c == '{' || c == '[') {
            if (depth >= limits_.maxDepth) throw std::runtime_error("JSON nesting exceeds 128 levels.");
            return c == '{' ? parseObject(depth + 1) : parseArray(depth + 1);
        }
        if (c == '"') return Value(parseString());
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        if (text_.substr(position_, 4) == "true") {
            position_ += 4;
            return Value(true);
        }
        if (text_.substr(position_, 5) == "false") {
            position_ += 5;
            return Value(false);
        }
        if (text_.substr(position_, 4) == "null") {
            position_ += 4;
            return Value();
        }
        fail("Unexpected character");
    }
    Value parseObject(std::size_t depth) {
        ++position_;
        Object object;
        std::size_t local = 0;
        skipSpace();
        if (consume('}')) return Value(std::move(object));
        for (;;) {
            skipSpace();
            if (position_ >= text_.size() || text_[position_] != '"') fail("Expected a property name");
            auto key = parseString();
            skipSpace();
            if (!consume(':')) fail("Expected ':' after a property name");
            skipSpace();
            object.set(key, parseValue(depth));
            skipSpace();
            if (consume('}')) break;
            if (!consume(',')) fail("Expected ',' or '}'");
            separator(local);
        }
        return Value(std::move(object));
    }
    Value parseArray(std::size_t depth) {
        ++position_;
        Array array;
        std::size_t local = 0;
        skipSpace();
        if (consume(']')) return Value(std::move(array));
        for (;;) {
            skipSpace();
            array.push_back(parseValue(depth));
            skipSpace();
            if (consume(']')) break;
            if (!consume(',')) fail("Expected ',' or ']'");
            separator(local);
        }
        return Value(std::move(array));
    }
    unsigned hex4() {
        if (position_ + 4 > text_.size()) fail("Incomplete unicode escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[position_++];
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<unsigned>(c - 'A' + 10);
            else
                fail("Invalid unicode escape");
        }
        return value;
    }
    static void appendUtf8(std::string& out, unsigned code) {
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
    std::string parseString() {
        ++position_;
        std::string out;
        for (;;) {
            if (position_ >= text_.size()) fail("Unterminated string");
            const auto c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"') break;
            if (c < 0x20) fail("Control character in string");
            if (c != '\\') {
                out += static_cast<char>(c);
                continue;
            }
            if (position_ >= text_.size()) fail("Unterminated escape");
            const char escape = text_[position_++];
            switch (escape) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned code = hex4();
                    if (code >= 0xd800 && code <= 0xdbff) {
                        // A lone high surrogate becomes U+FFFD rather than invalid UTF-8.
                        if (position_ + 6 <= text_.size() && text_[position_] == '\\' &&
                            text_[position_ + 1] == 'u') {
                            const auto saved = position_;
                            position_ += 2;
                            const unsigned low = hex4();
                            if (low >= 0xdc00 && low <= 0xdfff)
                                code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                            else {
                                position_ = saved;
                                code = 0xfffd;
                            }
                        } else
                            code = 0xfffd;
                    } else if (code >= 0xdc00 && code <= 0xdfff)
                        code = 0xfffd;
                    appendUtf8(out, code);
                    break;
                }
                default: fail("Invalid escape");
            }
        }
        return out;
    }
    Value parseNumber() {
        const auto start = position_;
        bool real = false;
        consume('-');
        if (position_ >= text_.size()) fail("Invalid number");
        if (text_[position_] == '0')
            ++position_;
        else if (text_[position_] >= '1' && text_[position_] <= '9')
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
                ++position_;
        else
            fail("Invalid number");
        if (consume('.')) {
            real = true;
            if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9')
                fail("Invalid number");
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
                ++position_;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            real = true;
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-'))
                ++position_;
            if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9')
                fail("Invalid number");
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
                ++position_;
        }
        const std::string literal(text_.substr(start, position_ - start));
        if (!real) {
            errno = 0;
            char* end = nullptr;
            const long long value = std::strtoll(literal.c_str(), &end, 10);
            if (errno != ERANGE && end == literal.c_str() + literal.size())
                return Value(static_cast<std::int64_t>(value));
        }
        bool ok = false;
        const double value = parseDouble(literal, ok);
        if (!ok) fail("Number is out of range");
        return Value(value);
    }
};
}  // namespace

Value parse(std::string_view text, ParseLimits limits) {
    if (text.size() > limits.maxBytes) throw std::runtime_error("JSON exceeds its byte budget.");
    return Parser(text, limits).run();
}
std::string stringify(const Value& value) {
    std::string out;
    writeValue(out, value, -1, 0);
    return out;
}
std::string stringifyPretty(const Value& value) {
    std::string out;
    writeValue(out, value, 2, 0);
    return out;
}
}  // namespace flows::json
