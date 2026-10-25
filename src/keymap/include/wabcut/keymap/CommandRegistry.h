#pragma once

// Command registry: every command the keymap can bind, with a stable dotted id, its UI names, its scope and
// the Premiere Pro identity used to import Premiere key sets (.kys).

#include <wabcut/keymap/Scope.h>

#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace wabcut::keymap {

/// Stable dotted command id, e.g. "edit.undo", "timeline.rippleDelete", "tool.razor",
/// "transport.shuttleForward". Ids never change once shipped: user overrides and the app refer to them.
using CommandId = std::string;

struct CommandInfo {
    std::string_view id;
    /// The one scope the command is bound in (Premiere commands are per panel, e.g. Edit > Ripple Delete
    /// and the Timeline panel's Ripple Delete are separate commands).
    Scope scope = Scope::Application;
    /// Menu or panel group for menus and the shortcuts editor ("Edit", "Tools", ...).
    std::string_view group;
    /// Wabcut UI name.
    std::string_view displayName;
    /// Premiere's English command name as Adobe documents it ("" for Wabcut-only commands).
    std::string_view premiereName;
    /// Premiere .kys identity: shortcut context ("context.global", "context.timeline", ...) and internal
    /// command name ("cmd.edit.undo"). Empty when no mapping has been verified; such commands are skipped by
    /// .kys import. See docs/keymap.md for how each mapping was verified.
    std::string_view kysContext;
    std::string_view kysName;
    /// Whether a held key's auto-repeat re-triggers the command (frame steps, nudges, zoom, undo). Toggles
    /// such as Play/Stop must not repeat.
    bool autoRepeat = false;
};

/// Ids of the shuttle commands that drive the J/K/L gesture (TransportGesture turns them into intents).
namespace commands {
inline constexpr std::string_view kShuttleBackward = "transport.shuttleBackward"; ///< J
inline constexpr std::string_view kShuttleStop = "transport.shuttleStop";         ///< K
inline constexpr std::string_view kShuttleForward = "transport.shuttleForward";   ///< L
/// 0.4: Premiere's Shuttle Slow Left / Right (Shift+J / Shift+L): latched slow play, 1/4x -> 1/2x -> 1x.
inline constexpr std::string_view kShuttleSlowBackward = "transport.shuttleSlowBackward"; ///< Shift+J
inline constexpr std::string_view kShuttleSlowForward = "transport.shuttleSlowForward";   ///< Shift+L
} // namespace commands

/// An immutable, indexed set of commands. The builtin() registry holds every Wabcut command; tests may build
/// their own from static CommandInfo arrays (the registry only references them, so they must outlive it).
class CommandRegistry {
public:
    /// Precondition: ids are unique (later duplicates are not indexed); the span's storage outlives the
    /// registry.
    explicit CommandRegistry(std::span<const CommandInfo> commands);

    /// All built-in Wabcut commands, in menu order.
    static const CommandRegistry& builtin();

    std::span<const CommandInfo> all() const { return m_commands; }
    std::size_t size() const { return m_commands.size(); }

    const CommandInfo* find(std::string_view id) const;
    bool contains(std::string_view id) const { return find(id) != nullptr; }

    /// The command a Premiere .kys item (context, command name) maps to, or nullptr.
    const CommandInfo* findKys(std::string_view kysContext, std::string_view kysName) const;

private:
    std::span<const CommandInfo> m_commands;
    std::unordered_map<std::string_view, std::size_t> m_byId;
    std::map<std::pair<std::string_view, std::string_view>, std::size_t> m_byKys;
};

} // namespace wabcut::keymap
