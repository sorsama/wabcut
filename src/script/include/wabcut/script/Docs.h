#pragma once

// The generated part of docs/extensions.md: which commands each grant lets an extension run, from the catalog
// (wabcut_command_docgen writes it between the markers; tests/script/test_docs.cpp keeps it in sync).

#include <wabcut/command/Registry.h>

#include <optional>
#include <string>
#include <string_view>

namespace wabcut::script {

inline constexpr std::string_view kGeneratedBegin = "<!-- BEGIN GENERATED (wabcut_command_docgen): extension commands -->";
inline constexpr std::string_view kGeneratedEnd = "<!-- END GENERATED -->";

std::string extensionCommandsMarkdown(const command::CommandRegistry& registry);
/// `doc` with the text between the markers replaced by extensionCommandsMarkdown(); nullopt without the markers.
std::optional<std::string> withExtensionCommands(std::string_view doc, const command::CommandRegistry& registry);

} // namespace wabcut::script
