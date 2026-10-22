#pragma once

// Minimal JSON value / parser / serializer.
//
// Scope is deliberately small: the client only reads and writes flat
// configuration documents and one-line control messages. No streaming API,
// no comments, no trailing commas -- strict RFC 8259.
//
// Locale note: parsing and serialization of non-integral numbers use strtod()
// and snprintf(), which honour LC_NUMERIC. The process must therefore never
// call setlocale(LC_NUMERIC, ...) with a locale that uses a decimal comma.
// The client never calls setlocale at all, so the default "C" locale applies.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace multiroom {
namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Parser;

class Value
{
public:
    using Member = std::pair<std::string, Value>;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : type_(Type::Bool), bool_(v) {}
    Value(double v) : type_(Type::Number), number_(v), integral_(false) {}
    Value(int64_t v) : type_(Type::Number), number_(static_cast<double>(v)), integer_(v), integral_(true) {}
    Value(int32_t v) : Value(static_cast<int64_t>(v)) {}
    Value(uint32_t v) : Value(static_cast<int64_t>(v)) {}
    // Above int64's range it becomes a double, as parse() treats the same
    // number, rather than wrapping into a negative integer.
    Value(uint64_t v)
        : type_(Type::Number), number_(static_cast<double>(v)),
          integer_(v <= INT64_MAX ? static_cast<int64_t>(v) : 0), integral_(v <= INT64_MAX) {}
    Value(const char* v) : type_(Type::String), string_(v ? v : "") {}
    Value(std::string v) : type_(Type::String), string_(std::move(v)) {}

    static Value makeArray();
    static Value makeObject();

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    // True if the number was written without fraction/exponent and therefore
    // round-trips exactly through int64_t.
    bool isIntegral() const { return type_ == Type::Number && integral_; }

    bool asBool(bool fallback = false) const;
    double asDouble(double fallback = 0.0) const;
    // Truncates a non-integral number; one outside int64's range gives the
    // fallback.
    int64_t asInt(int64_t fallback = 0) const;
    std::string asString(const std::string& fallback = std::string()) const;

    // Array access. at() returns a null value reference when out of range.
    size_t size() const;
    const Value& at(size_t index) const;
    void pushBack(Value v);

    // Object access. Insertion order is preserved so serialized documents are
    // stable across runs.
    const Value* find(const std::string& key) const;
    Value& operator[](const std::string& key);
    const std::vector<Member>& members() const { return members_; }

private:
    // Appends object members directly; see parseObject.
    friend class Parser;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    int64_t integer_ = 0;
    bool integral_ = false;
    std::string string_;
    std::vector<Value> elements_;
    std::vector<Member> members_;
};

// Parses a complete document. Trailing non-whitespace is an error.
// On failure, out is left untouched and error describes the problem.
bool parse(const std::string& text, Value& out, std::string& error);

// pretty = two-space indentation and a trailing newline.
std::string serialize(const Value& value, bool pretty = false);

// Typed object field accessors. Return false if the key is missing or has a
// different type; out is then untouched. getInt accepts integral numbers only.
bool getString(const Value& object, const char* key, std::string& out);
bool getInt(const Value& object, const char* key, int64_t& out);
bool getDouble(const Value& object, const char* key, double& out);
bool getBool(const Value& object, const char* key, bool& out);

} // namespace json
} // namespace multiroom
