// The JSON Schema subset used for command params.

#include <wabcut/command/Schema.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut::command;

namespace {

std::string firstIssue(const Json& schema, const Json& value) {
    const auto issues = validateAgainstSchema(schema, value);
    return issues.empty() ? std::string() : issues.front().path + " " + issues.front().message;
}

} // namespace

TEST_CASE("Types, including type lists and integral numbers", "[command][schema]") {
    CHECK(firstIssue(Json{{"type", "integer"}}, 3).empty());
    CHECK(firstIssue(Json{{"type", "integer"}}, 3.0).empty());
    CHECK(firstIssue(Json{{"type", "integer"}}, 3.5) == "/ expected integer, got number");
    CHECK(firstIssue(Json{{"type", "number"}}, 3).empty());
    CHECK(firstIssue(Json{{"type", Json::array({"integer", "string"})}}, "00:00:01:00").empty());
    CHECK(firstIssue(Json{{"type", Json::array({"integer", "string"})}}, true) == "/ expected integer or string, got boolean");
    CHECK(firstIssue(Json{{"type", "null"}}, nullptr).empty());
    CHECK(firstIssue(Json{{"type", "boolean"}}, 0) == "/ expected boolean, got integer");
}

TEST_CASE("Ranges, lengths, enums and const", "[command][schema]") {
    const Json range = schema::integer("", 0, 15);
    CHECK(firstIssue(range, 0).empty());
    CHECK(firstIssue(range, 15).empty());
    CHECK(firstIssue(range, 16) == "/ must be at most 15");
    CHECK(firstIssue(range, -1) == "/ must be at least 0");
    CHECK(firstIssue(Json{{"type", "integer"}, {"exclusiveMinimum", 0}}, 0) == "/ must be greater than 0");
    CHECK(firstIssue(Json{{"type", "number"}, {"exclusiveMaximum", 1}}, 0.5).empty());
    // 64-bit values compare exactly.
    CHECK(firstIssue(Json{{"type", "integer"}, {"maximum", 9007199254740993LL}}, 9007199254740993LL).empty());
    CHECK_FALSE(firstIssue(Json{{"type", "integer"}, {"maximum", 9007199254740992LL}}, 9007199254740993LL).empty());
    CHECK(firstIssue(Json{{"type", "integer"}, {"minimum", 1}}, std::uint64_t{18446744073709551615ULL}).empty());
    CHECK(firstIssue(Json{{"type", "string"}, {"minLength", 2}, {"maxLength", 3}}, "a") ==
          "/ must be at least 2 characters long");
    CHECK(firstIssue(schema::enumeration({"head", "tail"}, ""), "middle") == "/ must be one of [\"head\",\"tail\"]");
    CHECK(firstIssue(Json{{"const", 7}}, 7).empty());
    CHECK_FALSE(firstIssue(Json{{"const", 7}}, 8).empty());
}

TEST_CASE("Objects and arrays report JSON-pointer paths", "[command][schema]") {
    const Json s = schema::object(
        {{"clips", schema::array(schema::object({{"track", schema::string("")}, {"time", schema::integer("")}},
                                                {"track", "time"}),
                                 "", 1)},
         {"a/b", schema::boolean("")}},
        {"clips"});
    CHECK(firstIssue(s, Json{{"clips", Json::array({Json{{"track", "V1"}, {"time", 3}}})}}).empty());
    CHECK(firstIssue(s, Json::object()) == "/ is missing required \"clips\"");
    CHECK(firstIssue(s, Json{{"clips", Json::array()}}) == "/clips needs at least 1 items");
    CHECK(firstIssue(s, Json{{"clips", Json::array({Json{{"track", "V1"}, {"time", "x"}}})}}) ==
          "/clips/0/time expected integer, got string");
    CHECK(firstIssue(s, Json{{"clips", Json::array({Json{{"track", "V1"}}})}}) ==
          "/clips/0 is missing required \"time\"");
    CHECK(firstIssue(s, Json{{"clips", Json::array({Json{{"track", "V1"}, {"time", 1}}})}, {"extra", 1}}) ==
          "/extra is not a known parameter");
    CHECK(firstIssue(s, Json{{"clips", Json::array({Json{{"track", "V1"}, {"time", 1}}})}, {"a/b", 1}}) ==
          "/a~1b expected boolean, got integer");
    // Every problem is listed, not only the first.
    CHECK(validateAgainstSchema(s, Json{{"x", 1}, {"y", 2}}).size() == 3);
}

TEST_CASE("Keywords outside the subset are reported", "[command][schema]") {
    CHECK(unsupportedSchemaKeywords(schema::object({{"a", schema::integer("", 1)}}, {"a"}, "d")).empty());
    const Json s{{"type", "object"},
                 {"properties", Json{{"a", Json{{"type", "string"}, {"pattern", "^x"}}}}},
                 {"oneOf", Json::array()}};
    CHECK(unsupportedSchemaKeywords(s) == std::vector<std::string>{"oneOf", "pattern"});
}
