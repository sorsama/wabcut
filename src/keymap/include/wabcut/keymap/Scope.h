#pragma once

// Keyboard scopes. Application shortcuts work regardless of focus; a panel scope's shortcuts work only while
// that panel has focus and, like Premiere, take precedence over an Application shortcut on the same chord.
// Every panel reports its focus to the app's KeymapDispatcher as one of these.

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace wabcut::keymap {

enum class Scope : std::uint8_t {
    Application,    ///< menus and global commands (Premiere "Application" / context.global)
    Timeline,       ///< Timeline panel
    ProgramMonitor, ///< Program Monitor panel
    SourceMonitor,  ///< Source Monitor panel
    Project,        ///< Project panel (bins)
    EffectControls, ///< Effect Controls panel
    Effects,        ///< Effects panel
    AudioMixer,     ///< Audio Track Mixer panel
    History,        ///< History panel
    MediaBrowser,   ///< Media Browser panel
    Metadata,       ///< Metadata panel
};

inline constexpr std::array kAllScopes{
    Scope::Application, Scope::Timeline,       Scope::ProgramMonitor, Scope::SourceMonitor,
    Scope::Project,     Scope::EffectControls, Scope::Effects,        Scope::AudioMixer,
    Scope::History,     Scope::MediaBrowser,   Scope::Metadata,
};

/// Stable id used in JSON tables: "application", "timeline", "programMonitor", "audioMixer", ...
std::string_view scopeId(Scope scope);
std::optional<Scope> scopeFromId(std::string_view id);

/// English panel name for UI: "Application", "Program Monitor", "Audio Track Mixer", ...
std::string_view scopeDisplayName(Scope scope);

} // namespace wabcut::keymap
