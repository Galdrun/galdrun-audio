#include "TestSupport.h"

#include "util/Json.h"

#include <cstdio>

using namespace multiroom;

namespace {

json::Value parseOrFail(const std::string& text)
{
    json::Value value;
    std::string error;
    if (!json::parse(text, value, error))
        multiroomtest::failAt("parse failed for '" + text + "': " + error, __FILE__, __LINE__);
    return value;
}

void expectParseError(const std::string& text)
{
    json::Value value;
    std::string error;
    if (json::parse(text, value, error))
        multiroomtest::failAt("expected parse error for '" + text + "'", __FILE__, __LINE__);
}

} // namespace

MULTIROOM_TEST(json_parses_scalars)
{
    CHECK(parseOrFail("null").isNull());
    CHECK_EQ(parseOrFail("true").asBool(), true);
    CHECK_EQ(parseOrFail("false").asBool(true), false);
    CHECK_EQ(parseOrFail("0").asInt(), static_cast<int64_t>(0));
    CHECK_EQ(parseOrFail("-17").asInt(), static_cast<int64_t>(-17));
    CHECK_EQ(parseOrFail("48000").asInt(), static_cast<int64_t>(48000));
    CHECK_NEAR(parseOrFail("12.7").asDouble(), 12.7, 1e-12);
    CHECK_NEAR(parseOrFail("1.5e3").asDouble(), 1500.0, 1e-9);
    CHECK_EQ(parseOrFail("\"text\"").asString(), std::string("text"));
}

MULTIROOM_TEST(json_distinguishes_integers_from_reals)
{
    CHECK(parseOrFail("48000").isIntegral());
    CHECK(!parseOrFail("48000.0").isIntegral());
    // Integers round-trip without gaining a fractional part.
    CHECK_EQ(json::serialize(parseOrFail("48000")), std::string("48000"));
}

MULTIROOM_TEST(json_parses_string_escapes)
{
    CHECK_EQ(parseOrFail("\"a\\nb\"").asString(), std::string("a\nb"));
    CHECK_EQ(parseOrFail("\"q\\\"q\"").asString(), std::string("q\"q"));
    CHECK_EQ(parseOrFail("\"back\\\\slash\"").asString(), std::string("back\\slash"));
    CHECK_EQ(parseOrFail("\"\\u0041\"").asString(), std::string("A"));
    // U+00E4 -> two UTF-8 bytes
    CHECK_EQ(parseOrFail("\"\\u00e4\"").asString(), std::string("\xC3\xA4"));
    // Surrogate pair for U+1F50A
    CHECK_EQ(parseOrFail("\"\\ud83d\\udd0a\"").asString(), std::string("\xF0\x9F\x94\x8A"));
}

MULTIROOM_TEST(json_parses_objects_and_arrays)
{
    const json::Value root = parseOrFail(
        "{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":\"e\"},\"f\":[],\"g\":{}}");

    CHECK(root.isObject());
    CHECK_EQ(root.find("a")->asInt(), static_cast<int64_t>(1));

    const json::Value* b = root.find("b");
    CHECK(b != nullptr && b->isArray());
    CHECK_EQ(b->size(), static_cast<size_t>(3));
    CHECK_EQ(b->at(2).asInt(), static_cast<int64_t>(3));

    const json::Value* c = root.find("c");
    CHECK(c != nullptr && c->isObject());
    CHECK_EQ(c->find("d")->asString(), std::string("e"));

    CHECK_EQ(root.find("f")->size(), static_cast<size_t>(0));
    CHECK(root.find("g")->isObject());
    CHECK(root.find("missing") == nullptr);
}

MULTIROOM_TEST(json_rejects_malformed_input)
{
    expectParseError("");
    expectParseError("{");
    expectParseError("[1,2");
    expectParseError("{\"a\":1,}");
    expectParseError("[1,]");
    expectParseError("{'a':1}");
    expectParseError("nul");
    expectParseError("01");
    expectParseError("1.");
    expectParseError(".5");
    expectParseError("1e");
    expectParseError("\"unterminated");
    expectParseError("\"bad\\xescape\"");
    expectParseError("\"\\ud83d\"");   // unpaired high surrogate
    expectParseError("{\"a\":1} junk"); // trailing garbage
}

MULTIROOM_TEST(json_rejects_excessive_nesting)
{
    std::string text;
    for (int i = 0; i < 200; ++i)
        text += "[";
    for (int i = 0; i < 200; ++i)
        text += "]";
    expectParseError(text);
}

MULTIROOM_TEST(json_rejects_duplicate_object_keys)
{
    expectParseError("{\"a\":1,\"a\":2}");
    expectParseError("{\"a\":1,\"b\":2,\"a\":3}");
    expectParseError("{\"o\":{\"x\":1,\"x\":1}}");
    // The same key in different objects is not a duplicate.
    parseOrFail("{\"a\":{\"a\":1},\"b\":{\"a\":2}}");
}

