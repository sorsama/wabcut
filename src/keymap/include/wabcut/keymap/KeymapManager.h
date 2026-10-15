#pragma once

// KeymapManager: one platform's default shortcut table plus the user's overrides, resolved per focused scope.
//
// Resolution follows Premiere: the focused panel's shortcuts are tried first, then Application shortcuts, so
// a panel shortcut shadows an Application shortcut on the same chord while that panel has focus (Ctrl+L is
// Link in the Clip menu but Loop in the Audio Track Mixer). Each command belongs to exactly one scope (see
// CommandInfo::scope); its chords are the user's override if one exists, else the table's defaults.
//
// Not thread-safe: owned and used on the GUI thread.

#include <wabcut/keymap/CommandRegistry.h>
#include <wabcut/keymap/Key.h>
#include <wabcut/keymap/Platform.h>
#include <wabcut/keymap/Scope.h>
#include <wabcut/model/Error.h>

#include <compare>
#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wabcut::keymap {

/// One effective chord → command assignment.
struct Binding {
    Scope scope = Scope::Application;
    KeyChord chord;
    CommandId command;
    bool overridden = false; ///< comes from the user's overrides rather than the default table

    bool operator==(const Binding&) const = default;
};

/// Two or more commands on the same chord in the same scope.
struct Conflict {
    Scope scope = Scope::Application;
    KeyChord chord;
    std::vector<CommandId> commands; ///< in resolution order: resolve() returns commands.front()
    bool acknowledged = false;       ///< listed in the default table's "acknowledgedConflicts"
};

/// Replacement chords for one command (an empty list unbinds it).
struct CommandOverride {
    CommandId command;
    std::vector<KeyChord> chords;

    bool operator==(const CommandOverride&) const = default;
};

/// Non-fatal findings while loading saved overrides.
struct OverridesReport {
    std::vector<std::string> warnings; ///< e.g. overrides for command ids this build does not know (skipped)
};

class KeymapManager {
public:
    /// An empty keymap (no defaults) for `platform`. The registry must outlive the manager.
    explicit KeymapManager(Platform platform = currentPlatform(),
                           const CommandRegistry& registry = CommandRegistry::builtin());

    /// A keymap holding the built-in Premiere default table for `platform` (Linux uses the Windows table).
    static std::expected<KeymapManager, Error>
    withDefaults(Platform platform = currentPlatform(),
                 const CommandRegistry& registry = CommandRegistry::builtin());

    /// Loads a default table (schema in docs/keymap.md) and makes it the defaults. Chord text is parsed for
    /// the table's "platform"; `platform` must use that table (Linux may load a Windows table). User
    /// overrides are kept unless the platform's modifier family changes. On error nothing changes.
    std::expected<void, Error> load(std::string_view json, Platform platform);
    /// load(json, platform of the table).
    std::expected<void, Error> load(std::string_view json);

    Platform platform() const { return m_platform; }
    const std::string& tableName() const { return m_tableName; }
    const CommandRegistry& registry() const { return *m_registry; }

    // ---- Resolution ----

    /// The command bound to `chord` while `focused` has focus: the focused scope first, then Application.
    /// `chord` may come straight from a Qt key event: Key_Backtab, GroupSwitch, keypad navigation keys
    /// (Delete, arrows, ...; keypad digits and operators stay distinct), shifted US symbols (Shift+Key_Colon
    /// for Shift+;) and keypad Enter (→ Return) are matched to the bound chord. An
    /// exact match in either scope beats such a fallback: an Application keypad binding beats the focused
    /// panel's main-keyboard binding of the same key.
    std::optional<CommandId> resolve(Scope focused, KeyChord chord) const;
    /// Same as resolve(), returning the registry entry (nullptr when nothing is bound).
    const CommandInfo* resolveCommand(Scope focused, KeyChord chord) const;

    // ---- Queries ----

