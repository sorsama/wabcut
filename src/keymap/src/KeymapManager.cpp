#include <wabcut/keymap/DefaultKeymaps.h>
#include <wabcut/keymap/KeymapManager.h>

#include <algorithm>
#include <array>
#include <format>
#include <nlohmann/json.hpp>
#include <set>
#include <tuple>
#include <utility>

namespace wabcut::keymap {
namespace {

using Json = nlohmann::json;

constexpr int kSchemaVersion = 1;

Error invalid(std::string message) {
    return {ErrorCode::InvalidArgument, std::move(message)};
}
Error parseFailure(std::string message) {
    return {ErrorCode::ParseError, std::move(message)};
}
Error unknownCommand(std::string_view id) {
    return {ErrorCode::NotFound, std::format("unknown command '{}'", id)};
}

std::optional<std::string_view> stringField(const Json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return std::nullopt;
    }
    return std::string_view(it->get_ref<const std::string&>());
}

/// Parses an array of chord strings (`what` names the owner for error messages).
std::expected<std::vector<KeyChord>, Error> parseChordList(const Json& keys, Platform p,
                                                           std::string_view what) {
    if (!keys.is_array()) {
        return std::unexpected(parseFailure(std::format("{}: \"keys\" must be an array of strings", what)));
    }
    std::vector<KeyChord> chords;
    for (const Json& k : keys) {
        if (!k.is_string()) {
            return std::unexpected(
                parseFailure(std::format("{}: \"keys\" must be an array of strings", what)));
        }
        auto chord = parseKeyChord(k.get_ref<const std::string&>(), p);
        if (!chord) {
            return std::unexpected(parseFailure(std::format("{}: {}", what, chord.error().message)));
        }
        chords.push_back(*chord);
    }
    return chords;
}

std::expected<Json, Error> parseJsonObject(std::string_view text, std::string_view what) {
    Json doc = Json::parse(text, nullptr, false);
    if (doc.is_discarded()) {
        return std::unexpected(parseFailure(std::format("{} is not valid JSON", what)));
    }
    if (!doc.is_object()) {
        return std::unexpected(parseFailure(std::format("{} must be a JSON object", what)));
    }
    const auto version = doc.find("schemaVersion");
    if (version == doc.end() || !version->is_number_integer()) {
        return std::unexpected(parseFailure(std::format("{} has no integer \"schemaVersion\"", what)));
    }
    if (version->get<std::int64_t>() != kSchemaVersion) {
        return std::unexpected(
            Error{ErrorCode::VersionMismatch, std::format("{} has schemaVersion {}, expected {}", what,
                                                          version->get<std::int64_t>(), kSchemaVersion)});
    }
    return doc;
}

/// The form a chord is stored in: normalised, and with Qt's shifted US symbols folded back to the base key
/// (Shift+Key_Colon → Shift+;), so a chord captured from a QKeyEvent binds what resolve() matches.
KeyChord canonicalChord(KeyChord chord) {
    const KeyChord n = chord.normalized();
    return unshiftedUsLayout(n).value_or(n);
}

std::vector<KeyChord> dedupe(std::vector<KeyChord> chords) {
    std::vector<KeyChord> out;
    out.reserve(chords.size());
    for (const KeyChord& c : chords) {
        if (std::ranges::find(out, c) == out.end()) {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

KeymapManager::KeymapManager(Platform platform, const CommandRegistry& registry)
    : m_registry(&registry), m_platform(platform) {}

std::expected<KeymapManager, Error> KeymapManager::withDefaults(Platform platform,
                                                                const CommandRegistry& registry) {
    KeymapManager manager(platform, registry);
    if (auto loaded = manager.load(defaultKeymapJson(platform), platform); !loaded) {
        return std::unexpected(loaded.error());
    }
    return manager;
}

std::expected<void, Error> KeymapManager::load(std::string_view json) {
    return loadTable(json, std::nullopt);
}

std::expected<void, Error> KeymapManager::load(std::string_view json, Platform platform) {
    return loadTable(json, platform);
}

std::expected<void, Error> KeymapManager::loadTable(std::string_view json, std::optional<Platform> platform) {
    try {
        auto doc = parseJsonObject(json, "keymap table");
        if (!doc) {
            return std::unexpected(doc.error());
        }
        const auto platformText = stringField(*doc, "platform");
        const auto tablePlat = platformText ? platformFromId(*platformText) : std::nullopt;
        if (!tablePlat || *tablePlat == Platform::Linux) {
            return std::unexpected(
                parseFailure("keymap table: \"platform\" must be \"windows\" or \"macos\""));
        }
        const Platform runtime = platform.value_or(*tablePlat);
        if (tablePlatform(runtime) != *tablePlat) {
            return std::unexpected(invalid(std::format("keymap table is for {}, which {} does not use",
                                                       *platformText, platformId(runtime))));
        }

        const auto bindings = doc->find("bindings");
        if (bindings == doc->end() || !bindings->is_array()) {
            return std::unexpected(parseFailure("keymap table: \"bindings\" must be an array"));
        }

        std::vector<DefaultEntry> defaults;
        std::unordered_map<std::string_view, std::size_t> positions;
        for (const Json& b : *bindings) {
            if (!b.is_object()) {
                return std::unexpected(parseFailure("keymap table: every binding must be an object"));
            }
            const auto command = stringField(b, "command");
            const auto scopeText = stringField(b, "scope");
            if (!command || !scopeText) {
                return std::unexpected(
                    parseFailure("keymap table: a binding lacks \"command\" or \"scope\""));
            }
            const CommandInfo* info = m_registry->find(*command);
            if (info == nullptr) {
                return std::unexpected(unknownCommand(*command));
            }
            const auto scope = scopeFromId(*scopeText);
            if (!scope) {
                return std::unexpected(
                    parseFailure(std::format("{}: unknown scope '{}'", *command, *scopeText)));
            }
            if (*scope != info->scope) {
                return std::unexpected(
                    invalid(std::format("{}: bound in scope '{}' but the command belongs to '{}'", *command,
                                        *scopeText, scopeId(info->scope))));
            }
            if (positions.contains(info->id)) {
                return std::unexpected(invalid(std::format("{}: listed twice", *command)));
            }
            const auto keys = b.find("keys");
            if (keys == b.end()) {
                return std::unexpected(parseFailure(std::format("{}: missing \"keys\"", *command)));
            }
            auto chords = parseChordList(*keys, *tablePlat, *command);
            if (!chords) {
                return std::unexpected(chords.error());
            }
            auto clean = cleanChords(*info, *chords);
            if (!clean) {
                return std::unexpected(clean.error());
            }
            positions.emplace(info->id, defaults.size());
            defaults.push_back({info, std::move(*clean)});
        }

        // Acknowledged conflicts must describe a conflict that really exists among the defaults.
        std::vector<AcknowledgedConflict> acknowledged;
        if (const auto acks = doc->find("acknowledgedConflicts"); acks != doc->end()) {
            if (!acks->is_array()) {
                return std::unexpected(
                    parseFailure("keymap table: \"acknowledgedConflicts\" must be an array"));
            }
            for (const Json& a : *acks) {
                const auto malformed = [] {
                    return std::unexpected(parseFailure(
                        "keymap table: an acknowledged conflict needs \"scope\", \"keys\", \"commands\""));
                };
                if (!a.is_object()) {
                    return malformed();
                }
                const auto scopeText = stringField(a, "scope");
                const auto keysText = stringField(a, "keys");
                const auto commands = a.find("commands");
                if (!scopeText || !keysText || commands == a.end() || !commands->is_array()) {
                    return malformed();
                }
                AcknowledgedConflict ack;
                const auto scope = scopeFromId(*scopeText);
                auto chord = parseKeyChord(*keysText, *tablePlat);
                if (!scope || !chord) {
                    return std::unexpected(
                        parseFailure(std::format("keymap table: bad acknowledged conflict '{}'", *keysText)));
                }
                ack.scope = *scope;
                ack.chord = canonicalChord(*chord);
                std::set<std::string_view> listed;
                for (const Json& c : *commands) {
                    const CommandInfo* info =
                        c.is_string() ? m_registry->find(c.get_ref<const std::string&>()) : nullptr;
                    if (info == nullptr) {
                        return std::unexpected(invalid(std::format(
                            "keymap table: acknowledged conflict on '{}' names an unknown command",
                            *keysText)));
                    }
                    ack.commands.push_back(info->id);
                    listed.insert(info->id);
                }
                std::set<std::string_view> actual;
                for (const DefaultEntry& e : defaults) {
                    if (e.info->scope == ack.scope &&
                        std::ranges::find(e.chords, ack.chord) != e.chords.end()) {
                        actual.insert(e.info->id);
                    }
                }
                if (listed.size() < 2 || listed != actual) {
                    return std::unexpected(invalid(std::format(
                        "keymap table: acknowledged conflict on '{}' does not match the table", *keysText)));
                }
                acknowledged.push_back(std::move(ack));
            }
        }

        // Commit.
        if (usesMacModifiers(runtime) != usesMacModifiers(m_platform)) {
            m_overrides.clear(); // override chords were written for the other modifier family
        }
        m_platform = runtime;
        m_tableName = stringField(*doc, "name").value_or("");
        m_defaults = std::move(defaults);
        m_defaultPos = std::move(positions);
        m_acknowledged = std::move(acknowledged);
        rebuildIndex();
        return {};
    } catch (const Json::exception& e) {
        return std::unexpected(parseFailure(std::format("keymap table: {}", e.what())));
    }
}

const std::vector<KeyChord>* KeymapManager::effectiveChords(const CommandInfo& info) const {
    if (const auto it = m_overrides.find(info.id); it != m_overrides.end()) {
        return &it->second;
    }
    if (const auto it = m_defaultPos.find(info.id); it != m_defaultPos.end()) {
        return &m_defaults[it->second].chords;
    }
    return nullptr;
}

void KeymapManager::rebuildIndex() {
    m_index.clear();
    for (const auto& [id, chords] : m_overrides) {
        const CommandInfo* info = m_registry->find(id);
        for (const KeyChord& c : chords) {
            m_index[{info->scope, c}].push_back(info);
        }
    }
    for (const DefaultEntry& e : m_defaults) {
        if (m_overrides.contains(e.info->id)) {
            continue;
        }
        for (const KeyChord& c : e.chords) {
            m_index[{e.info->scope, c}].push_back(e.info);
        }
    }
    // Winner order for shared chords: a chord the user added (not among the command's defaults) wins; then
    // the winner order of an acknowledged conflict; then default-table order.
    for (auto& [key, infos] : m_index) {
        if (infos.size() < 2) {
            continue;
        }
        const AcknowledgedConflict* ack = findAcknowledged(key.scope, key.chord);
        const auto rank = [&](const CommandInfo* info) {
            const auto def = m_defaultPos.find(info->id);
            bool fromDefaults = false;
            if (def != m_defaultPos.end()) {
                const auto& chords = m_defaults[def->second].chords;
                fromDefaults = std::ranges::find(chords, key.chord) != chords.end();
            }
            std::size_t ackPos = 0;
            if (ack != nullptr) {
                ackPos = static_cast<std::size_t>(
                    std::distance(ack->commands.begin(), std::ranges::find(ack->commands, info->id)));
            }
            const std::size_t tablePos = def != m_defaultPos.end() ? def->second : m_defaults.size();
            return std::tuple{fromDefaults ? 1 : 0, ackPos, tablePos};
        };
        std::ranges::stable_sort(infos, {}, rank);
    }
}

const KeymapManager::AcknowledgedConflict* KeymapManager::findAcknowledged(Scope scope,
                                                                           KeyChord chord) const {
    for (const AcknowledgedConflict& ack : m_acknowledged) {
        if (ack.scope == scope && ack.chord == chord) {
            return &ack;
        }
    }
    return nullptr;
}

const CommandInfo* KeymapManager::lookup(Scope scope, std::span<const KeyChord> candidates) const {
    for (const KeyChord& c : candidates) {
        if (const auto it = m_index.find({scope, c}); it != m_index.end()) {
            return it->second.front();
        }
    }
    return nullptr;
}

const CommandInfo* KeymapManager::resolveCommand(Scope focused, KeyChord chord) const {
    const KeyChord base = chord.normalized();
    if (!base.isValid()) {
        return nullptr;
    }
    // Spellings of the same physical key press: the exact chord first, then fallbacks.
    std::array<KeyChord, 8> candidates{};
    std::size_t count = 0;
    const auto add = [&](KeyChord c) {
        if (std::find(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(count), c) ==
            candidates.begin() + static_cast<std::ptrdiff_t>(count)) {
            candidates[count++] = c;
        }
    };
    add(base);
    // Keypad navigation/editing keys (Enter, Delete, arrows, Home/End, PgUp/PgDn, Insert; Qt also flags the
    // macOS arrow keys as keypad keys) fall back to their main-keyboard bindings. Keypad digits and operators
    // stay distinct: Premiere reserves them for timecode entry (numpad + and - are relative timecode).
    if (base.mods.has(Modifier::Keypad) && base.key >= Key_Escape) {
        add({base.key, base.mods.without(Modifier::Keypad)});
    }
    for (std::size_t i = 0, n = count; i < n; ++i) {
        if (const auto unshifted = unshiftedUsLayout(candidates[i])) {
            add(*unshifted);
        }
    }
    if (base.key == Key_Enter) {
        add({Key_Return, base.mods.without(Modifier::Keypad)});
    }
    // An exact match in either scope beats a fallback spelling; within each pass the focused panel comes
    // first.
    const std::array<std::span<const KeyChord>, 2> passes{
        std::span<const KeyChord>(candidates.data(), 1),
        std::span<const KeyChord>(candidates.data() + 1, count - 1),
    };
    for (const std::span<const KeyChord> spellings : passes) {
        if (focused != Scope::Application) {
            if (const CommandInfo* info = lookup(focused, spellings)) {
                return info;
            }
        }
        if (const CommandInfo* info = lookup(Scope::Application, spellings)) {
            return info;
        }
    }
    return nullptr;
}

std::optional<CommandId> KeymapManager::resolve(Scope focused, KeyChord chord) const {
    if (const CommandInfo* info = resolveCommand(focused, chord)) {
        return CommandId(info->id);
    }
    return std::nullopt;
}

std::vector<KeyChord> KeymapManager::chordsFor(std::string_view command) const {
    const CommandInfo* info = m_registry->find(command);
    if (info == nullptr) {
        return {};
    }
    const auto* chords = effectiveChords(*info);
    return chords != nullptr ? *chords : std::vector<KeyChord>{};
}

std::vector<KeyChord> KeymapManager::defaultChordsFor(std::string_view command) const {
    const auto it = m_defaultPos.find(command);
    return it == m_defaultPos.end() ? std::vector<KeyChord>{} : m_defaults[it->second].chords;
}

std::string KeymapManager::shortcutText(std::string_view command, ChordFormat format) const {
    const auto chords = chordsFor(command);
    return chords.empty() ? std::string{} : formatKeyChord(chords.front(), m_platform, format);
}

std::vector<Binding> KeymapManager::bindings() const {
    std::vector<Binding> out;
    for (const CommandInfo& info : m_registry->all()) {
        const auto* chords = effectiveChords(info);
        if (chords == nullptr) {
            continue;
        }
        const bool overridden = m_overrides.contains(info.id);
        for (const KeyChord& c : *chords) {
            out.push_back({info.scope, c, CommandId(info.id), overridden});
        }
    }
    return out;
}

std::vector<Conflict> KeymapManager::conflicts() const {
    std::vector<Conflict> out;
    for (const auto& [key, infos] : m_index) {
        if (infos.size() < 2) {
            continue;
        }
        Conflict conflict{key.scope, key.chord, {}, false};
        for (const CommandInfo* info : infos) {
            conflict.commands.emplace_back(info->id);
        }
        // Acknowledged only while it is exactly the table's conflict, with the table's winner.
        if (const AcknowledgedConflict* ack = findAcknowledged(key.scope, key.chord)) {
            conflict.acknowledged = std::ranges::equal(ack->commands, conflict.commands);
        }
        out.push_back(std::move(conflict));
    }
    return out;
}

std::expected<std::vector<KeyChord>, Error>
KeymapManager::cleanChords(const CommandInfo& info, std::span<const KeyChord> chords) const {
    std::vector<KeyChord> out;
    for (const KeyChord& c : chords) {
        const KeyChord n = canonicalChord(c);
        if (!n.isValid()) {
            return std::unexpected(
                invalid(std::format("{}: invalid key chord (key 0x{:X})", info.id, c.key)));
        }
        out.push_back(n);
    }
    return dedupe(std::move(out));
}

void KeymapManager::storeOverride(const CommandInfo& info, std::vector<KeyChord> chords) {
    const auto def = m_defaultPos.find(info.id);
    const bool equalsDefault =
        def != m_defaultPos.end() ? m_defaults[def->second].chords == chords : chords.empty();
    if (equalsDefault) {
        m_overrides.erase(std::string(info.id));
    } else {
        m_overrides.insert_or_assign(std::string(info.id), std::move(chords));
    }
}

std::expected<void, Error> KeymapManager::setChords(std::string_view command, std::vector<KeyChord> chords) {
    const CommandInfo* info = m_registry->find(command);
    if (info == nullptr) {
        return std::unexpected(unknownCommand(command));
    }
    auto clean = cleanChords(*info, chords);
    if (!clean) {
        return std::unexpected(clean.error());
    }
    storeOverride(*info, std::move(*clean));
    rebuildIndex();
    return {};
}

std::expected<void, Error> KeymapManager::applyOverrides(std::span<const CommandOverride> overrides) {
    std::vector<std::pair<const CommandInfo*, std::vector<KeyChord>>> staged;
    for (const CommandOverride& o : overrides) {
        const CommandInfo* info = m_registry->find(o.command);
        if (info == nullptr) {
            return std::unexpected(unknownCommand(o.command));
        }
        auto clean = cleanChords(*info, o.chords);
        if (!clean) {
            return std::unexpected(clean.error());
        }
        staged.emplace_back(info, std::move(*clean));
    }
    for (auto& [info, chords] : staged) {
        storeOverride(*info, std::move(chords));
    }
    rebuildIndex();
    return {};
}

std::expected<std::vector<CommandId>, Error> KeymapManager::assign(std::string_view command, KeyChord chord) {
    const CommandInfo* info = m_registry->find(command);
    if (info == nullptr) {
        return std::unexpected(unknownCommand(command));
    }
    const KeyChord c = canonicalChord(chord);
    if (!c.isValid()) {
        return std::unexpected(
            invalid(std::format("{}: invalid key chord (key 0x{:X})", command, chord.key)));
    }
    std::vector<CommandId> lost;
    if (const auto it = m_index.find({info->scope, c}); it != m_index.end()) {
        const std::vector<const CommandInfo*> holders =
            it->second; // copy: storeOverride invalidates nothing yet
        for (const CommandInfo* other : holders) {
            if (other == info) {
                continue;
            }
            std::vector<KeyChord> remaining = chordsFor(other->id);
            std::erase(remaining, c);
            storeOverride(*other, std::move(remaining));
            lost.emplace_back(other->id);
        }
    }
    std::vector<KeyChord> mine = chordsFor(info->id);
    if (std::ranges::find(mine, c) == mine.end()) {
        mine.push_back(c);
    }
    storeOverride(*info, std::move(mine));
    rebuildIndex();
    return lost;
}

std::expected<void, Error> KeymapManager::unassign(std::string_view command, KeyChord chord) {
    const CommandInfo* info = m_registry->find(command);
    if (info == nullptr) {
        return std::unexpected(unknownCommand(command));
    }
    std::vector<KeyChord> mine = chordsFor(info->id);
    if (std::erase(mine, canonicalChord(chord)) == 0) {
        return {};
    }
    storeOverride(*info, std::move(mine));
    rebuildIndex();
    return {};
}

void KeymapManager::resetToDefault(std::string_view command) {
    if (const auto it = m_overrides.find(command); it != m_overrides.end()) {
        m_overrides.erase(it);
        rebuildIndex();
    }
}

void KeymapManager::resetAllToDefaults() {
    m_overrides.clear();
    rebuildIndex();
}

bool KeymapManager::isOverridden(std::string_view command) const {
    return m_overrides.contains(command);
}

std::vector<CommandOverride> KeymapManager::overrides() const {
    std::vector<CommandOverride> out;
    for (const auto& [id, chords] : m_overrides) {
        out.push_back({id, chords});
    }
    return out;
}

std::string KeymapManager::saveOverrides() const {
    nlohmann::ordered_json doc;
    doc["schemaVersion"] = kSchemaVersion;
    doc["platform"] = std::string(platformId(m_platform));
    nlohmann::ordered_json overrides = nlohmann::ordered_json::object();
    for (const auto& [id, chords] : m_overrides) {
        nlohmann::ordered_json keys = nlohmann::ordered_json::array();
        for (const KeyChord& c : chords) {
            keys.push_back(formatKeyChord(c, m_platform, ChordFormat::Text));
        }
        overrides[id] = std::move(keys);
    }
    doc["overrides"] = std::move(overrides);
    return doc.dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) + "\n";
}

std::expected<OverridesReport, Error> KeymapManager::loadOverrides(std::string_view json) {
    try {
        auto doc = parseJsonObject(json, "keymap overrides");
        if (!doc) {
            return std::unexpected(doc.error());
        }
        const auto platformText = stringField(*doc, "platform");
        const auto filePlatform = platformText ? platformFromId(*platformText) : std::nullopt;
        if (!filePlatform) {
            return std::unexpected(parseFailure("keymap overrides: missing or unknown \"platform\""));
        }
        if (usesMacModifiers(*filePlatform) != usesMacModifiers(m_platform)) {
            return std::unexpected(
                invalid(std::format("keymap overrides were saved for {}, this keymap is for {}",
                                    *platformText, platformId(m_platform))));
        }
        const auto entries = doc->find("overrides");
        if (entries == doc->end() || !entries->is_object()) {
            return std::unexpected(parseFailure("keymap overrides: \"overrides\" must be an object"));
        }
        OverridesReport report;
        std::vector<std::pair<const CommandInfo*, std::vector<KeyChord>>> staged;
        for (const auto& [id, keys] : entries->items()) {
            auto chords = parseChordList(keys, *filePlatform, id);
            if (!chords) {
                return std::unexpected(chords.error());
            }
            const CommandInfo* info = m_registry->find(id);
            if (info == nullptr) {
                // Ids renamed since overrides could be saved (M5j moved the trim commands to the Sequence menu, where
                // Premiere has them): keep the user's keys (review P2-45).
                static constexpr std::pair<std::string_view, std::string_view> kRenamed[] = {
                    {"timeline.toggleTrimType", "sequence.toggleTrimType"},
                    {"timeline.trimBackward", "sequence.trimBackward"},
                    {"timeline.trimForward", "sequence.trimForward"},
                    {"timeline.trimBackwardMany", "sequence.trimBackwardMany"},
                    {"timeline.trimForwardMany", "sequence.trimForwardMany"},
                };
                for (const auto& [from, to] : kRenamed) {
                    if (id == from) {
                        info = m_registry->find(to);
                    }
                }
            }
            if (info == nullptr) {
                report.warnings.push_back(std::format("skipped override for unknown command '{}'", id));
                continue;
            }
            auto clean = cleanChords(*info, *chords);
            if (!clean) {
                return std::unexpected(clean.error());
            }
            staged.emplace_back(info, std::move(*clean));
        }
        m_overrides.clear();
        for (auto& [info, chords] : staged) {
            storeOverride(*info, std::move(chords));
        }
        rebuildIndex();
        return report;
    } catch (const Json::exception& e) {
        return std::unexpected(parseFailure(std::format("keymap overrides: {}", e.what())));
    }
}

} // namespace wabcut::keymap
