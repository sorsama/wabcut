#pragma once

// Tool routing for small context windows (AI-15, docs/ai.md "Local assistant tiers"): a local model with 6-8k tokens
// of context cannot read the whole catalog (about 230 tools, tens of thousands of tokens of schemas), so the assistant
// offers only the tools a request is about. Deterministic and offline: the request's words, widened by an editing
// vocabulary ("shorten" -> trim, "subtitles" -> captions, "louder" -> volume), are scored against each command's name,
// title, aliases and description, rarer words counting more. The core read (sequence_summary) is always offered.

#include <wabcut/ai/Chat.h>
#include <wabcut/command/Registry.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::agent {

struct RouteOptions {
    std::size_t maxTools = 12; ///< at most this many tools, the always-offered ones included
    /// Offered whatever the request says (when they are in the catalog), in this order.
    std::vector<std::string> always{"sequence_summary"};
};

struct RoutedTool {
    std::string name;
    double score = 0.0;
};

/// The tools of `tools` (a catalog export, ToolSpec names are tool names) ranked for `prompt`, best first, at most
/// options.maxTools; the always-offered ones come first with score 0. Tools that match no word of the request are
/// left out.
std::vector<RoutedTool> rankTools(const std::vector<ToolSpec>& tools, const command::CommandRegistry& registry,
                                  std::string_view prompt, const RouteOptions& options = {});

/// rankTools() applied: the offered subset of `tools`, in the catalog's order (stable prompts for provider caches).
std::vector<ToolSpec> routeTools(const std::vector<ToolSpec>& tools, const command::CommandRegistry& registry,
                                 std::string_view prompt, const RouteOptions& options = {});

/// How many tools a model with `contextTokens` of context gets: 8 up to 6k tokens, 12 up to 8k, 16 up to 16k;
/// 0 (no routing) above.
std::size_t toolBudgetFor(int contextTokens);

} // namespace wabcut::ai::agent
