#pragma once

// The JSON Schema subset command parameters are described with (draft 2020-12 keywords).
//
// Supported: type (a name or a list of names: object, array, string, integer, number, boolean, null),
// properties, required, additionalProperties (boolean), items, enum, const, minimum, maximum,
// exclusiveMinimum, exclusiveMaximum, minLength, maxLength, minItems, maxItems, and the annotations
// title, description, default and examples. Keywords apply only to values of the type they are about
// (as in JSON Schema), so {"type": ["integer", "object"], "required": [...]} requires the members only
// when the value is an object. The same schemas go into LLM tool definitions unchanged.

#include <wabcut/command/Command.h>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wabcut::command {

inline constexpr const char* kJsonSchemaDialect = "https://json-schema.org/draft/2020-12/schema";

struct SchemaIssue {
    std::string path;    ///< JSON Pointer into the value, e.g. "/clips/0/track"
    std::string message; ///< what is wrong, in English
};

/// Checks `value` against `schema`; returns every problem found (empty = valid).
std::vector<SchemaIssue> validateAgainstSchema(const Json& schema, const Json& value);

/// Keywords in `schema` (recursively) outside the supported subset; empty for a usable schema.
std::vector<std::string> unsupportedSchemaKeywords(const Json& schema);

/// Small builders for parameter schemas.
namespace schema {

Json object(std::initializer_list<std::pair<const char*, Json>> properties, std::vector<std::string> required = {},
            std::string description = {});
Json string(std::string description);
Json integer(std::string description, std::optional<std::int64_t> minimum = std::nullopt,
             std::optional<std::int64_t> maximum = std::nullopt);
Json number(std::string description, std::optional<double> minimum = std::nullopt,
            std::optional<double> maximum = std::nullopt);
Json boolean(std::string description, std::optional<bool> defaultValue = std::nullopt);
Json array(Json items, std::string description, std::optional<std::int64_t> minItems = std::nullopt);
Json enumeration(std::vector<std::string> values, std::string description);
/// Adds or replaces the "description".
Json described(Json schema, std::string description);

} // namespace schema

} // namespace wabcut::command
