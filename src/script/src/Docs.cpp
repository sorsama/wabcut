#include <wabcut/script/Docs.h>

#include <wabcut/script/Manifest.h>

#include <array>

namespace wabcut::script {

using command::Capability;

std::string extensionCommandsMarkdown(const command::CommandRegistry& registry) {
    constexpr std::array kCapabilities{Capability::Read, Capability::EditSequence, Capability::EditProject,
                                       Capability::MediaAnalysis, Capability::Network};
    std::string md = "An extension can run a command when it was granted the command's capability. Commands marked "
                     "*(AI)* run an AI model (locally or in the cloud) and need MediaAnalysis as well; the editor's "
                     "consent, spend cap and no-cloud settings still apply to them. Commands marked *(files)* read or "
                     "write files their parameters name: extensions never run them. Network is never granted.\n\n"
                     "| Capability | Commands |\n|---|---|\n";
    std::string models;
    std::size_t modelCount = 0;
    for (const Capability capability : kCapabilities) {
        std::string list;
        std::size_t count = 0;
        for (const command::CommandSpec& spec : registry.all()) {
            if (spec.capability != capability) {
                continue;
            }
            list += (list.empty() ? "`" : ", `") + spec.name + "`" + (spec.runsModel ? " *(AI)*" : "") +
                    (spec.touchesFiles ? " *(files)*" : "");
            ++count;
        }
        const bool grantable = grantableCapabilities().has(capability);
        md += "| " + std::string(command::toString(capability)) + (grantable ? "" : " (never granted)") + " (" +
              std::to_string(count) + ") | " + (list.empty() ? std::string("none in the catalog") : list) + " |\n";
    }
    for (const command::CommandSpec& spec : registry.all()) {
        if (spec.runsModel) {
            models += (models.empty() ? "`" : ", `") + spec.name + "`";
            ++modelCount;
        }
    }
    md += "\nThe " + std::to_string(modelCount) + " commands that run an AI model: " + models + ".\n";
    return md;
}

std::optional<std::string> withExtensionCommands(std::string_view doc, const command::CommandRegistry& registry) {
    const std::size_t begin = doc.find(kGeneratedBegin);
    const std::size_t end = doc.find(kGeneratedEnd);
    if (begin == std::string_view::npos || end == std::string_view::npos || end < begin) {
        return std::nullopt;
    }
    const std::size_t from = begin + kGeneratedBegin.size();
    return std::string(doc.substr(0, from)) + "\n\n" + extensionCommandsMarkdown(registry) + "\n" +
           std::string(doc.substr(end));
}

} // namespace wabcut::script
