#pragma once

// The portable JSON-Schema subset for tool parameters and structured output, and its per-provider rewrites.
//
// Tool specs (Chat.h) and ResponseFormat schemas are written once in the PORTABLE subset; adapters call
// adaptSchema() for their wire format. The subset is what the command catalog (wabcut::command::schema) produces:
//
//   type (a name, or an array of names to allow null), properties, required, additionalProperties (false only),
//   items, enum, const, anyOf, description, title, default, examples, minimum, maximum, exclusiveMinimum,
//   exclusiveMaximum, minLength, maxLength, minItems, maxItems, $schema, $comment
//
// Not portable: $ref/$defs (no recursion), oneOf/allOf/not/if, patternProperties, open objects.
//
// Providers' strict modes accept less, so adaptSchema() moves what a dialect cannot express into the description
// ("Minimum: 0."). The model still sees the bound, and the command engine validates arguments against the
// original schema in any case.

#include <wabcut/ai/AiError.h>
#include <wabcut/ai/Chat.h>

#include <expected>
#include <string_view>

namespace wabcut::ai {

enum class SchemaDialect {
    Anthropic,       ///< Messages API input_schema / output_config.format (strict: no numeric/length bounds)
    OpenAiStrict,    ///< Responses / Chat Completions with strict: every property required, optional -> nullable
    OpenAiCompatible, ///< non-strict Chat Completions servers (Ollama, LM Studio, llama-server, ...)
    Gemini,          ///< Interactions API function parameters / response_format
};

/// Checks that `schema` is inside the portable subset. InvalidRequest names the first offending path, e.g.
/// "/properties/clip: 'oneOf' is not portable".
std::expected<void, AiError> checkPortableSchema(const Json& schema);

/// Rewrites a portable schema for `dialect`. With `strict` the result follows that provider's strict-mode rules:
///   Anthropic:    bounds (minimum/maximum/exclusive*/minLength/maxLength/minItems > 1/maxItems) and default/
///                 examples move into the description; every object gets additionalProperties: false.
///   OpenAiStrict: every property is required; properties that were optional become nullable (type gains "null",
///                 an enum gains null); minLength/maxLength/default/examples move into the description; const
///                 becomes a one-value enum; objects get additionalProperties: false.
///   OpenAiCompatible / Gemini: annotations ($schema, $comment, examples) are dropped and const becomes a one-value
///                 enum; Gemini also drops additionalProperties, which its schema subset does not accept.
/// Without `strict` only the annotations are dropped. The input is not modified.
Json adaptSchema(const Json& schema, SchemaDialect dialect, bool strict);

/// Tool and response-format names: [A-Za-z0-9_-]{1,64}.
bool isValidToolName(std::string_view name);

/// Validates the tools and response format of a request before it is sent: names, unique tool names, portable
/// schemas, a Named tool choice that names a listed tool. InvalidRequest otherwise.
std::expected<void, AiError> validateChatRequest(const ChatRequest& request);

/// The tool call's arguments as parsed JSON: an object, or InvalidRequest when the model produced text that is
/// not a JSON object (the agent then returns a tool error so the model can retry).
std::expected<Json, AiError> parseToolArguments(std::string_view text);

} // namespace wabcut::ai
