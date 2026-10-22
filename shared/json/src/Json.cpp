#include "util/Json.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace multiroom {
namespace json {
namespace {

const Value& nullValue()
{
    static const Value kNull;
    return kNull;
}

// Guards against stack exhaustion from deeply nested hostile input.
const int kMaxDepth = 64;

} // namespace

class Parser
{
public:
    explicit Parser(const std::string& text) : text_(text) {}

    bool parseDocument(Value& out, std::string& error)
    {
        skipWhitespace();
        Value value;
        if (!parseValue(value, 0))
        {
            error = error_;
            return false;
        }
        skipWhitespace();
        if (pos_ != text_.size())
        {
            fail("trailing characters after JSON value");
            error = error_;
            return false;
        }
        out = std::move(value);
        return true;
    }

private:
    const std::string& text_;
    size_t pos_ = 0;
    std::string error_;

    bool atEnd() const { return pos_ >= text_.size(); }
    char peek() const { return text_[pos_]; }

    void fail(const char* message)
    {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%s at offset %lu", message,
                      static_cast<unsigned long>(pos_));
        error_ = buffer;
    }

    void skipWhitespace()
    {
        while (!atEnd())
        {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }

    bool expect(char c)
    {
        if (atEnd() || peek() != c)
        {
            char message[32];
            std::snprintf(message, sizeof(message), "expected '%c'", c);
            fail(message);
            return false;
        }
        ++pos_;
        return true;
    }

    bool literal(const char* word, Value value, Value& out)
    {
        const size_t length = std::strlen(word);
        if (text_.compare(pos_, length, word) != 0)
        {
            fail("invalid literal");
            return false;
        }
        pos_ += length;
        out = std::move(value);
        return true;
    }

    bool parseValue(Value& out, int depth)
    {
        if (depth > kMaxDepth)
        {
            fail("maximum nesting depth exceeded");
            return false;
        }
        if (atEnd())
        {
            fail("unexpected end of input");
            return false;
        }

        switch (peek())
        {
        case '{':
            return parseObject(out, depth);
        case '[':
            return parseArray(out, depth);
        case '"':
        {
            std::string s;
            if (!parseString(s))
                return false;
            out = Value(std::move(s));
            return true;
        }
        case 't':
            return literal("true", Value(true), out);
        case 'f':
            return literal("false", Value(false), out);
        case 'n':
            return literal("null", Value(), out);
        default:
            return parseNumber(out);
        }
    }

    // Sorted rather than hashed: std::hash is unseeded, so a sender could
    // pick colliding keys and make a hash set quadratic again.
    static bool hasDuplicateKey(const Value& object)
    {
        std::vector<const std::string*> keys;
        keys.reserve(object.members_.size());
        for (const Value::Member& member : object.members_)
            keys.push_back(&member.first);
        std::sort(keys.begin(), keys.end(),
                  [](const std::string* a, const std::string* b) { return *a < *b; });
        for (size_t i = 1; i < keys.size(); ++i)
        {
            if (*keys[i - 1] == *keys[i])
                return true;
        }
        return false;
    }

    bool parseObject(Value& out, int depth)
    {
        if (!expect('{'))
            return false;
        Value object = Value::makeObject();
        skipWhitespace();
        if (!atEnd() && peek() == '}')
        {
            ++pos_;
            out = std::move(object);
            return true;
        }
        for (;;)
        {
            skipWhitespace();
            std::string key;
            if (!parseString(key))
                return false;
            skipWhitespace();
            if (!expect(':'))
                return false;
            skipWhitespace();
            Value member;
            if (!parseValue(member, depth + 1))
                return false;
            // Appended, not operator[]: a lookup per key makes an object
            // quadratic in its size, and a 1 MiB message of short keys would
            // hold the owner thread for minutes. Duplicates are caught once
            // the object is complete.
            object.members_.push_back(Value::Member(std::move(key), std::move(member)));
            skipWhitespace();
            if (atEnd())
            {
                fail("unterminated object");
                return false;
            }
            if (peek() == ',')
            {
                ++pos_;
                continue;
            }
            if (peek() == '}')
            {
                if (hasDuplicateKey(object))
                {
                    fail("duplicate object key");
                    return false;
                }
                ++pos_;
                out = std::move(object);
                return true;
            }
            fail("expected ',' or '}'");
            return false;
        }
    }

    bool parseArray(Value& out, int depth)
    {
        if (!expect('['))
            return false;
        Value array = Value::makeArray();
        skipWhitespace();
        if (!atEnd() && peek() == ']')
        {
            ++pos_;
            out = std::move(array);
            return true;
        }
        for (;;)
        {
            skipWhitespace();
            Value element;
            if (!parseValue(element, depth + 1))
                return false;
            array.pushBack(std::move(element));
            skipWhitespace();
            if (atEnd())
            {
                fail("unterminated array");
                return false;
            }
            if (peek() == ',')
            {
                ++pos_;
                continue;
            }
            if (peek() == ']')
            {
                ++pos_;
                out = std::move(array);
                return true;
            }
            fail("expected ',' or ']'");
            return false;
        }
    }

    bool parseHex4(uint32_t& out)
    {
        if (pos_ + 4 > text_.size())
        {
            fail("truncated unicode escape");
            return false;
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = text_[pos_ + static_cast<size_t>(i)];
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<uint32_t>(c - 'A' + 10);
            else
            {
                fail("invalid hex digit in unicode escape");
                return false;
            }
        }
        pos_ += 4;
        out = value;
        return true;
    }

    static void appendUtf8(std::string& out, uint32_t cp)
    {
        if (cp < 0x80)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parseString(std::string& out)
    {
        if (!expect('"'))
            return false;
        std::string result;
        for (;;)
        {
            if (atEnd())
            {
                fail("unterminated string");
                return false;
            }
            const char c = text_[pos_++];
            if (c == '"')
            {
                out = std::move(result);
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20)
            {
                --pos_;
                fail("unescaped control character in string");
                return false;
            }
            if (c != '\\')
            {
                result.push_back(c);
                continue;
            }
            if (atEnd())
            {
                fail("unterminated escape sequence");
                return false;
            }
            const char escape = text_[pos_++];
            switch (escape)
            {
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            case '/':
                result.push_back('/');
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u':
            {
                uint32_t unit = 0;
                if (!parseHex4(unit))
                    return false;
                if (unit >= 0xD800 && unit <= 0xDBFF)
                {
                    // High surrogate: a low surrogate must follow.
                    if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u')
                    {
                        fail("unpaired high surrogate");
                        return false;
                    }
                    pos_ += 2;
                    uint32_t low = 0;
                    if (!parseHex4(low))
                        return false;
                    if (low < 0xDC00 || low > 0xDFFF)
                    {
                        fail("invalid low surrogate");
                        return false;
                    }
                    unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                }
                else if (unit >= 0xDC00 && unit <= 0xDFFF)
                {
                    fail("unpaired low surrogate");
                    return false;
                }
                appendUtf8(result, unit);
                break;
            }
            default:
                --pos_;
                fail("invalid escape sequence");
                return false;
            }
        }
    }

    bool parseNumber(Value& out)
    {
        const size_t start = pos_;
        if (!atEnd() && peek() == '-')
            ++pos_;

        if (atEnd() || peek() < '0' || peek() > '9')
        {
            fail("invalid number");
            return false;
        }
        if (peek() == '0')
        {
            ++pos_;
        }
        else
        {
            while (!atEnd() && peek() >= '0' && peek() <= '9')
                ++pos_;
        }

        bool integral = true;
        if (!atEnd() && peek() == '.')
        {
            integral = false;
            ++pos_;
            if (atEnd() || peek() < '0' || peek() > '9')
            {
                fail("missing digits after decimal point");
                return false;
            }
            while (!atEnd() && peek() >= '0' && peek() <= '9')
                ++pos_;
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E'))
        {
            integral = false;
            ++pos_;
            if (!atEnd() && (peek() == '+' || peek() == '-'))
                ++pos_;
            if (atEnd() || peek() < '0' || peek() > '9')
            {
                fail("missing digits in exponent");
                return false;
            }
            while (!atEnd() && peek() >= '0' && peek() <= '9')
                ++pos_;
        }

        const std::string token = text_.substr(start, pos_ - start);
        if (integral)
        {
            errno = 0;
            char* end = nullptr;
            const long long parsed = std::strtoll(token.c_str(), &end, 10);
            if (errno == 0 && end != nullptr && *end == '\0')
            {
                out = Value(static_cast<int64_t>(parsed));
                return true;
            }
            // Outside int64 range: keep it as a double instead of failing.
        }

        char* end = nullptr;
        const double parsed = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0')
        {
            pos_ = start;
            fail("invalid number");
            return false;
        }
        // 1e999 comes back as HUGE_VAL, which JSON cannot write back: it
        // would serialize as null.
        if (!std::isfinite(parsed))
        {
            pos_ = start;
            fail("number out of range");
            return false;
        }
        out = Value(parsed);
        return true;
    }
};

namespace {

void escapeTo(std::string& out, const std::string& text)
{
    out.push_back('"');
    for (size_t i = 0; i < text.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                out += buffer;
            }
            else
            {
                // UTF-8 bytes are emitted verbatim.
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
}

void appendDouble(std::string& out, double value)
{
    if (std::isnan(value) || std::isinf(value))
    {
        // JSON has no NaN/Infinity; emit null rather than invalid output.
        out += "null";
        return;
    }
    char buffer[40];
    // Shortest representation that still round-trips exactly.
    for (int precision = 15; precision <= 17; ++precision)
    {
        std::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
        if (std::strtod(buffer, nullptr) == value)
            break;
    }
    out += buffer;
}

void serializeTo(std::string& out, const Value& value, bool pretty, int depth)
{
    const char* const newline = pretty ? "\n" : "";

    switch (value.type())
    {
    case Type::Null:
        out += "null";
        return;
    case Type::Bool:
        out += value.asBool() ? "true" : "false";
        return;
    case Type::Number:
        if (value.isIntegral())
        {
            char buffer[24];
            std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value.asInt()));
            out += buffer;
        }
        else
        {
            appendDouble(out, value.asDouble());
        }
        return;
    case Type::String:
        escapeTo(out, value.asString());
        return;
    case Type::Array:
    {
        if (value.size() == 0)
        {
            out += "[]";
            return;
        }
        out += "[";
        out += newline;
        for (size_t i = 0; i < value.size(); ++i)
        {
            if (pretty)
                out.append(static_cast<size_t>(depth + 1) * 2, ' ');
            serializeTo(out, value.at(i), pretty, depth + 1);
            if (i + 1 < value.size())
                out += ",";
            out += newline;
        }
        if (pretty)
            out.append(static_cast<size_t>(depth) * 2, ' ');
        out += "]";
        return;
    }
    case Type::Object:
    {
        const std::vector<Value::Member>& members = value.members();
        if (members.empty())
        {
            out += "{}";
            return;
        }
        out += "{";
        out += newline;
        for (size_t i = 0; i < members.size(); ++i)
        {
            if (pretty)
                out.append(static_cast<size_t>(depth + 1) * 2, ' ');
            escapeTo(out, members[i].first);
            out += pretty ? ": " : ":";
            serializeTo(out, members[i].second, pretty, depth + 1);
            if (i + 1 < members.size())
                out += ",";
            out += newline;
        }
        if (pretty)
            out.append(static_cast<size_t>(depth) * 2, ' ');
        out += "}";
        return;
    }
    }
}

} // namespace

Value Value::makeArray()
{
    Value v;
    v.type_ = Type::Array;
    return v;
}

Value Value::makeObject()
{
    Value v;
    v.type_ = Type::Object;
    return v;
}

bool Value::asBool(bool fallback) const
{
    return type_ == Type::Bool ? bool_ : fallback;
}

double Value::asDouble(double fallback) const
{
    if (type_ != Type::Number)
        return fallback;
    return integral_ ? static_cast<double>(integer_) : number_;
}

int64_t Value::asInt(int64_t fallback) const
{
    if (type_ != Type::Number)
        return fallback;
    if (integral_)
        return integer_;
    // Converting a double outside int64's range is undefined behaviour. The
    // bounds are exact powers of two, so the comparison itself is exact.
    if (!(number_ >= -9223372036854775808.0 && number_ < 9223372036854775808.0))
        return fallback;
    return static_cast<int64_t>(number_);
}

std::string Value::asString(const std::string& fallback) const
{
    return type_ == Type::String ? string_ : fallback;
}

size_t Value::size() const
{
    if (type_ == Type::Array)
        return elements_.size();
    if (type_ == Type::Object)
        return members_.size();
    return 0;
}

const Value& Value::at(size_t index) const
{
    if (type_ != Type::Array || index >= elements_.size())
        return nullValue();
    return elements_[index];
}

void Value::pushBack(Value v)
{
    if (type_ != Type::Array)
        *this = makeArray();
    elements_.push_back(std::move(v));
}

const Value* Value::find(const std::string& key) const
{
    if (type_ != Type::Object)
        return nullptr;
    for (size_t i = 0; i < members_.size(); ++i)
    {
        if (members_[i].first == key)
            return &members_[i].second;
    }
    return nullptr;
}

Value& Value::operator[](const std::string& key)
{
    if (type_ != Type::Object)
        *this = makeObject();
    for (size_t i = 0; i < members_.size(); ++i)
    {
        if (members_[i].first == key)
            return members_[i].second;
    }
    members_.push_back(Member(key, Value()));
    return members_.back().second;
}

bool parse(const std::string& text, Value& out, std::string& error)
{
    Parser parser(text);
    return parser.parseDocument(out, error);
}

std::string serialize(const Value& value, bool pretty)
{
    std::string out;
    serializeTo(out, value, pretty, 0);
    if (pretty)
        out.push_back('\n');
    return out;
}

bool getString(const Value& object, const char* key, std::string& out)
{
    const Value* v = object.find(key);
    if (v == nullptr || !v->isString())
        return false;
    out = v->asString();
    return true;
}

bool getInt(const Value& object, const char* key, int64_t& out)
{
    const Value* v = object.find(key);
    // 4720.5 is a number of the wrong type, not 4720.
    if (v == nullptr || !v->isIntegral())
        return false;
    out = v->asInt();
    return true;
}

bool getDouble(const Value& object, const char* key, double& out)
{
    const Value* v = object.find(key);
    if (v == nullptr || !v->isNumber())
        return false;
    out = v->asDouble();
    return true;
}

bool getBool(const Value& object, const char* key, bool& out)
{
    const Value* v = object.find(key);
    if (v == nullptr || !v->isBool())
        return false;
    out = v->asBool();
    return true;
}

} // namespace json
} // namespace multiroom