MULTIROOM_TEST(json_parses_a_large_object_in_linear_time)
{
    // About a megabyte of short keys, the size limit of a MATP message. With
    // a lookup per inserted key this took minutes on the ARMv6 target.
    const int count = 100000;
    std::string text = "{";
    char key[16];
    for (int i = 0; i < count; ++i)
    {
        std::snprintf(key, sizeof(key), "\"%06d\":0,", i);
        text += key;
    }
    text.back() = '}';

    const json::Value root = parseOrFail(text);
    CHECK_EQ(root.members().size(), static_cast<size_t>(count));
    CHECK_EQ(root.members().front().first, std::string("000000"));
    CHECK(root.find("099999") != nullptr);
}

MULTIROOM_TEST(json_serializes_compactly_and_preserves_order)
{
    json::Value root = json::Value::makeObject();
    root["z"] = json::Value(1);
    root["a"] = json::Value("two");
    root["n"] = json::Value(true);

    CHECK_EQ(json::serialize(root), std::string("{\"z\":1,\"a\":\"two\",\"n\":true}"));
}

MULTIROOM_TEST(json_escapes_on_serialization)
{
    json::Value root = json::Value::makeObject();
    root["k"] = json::Value(std::string("line\nquote\"tab\t"));
    CHECK_EQ(json::serialize(root), std::string("{\"k\":\"line\\nquote\\\"tab\\t\"}"));

    json::Value control = json::Value::makeObject();
    control["k"] = json::Value(std::string("\x01"));
    CHECK_EQ(json::serialize(control), std::string("{\"k\":\"\\u0001\"}"));
}

MULTIROOM_TEST(json_round_trips_doubles)
{
    const double values[] = {12.7, -0.0000125, 48000.5, 1.0 / 3.0, 1e-9, 123456789.123};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    {
        json::Value original(values[i]);
        const std::string text = json::serialize(original);
        json::Value reparsed;
        std::string error;
        CHECK(json::parse(text, reparsed, error));
        CHECK_EQ(reparsed.asDouble(), values[i]);
    }
}

MULTIROOM_TEST(json_pretty_output_reparses)
{
    json::Value root = json::Value::makeObject();
    root["devices"] = json::Value::makeArray();
    json::Value device = json::Value::makeObject();
    device["id"] = json::Value("hw:CARD=Device,DEV=0");
    device["rate"] = json::Value(48000);
    root["devices"].pushBack(device);

    const std::string pretty = json::serialize(root, true);
    CHECK(pretty.find('\n') != std::string::npos);

    json::Value reparsed;
    std::string error;
    CHECK(json::parse(pretty, reparsed, error));
    CHECK_EQ(reparsed.find("devices")->at(0).find("id")->asString(),
             std::string("hw:CARD=Device,DEV=0"));
}

MULTIROOM_TEST(json_typed_accessors_reject_wrong_types)
{
    const json::Value root = parseOrFail("{\"s\":\"x\",\"i\":5,\"b\":true}");

    std::string text;
    int64_t number = 0;
    bool flag = false;

    CHECK(json::getString(root, "s", text));
    CHECK_EQ(text, std::string("x"));
    CHECK(!json::getString(root, "i", text));
    CHECK(!json::getInt(root, "s", number));
    CHECK(json::getInt(root, "i", number));
    CHECK_EQ(number, static_cast<int64_t>(5));
    CHECK(!json::getBool(root, "i", flag));
    CHECK(json::getBool(root, "b", flag));
    CHECK_EQ(flag, true);
    CHECK(!json::getString(root, "missing", text));
}

MULTIROOM_TEST(json_numbers_outside_int64_are_not_integers)
{
    // Kept as doubles by the parser, and converting one of those to int64_t
    // unchecked is undefined behaviour.
    CHECK_EQ(parseOrFail("1e30").asInt(-1), static_cast<int64_t>(-1));
    CHECK_EQ(parseOrFail("-1e30").asInt(-1), static_cast<int64_t>(-1));
    CHECK_EQ(parseOrFail("99999999999999999999").asInt(-1), static_cast<int64_t>(-1));
    CHECK_EQ(parseOrFail("12.7").asInt(), static_cast<int64_t>(12));

    // A key of the wrong type is an error, and a fraction is the wrong type.
    const json::Value root = parseOrFail("{\"port\":4720.5,\"big\":1e30,\"whole\":4720.0}");
    int64_t number = 7;
    CHECK(!json::getInt(root, "port", number));
    CHECK(!json::getInt(root, "big", number));
    CHECK(!json::getInt(root, "whole", number));
    CHECK_EQ(number, static_cast<int64_t>(7));
}

MULTIROOM_TEST(json_uint64_above_int64_is_not_an_integer)
{
    // The same answer the parser gives for the same number, not a wrap to
    // a negative "exact" integer.
    const json::Value big(static_cast<uint64_t>(INT64_MAX) + 1);
    CHECK(big.isNumber());
    CHECK(!big.isIntegral());
    CHECK_EQ(big.asInt(-1), static_cast<int64_t>(-1));
    CHECK(big.asDouble() > 0.0);

    const json::Value edge(static_cast<uint64_t>(INT64_MAX));
    CHECK(edge.isIntegral());
    CHECK_EQ(edge.asInt(), static_cast<int64_t>(INT64_MAX));
}

MULTIROOM_TEST(json_rejects_numbers_that_do_not_fit_a_double)
{
    // strtod gives HUGE_VAL, which would serialize back as null.
    expectParseError("1e999");
    expectParseError("-1e999");
    expectParseError("{\"prebufferMs\":1e999}");
}
