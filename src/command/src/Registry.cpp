#include <wabcut/command/Registry.h>

#include <wabcut/command/Schema.h>

#include "Catalog.h"

#include <algorithm>
#include <tuple>

namespace wabcut::command {

std::string toolNameFor(std::string_view commandName) {
    std::string out(commandName);
    std::replace(out.begin(), out.end(), '.', '_');
    return out;
}

std::string CommandRegistry::canonicalName(std::string_view name) const {
    // LLM tool names use '_' for '.'; catalog names and aliases never contain '_'.
    std::string dotted(name);
    std::replace(dotted.begin(), dotted.end(), '_', '.');
    for (const auto& [alias, target] : m_aliases) {
        if (alias == dotted) {
            return target;
        }
    }
    return dotted;
}

std::expected<void, std::string> CommandRegistry::add(CommandSpec spec) {
    if (spec.name.empty() || spec.name.find('_') != std::string::npos || spec.name.find('.') == std::string::npos) {
        return std::unexpected("command names are dotted and contain no '_': " + spec.name);
    }
    if (!spec.execute) {
        return std::unexpected(spec.name + " has no executor");
    }
    if (const auto bad = unsupportedSchemaKeywords(spec.params); !bad.empty()) {
        return std::unexpected(spec.name + " uses unsupported schema keyword " + bad.front());
    }
    for (const CommandSpec& s : m_specs) {
        if (s.name == spec.name && s.version == spec.version) {
            return std::unexpected(spec.name + " v" + std::to_string(spec.version) + " is already registered");
        }
    }
    for (const std::string& alias : spec.aliases) {
        if (alias.find('_') != std::string::npos || alias == spec.name || knows(alias)) {
            return std::unexpected("alias " + alias + " is already taken");
        }
    }
    for (const std::string& alias : spec.aliases) {
        m_aliases.emplace_back(alias, spec.name);
    }
    m_specs.push_back(std::move(spec));
    std::sort(m_specs.begin(), m_specs.end(), [](const CommandSpec& a, const CommandSpec& b) {
        return std::tie(a.name, a.version) < std::tie(b.name, b.version);
    });
    return {};
}

const CommandSpec* CommandRegistry::find(std::string_view name, std::optional<int> version) const {
    const std::string canonical = canonicalName(name);
    const CommandSpec* best = nullptr;
    for (const CommandSpec& s : m_specs) {
        if (s.name != canonical) {
            continue;
        }
        if (version) {
            if (s.version == *version) {
                return &s;
            }
        } else if (!best || s.version > best->version) {
            best = &s;
        }
    }
    return best;
}

bool CommandRegistry::knows(std::string_view name) const { return find(name) != nullptr; }

void registerBuiltinCommands(CommandRegistry& registry) {
    detail::registerEditCommands(registry);
    detail::registerProjectCommands(registry);
    detail::registerQueryCommands(registry);
    detail::registerFinishingCommands(registry);
    detail::registerRhythmCommands(registry);
    detail::registerSourceCommands(registry);
    detail::registerMarkerCommands(registry);
    detail::registerCaptionCommands(registry);
    detail::registerNestingCommands(registry);
    detail::registerCollaborationCommands(registry);
    detail::registerClipboardCommands(registry);
    detail::registerLibraryCommands(registry);
    detail::registerGraphicsCommands(registry);
    detail::registerInterchangeCommands(registry);
    detail::registerParityCommands(registry);
    detail::registerLayoutCommands(registry);
    detail::registerGamingCommands(registry);
}

const CommandRegistry& builtinCommands() {
    static const CommandRegistry registry = [] {
        CommandRegistry r;
        registerBuiltinCommands(r);
        return r;
    }();
    return registry;
}

} // namespace wabcut::command
