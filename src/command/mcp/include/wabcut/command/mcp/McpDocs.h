#pragma once

#include <wabcut/command/Registry.h>

#include <string>

namespace wabcut::command::mcp {

/// docs/mcp.md: setup, protocol, safety model and the generated tool list.
std::string mcpMarkdown(const CommandRegistry& registry);

} // namespace wabcut::command::mcp
