#pragma once

// Type-checked access to JSON that comes from outside: provider responses and streams, helper processes, cache and
// settings files. nlohmann's value()/get<>() throw when a field has another type than expected ({"id": null},
// {"index": "3"}); on a transport thread such an exception has nowhere to go. These accessors never throw: a
// missing or wrongly typed field is "absent", and callers decide whether that is an error (docs/ai.md, "Security").

#include <nlohmann/json.hpp>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace wabcut::ai {

using Json = nlohmann::json;

/// The member `key` of `j` when `j` is an object that has it (any type, null included), else nullptr.
const Json* jsonMember(const Json& j, std::string_view key);
/// The member when it is an object / an array, else nullptr.
const Json* jsonObject(const Json& j, std::string_view key);
const Json* jsonArray(const Json& j, std::string_view key);
/// Follows a path of object members: jsonAt(u, {"prompt_tokens_details", "cached_tokens"}).
const Json* jsonAt(const Json& j, std::initializer_list<std::string_view> path);

/// A string member.
std::optional<std::string> jsonString(const Json& j, std::string_view key);
std::string jsonStringOr(const Json& j, std::string_view key, std::string fallback = {});
/// An integer member: an integer, or a floating-point number with an integral value in range.
std::optional<std::int64_t> jsonInt(const Json& j, std::string_view key);
std::int64_t jsonIntOr(const Json& j, std::string_view key, std::int64_t fallback);
/// A finite number member.
std::optional<double> jsonNumber(const Json& j, std::string_view key);
double jsonNumberOr(const Json& j, std::string_view key, double fallback);
std::optional<bool> jsonBool(const Json& j, std::string_view key);
bool jsonBoolOr(const Json& j, std::string_view key, bool fallback);

/// The same for a value itself (an array element, a path's end).
std::optional<std::string> asString(const Json* j);
std::optional<std::int64_t> asInt(const Json* j);
std::optional<double> asNumber(const Json* j);

/// A string for an id/code field that providers send as a string or a number; empty otherwise.
std::string jsonIdOr(const Json& j, std::string_view key);

} // namespace wabcut::ai
