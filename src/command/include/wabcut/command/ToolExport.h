#pragma once

// Exports the command catalog as tool definitions for LLM APIs. Pure data: nothing here talks to a network.
//
//   JsonSchema  [{"name": "timeline.rippleDelete", "toolName": "timeline_rippleDelete", "version": 1,
//                 "title": ..., "capability": "EditSequence", "description": ..., "parameters": {...}}]
//   Anthropic   [{"name": "timeline_rippleDelete", "description": ..., "input_schema": {...}}]
//               (Messages API "tools")
//   OpenAI      [{"type": "function", "function": {"name": "timeline_rippleDelete", "description": ...,
//                 "parameters": {...}}}]  (Chat Completions "tools")
//
// Tool names use '_' instead of '.' because both APIs only accept [A-Za-z0-9_-]{1,64};
// CommandRegistry::find() accepts them back.

#include <wabcut/command/Command.h>
#include <wabcut/command/Registry.h>

#include <optional>
#include <string_view>

namespace wabcut::command {

enum class ToolFormat { JsonSchema, Anthropic, OpenAI };

std::optional<ToolFormat> toolFormatFromString(std::string_view text); ///< "jsonschema", "anthropic", "openai"

/// The newest version of every command whose capability is in `allowed`, in name order.
Json exportTools(const CommandRegistry& registry, ToolFormat format, CapabilitySet allowed = CapabilitySet::all());

} // namespace wabcut::command