    /// Effective chords of a command (override or default), primary first. Empty for unknown or unbound
    /// commands. Use for tooltips and menus.
    std::vector<KeyChord> chordsFor(std::string_view command) const;
    /// The default table's chords of a command, ignoring overrides.
    std::vector<KeyChord> defaultChordsFor(std::string_view command) const;
    /// The primary chord formatted for this platform ("Ctrl+K", "⌘K"), or "" when unbound.
    std::string shortcutText(std::string_view command, ChordFormat format = ChordFormat::Native) const;
    /// Every effective binding, in registry order.
    std::vector<Binding> bindings() const;
    /// Every same-scope conflict, ordered by scope then chord, with its commands in resolution order: a chord
    /// the user added wins, then the winner of an acknowledged table conflict, then table order. A panel
    /// chord shadowing an Application chord is by design and not a conflict.
    std::vector<Conflict> conflicts() const;

    // ---- User overrides (a small diff over the defaults) ----

    /// Replaces a command's chords. Chords are normalised (KeyChord::normalized, and shifted US symbols
    /// folded to their base key, so chords captured from Qt events work) and de-duplicated; setting the
    /// defaults back removes the override. Errors: NotFound (unknown command), InvalidArgument (invalid
    /// chord).
    std::expected<void, Error> setChords(std::string_view command, std::vector<KeyChord> chords);
    /// Applies several overrides atomically (all validated first; nothing changes on error).
    std::expected<void, Error> applyOverrides(std::span<const CommandOverride> overrides);
    /// Adds `chord` to `command` the way Premiere's shortcut editor does: the chord is taken away from any
    /// other command in the same scope. Returns the commands that lost it.
    std::expected<std::vector<CommandId>, Error> assign(std::string_view command, KeyChord chord);
    /// Removes `chord` from `command` (no-op when it is not bound).
    std::expected<void, Error> unassign(std::string_view command, KeyChord chord);
    void resetToDefault(std::string_view command);
    void resetAllToDefaults();
    bool isOverridden(std::string_view command) const;
    bool hasOverrides() const { return !m_overrides.empty(); }
    /// The current overrides, in command-id order.
    std::vector<CommandOverride> overrides() const;

    /// Serialises the overrides as a small JSON diff:
    /// {"schemaVersion":1,"platform":"windows","overrides":{"tool.razor":["X"],"edit.clear":[]}}
    std::string saveOverrides() const;
    /// Replaces the current overrides with saved ones. The file's platform must use the same modifier family
    /// (Windows/Linux vs macOS). Unknown command ids are skipped with a warning; malformed content is an
    /// error and leaves the current overrides untouched.
    std::expected<OverridesReport, Error> loadOverrides(std::string_view json);

private:
    struct DefaultEntry {
        const CommandInfo* info = nullptr;
        std::vector<KeyChord> chords;
    };
    struct AcknowledgedConflict {
        Scope scope = Scope::Application;
        KeyChord chord;
        std::vector<std::string_view> commands; ///< ids (registry storage), winner first
    };
    struct LookupKey {
        Scope scope = Scope::Application;
        KeyChord chord;
        auto operator<=>(const LookupKey&) const = default;
    };

    std::expected<void, Error> loadTable(std::string_view json, std::optional<Platform> platform);
    const std::vector<KeyChord>* effectiveChords(const CommandInfo& info) const;
    const CommandInfo* lookup(Scope scope, std::span<const KeyChord> candidates) const;
    std::expected<std::vector<KeyChord>, Error> cleanChords(const CommandInfo& info,
                                                            std::span<const KeyChord> chords) const;
    void storeOverride(const CommandInfo& info, std::vector<KeyChord> chords);
    void rebuildIndex();
    const AcknowledgedConflict* findAcknowledged(Scope scope, KeyChord chord) const;

    const CommandRegistry* m_registry;
    Platform m_platform;
    std::string m_tableName;
    std::vector<DefaultEntry> m_defaults;                           ///< table order
    std::unordered_map<std::string_view, std::size_t> m_defaultPos; ///< command id → m_defaults index
    std::vector<AcknowledgedConflict> m_acknowledged;
    std::map<std::string, std::vector<KeyChord>, std::less<>> m_overrides;
    std::map<LookupKey, std::vector<const CommandInfo*>> m_index; ///< (scope, chord) → commands, winner first
};

} // namespace wabcut::keymap
