#include <wabcut/keymap/Scope.h>

namespace wabcut::keymap {
namespace {

struct ScopeNames {
    Scope scope;
    std::string_view id;
    std::string_view display;
};

constexpr std::array kScopeNames{
    ScopeNames{Scope::Application, "application", "Application"},
    ScopeNames{Scope::Timeline, "timeline", "Timeline"},
    ScopeNames{Scope::ProgramMonitor, "programMonitor", "Program Monitor"},
    ScopeNames{Scope::SourceMonitor, "sourceMonitor", "Source Monitor"},
    ScopeNames{Scope::Project, "project", "Project"},
    ScopeNames{Scope::EffectControls, "effectControls", "Effect Controls"},
    ScopeNames{Scope::Effects, "effects", "Effects"},
    ScopeNames{Scope::AudioMixer, "audioMixer", "Audio Track Mixer"},
    ScopeNames{Scope::History, "history", "History"},
    ScopeNames{Scope::MediaBrowser, "mediaBrowser", "Media Browser"},
    ScopeNames{Scope::Metadata, "metadata", "Metadata"},
};
static_assert(kScopeNames.size() == kAllScopes.size());

const ScopeNames& names(Scope scope) {
    for (const ScopeNames& n : kScopeNames) {
        if (n.scope == scope) {
            return n;
        }
    }
    return kScopeNames[0];
}

} // namespace

std::string_view scopeId(Scope scope) {
    return names(scope).id;
}

std::optional<Scope> scopeFromId(std::string_view id) {
    for (const ScopeNames& n : kScopeNames) {
        if (n.id == id) {
            return n.scope;
        }
    }
    return std::nullopt;
}

std::string_view scopeDisplayName(Scope scope) {
    return names(scope).display;
}

} // namespace wabcut::keymap
