#include "ShortcutsBridge.h"

#include "CommandPalette.h"
#include "KeymapDispatcher.h"
#include "ProjectController.h"

#include <wabcut/keymap/Key.h>
#include <wabcut/keymap/KysImport.h>
#include <wabcut/keymap/Scope.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQmlEngine>
#include <QSaveFile>
#include <QSettings>

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace wabcut::app {

namespace km = keymap;

namespace {

const QString kPresetKey = QStringLiteral("shortcuts/preset");
const QString kUserPrefix = QStringLiteral("user:");

QString fromView(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

constexpr std::uint32_t kKeypadBit = static_cast<std::uint32_t>(km::Modifier::Keypad);
constexpr std::uint32_t kModifierBits = static_cast<std::uint32_t>(km::Modifier::Shift) |
                                        static_cast<std::uint32_t>(km::Modifier::Control) |
                                        static_cast<std::uint32_t>(km::Modifier::Alt) |
                                        static_cast<std::uint32_t>(km::Modifier::Meta);

/// The key a chord is drawn on (the US key: "+" is Shift+=) and its modifiers, the keypad bit kept.
km::KeyChord onKeyboard(km::KeyChord chord) {
    if (!chord.mods.has(km::Modifier::Keypad)) {
        if (const auto us = km::unshiftedUsLayout(chord)) {
            return *us;
        }
    }
    return chord;
}

QString keyId(int key, bool keypad) {
    return QString::number(key) + (keypad ? QStringLiteral("k") : QString());
}

// ---- The Wabcut key set ---------------------------------------------------------------------------------
// Premiere Pro's keys with a few changes (docs/keymap.md, "Keyboard Shortcuts editor"). Chords are separated by
// "|"; "-" unbinds the command; "" keeps Premiere's keys on that platform.
struct PresetChange {
    std::string_view command;
    std::string_view windows;
    std::string_view macos;
};
constexpr std::array kWabcutPreset{
    // Redo also on Ctrl+Y, as in most Windows applications.
    PresetChange{"edit.redo", "Ctrl+Shift+Z|Ctrl+Y", ""},
    // Backspace ripple-deletes in the Timeline, as in DaVinci Resolve and Final Cut Pro (Delete still lifts).
    PresetChange{"timeline.rippleDelete", "Backspace|Alt+Backspace", "Backspace|Opt+Backspace"},
    PresetChange{"timeline.clearSelection", "-", "-"},
};

std::expected<std::vector<km::CommandOverride>, Error> wabcutPreset(km::Platform platform) {
    std::vector<km::CommandOverride> out;
    const bool mac = km::usesMacModifiers(platform);
    for (const PresetChange& change : kWabcutPreset) {
        const std::string_view text = mac ? change.macos : change.windows;
        if (text.empty()) {
            continue;
        }
        km::CommandOverride override{std::string(change.command), {}};
        if (text != "-") {
            std::size_t start = 0;
            while (start <= text.size()) {
                const std::size_t end = std::min(text.find('|', start), text.size());
                auto chord = km::parseKeyChord(text.substr(start, end - start), platform);
                if (!chord) {
                    return std::unexpected(chord.error());
                }
                override.chords.push_back(*chord);
                start = end + 1;
            }
        }
        out.push_back(std::move(override));
    }
    return out;
}

bool sameOverrides(const std::vector<km::CommandOverride>& a, const std::vector<km::CommandOverride>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].command != b[i].command || a[i].chords != b[i].chords) {
            return false;
        }
    }
    return true;
}

// ---- The keyboard ---------------------------------------------------------------------------------------
// US ANSI, positions in key units (1 = a letter key): the main block, the navigation block and the keypad.
struct KeyCap {
    int key;
    bool keypad;
    const char* label;
    double x;
    double y;
    double w = 1;
    double h = 1;
    km::Modifier modifier = km::Modifier::None;
};

std::vector<KeyCap> usKeyboard() {
    using M = km::Modifier;
    std::vector<KeyCap> keys;
    const auto row = [&](double x, double y, std::initializer_list<std::pair<int, const char*>> caps) {
        for (const auto& [key, label] : caps) {
            keys.push_back({key, false, label, x, y});
            x += 1;
        }
    };
    // Function row
    keys.push_back({Qt::Key_Escape, false, "Esc", 0, 0});
    row(2, 0, {{Qt::Key_F1, "F1"}, {Qt::Key_F2, "F2"}, {Qt::Key_F3, "F3"}, {Qt::Key_F4, "F4"}});
    row(6.5, 0, {{Qt::Key_F5, "F5"}, {Qt::Key_F6, "F6"}, {Qt::Key_F7, "F7"}, {Qt::Key_F8, "F8"}});
    row(11, 0, {{Qt::Key_F9, "F9"}, {Qt::Key_F10, "F10"}, {Qt::Key_F11, "F11"}, {Qt::Key_F12, "F12"}});
    // Number row
    row(0, 1.25,
        {{Qt::Key_QuoteLeft, "`"}, {Qt::Key_1, "1"}, {Qt::Key_2, "2"}, {Qt::Key_3, "3"}, {Qt::Key_4, "4"},
         {Qt::Key_5, "5"}, {Qt::Key_6, "6"}, {Qt::Key_7, "7"}, {Qt::Key_8, "8"}, {Qt::Key_9, "9"}, {Qt::Key_0, "0"},
         {Qt::Key_Minus, "-"}, {Qt::Key_Equal, "="}});
    keys.push_back({Qt::Key_Backspace, false, "Backspace", 13, 1.25, 2});
    // QWERTY row
    keys.push_back({Qt::Key_Tab, false, "Tab", 0, 2.25, 1.5});
    row(1.5, 2.25,
        {{Qt::Key_Q, "Q"}, {Qt::Key_W, "W"}, {Qt::Key_E, "E"}, {Qt::Key_R, "R"}, {Qt::Key_T, "T"}, {Qt::Key_Y, "Y"},
         {Qt::Key_U, "U"}, {Qt::Key_I, "I"}, {Qt::Key_O, "O"}, {Qt::Key_P, "P"}, {Qt::Key_BracketLeft, "["},
         {Qt::Key_BracketRight, "]"}});
    keys.push_back({Qt::Key_Backslash, false, "\\", 13.5, 2.25, 1.5});
    // Home row
    keys.push_back({Qt::Key_CapsLock, false, "Caps", 0, 3.25, 1.75});
    row(1.75, 3.25,
        {{Qt::Key_A, "A"}, {Qt::Key_S, "S"}, {Qt::Key_D, "D"}, {Qt::Key_F, "F"}, {Qt::Key_G, "G"}, {Qt::Key_H, "H"},
         {Qt::Key_J, "J"}, {Qt::Key_K, "K"}, {Qt::Key_L, "L"}, {Qt::Key_Semicolon, ";"}, {Qt::Key_Apostrophe, "'"}});
    keys.push_back({Qt::Key_Return, false, "Enter", 12.75, 3.25, 2.25});
    // Bottom letter row
    keys.push_back({Qt::Key_Shift, false, "Shift", 0, 4.25, 2.25, 1, M::Shift});
    row(2.25, 4.25,
        {{Qt::Key_Z, "Z"}, {Qt::Key_X, "X"}, {Qt::Key_C, "C"}, {Qt::Key_V, "V"}, {Qt::Key_B, "B"}, {Qt::Key_N, "N"},
         {Qt::Key_M, "M"}, {Qt::Key_Comma, ","}, {Qt::Key_Period, "."}, {Qt::Key_Slash, "/"}});
    keys.push_back({Qt::Key_Shift, false, "Shift", 12.25, 4.25, 2.75, 1, M::Shift});
    // Space row (labels per platform in QML: "Ctrl" / "⌃" ...)
    keys.push_back({Qt::Key_Control, false, "Ctrl", 0, 5.25, 1.25, 1, M::Control});
    keys.push_back({Qt::Key_Meta, false, "Win", 1.25, 5.25, 1.25, 1, M::Meta});
    keys.push_back({Qt::Key_Alt, false, "Alt", 2.5, 5.25, 1.25, 1, M::Alt});
    keys.push_back({Qt::Key_Space, false, "Space", 3.75, 5.25, 6.25});
    keys.push_back({Qt::Key_Alt, false, "Alt", 10, 5.25, 1.25, 1, M::Alt});
    keys.push_back({Qt::Key_Meta, false, "Win", 11.25, 5.25, 1.25, 1, M::Meta});
    keys.push_back({Qt::Key_Menu, false, "Menu", 12.5, 5.25, 1.25});
    keys.push_back({Qt::Key_Control, false, "Ctrl", 13.75, 5.25, 1.25, 1, M::Control});
    // Navigation block
    row(15.5, 1.25, {{Qt::Key_Insert, "Ins"}, {Qt::Key_Home, "Home"}, {Qt::Key_PageUp, "PgUp"}});
    row(15.5, 2.25, {{Qt::Key_Delete, "Del"}, {Qt::Key_End, "End"}, {Qt::Key_PageDown, "PgDn"}});
    keys.push_back({Qt::Key_Up, false, "↑", 16.5, 4.25});
    row(15.5, 5.25, {{Qt::Key_Left, "←"}, {Qt::Key_Down, "↓"}, {Qt::Key_Right, "→"}});
    // Keypad
    const auto pad = [&](int key, const char* label, double x, double y, double w = 1, double h = 1) {
        keys.push_back({key, true, label, x, y, w, h});
    };
    keys.push_back({Qt::Key_NumLock, false, "Num", 18.75, 1.25});
    pad(Qt::Key_Slash, "/", 19.75, 1.25);
    pad(Qt::Key_Asterisk, "*", 20.75, 1.25);
    pad(Qt::Key_Minus, "-", 21.75, 1.25);
    pad(Qt::Key_7, "7", 18.75, 2.25);
    pad(Qt::Key_8, "8", 19.75, 2.25);
    pad(Qt::Key_9, "9", 20.75, 2.25);
    pad(Qt::Key_Plus, "+", 21.75, 2.25, 1, 2);
    pad(Qt::Key_4, "4", 18.75, 3.25);
    pad(Qt::Key_5, "5", 19.75, 3.25);
    pad(Qt::Key_6, "6", 20.75, 3.25);
    pad(Qt::Key_1, "1", 18.75, 4.25);
    pad(Qt::Key_2, "2", 19.75, 4.25);
    pad(Qt::Key_3, "3", 20.75, 4.25);
    pad(Qt::Key_Enter, "Enter", 21.75, 4.25, 1, 2);
    pad(Qt::Key_0, "0", 18.75, 5.25, 2);
    pad(Qt::Key_Period, ".", 20.75, 5.25);
    return keys;
}

} // namespace

// ---- ShortcutCommandModel -------------------------------------------------------------------------------

int ShortcutCommandModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant ShortcutCommandModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= static_cast<int>(m_rows.size())) {
        return {};
    }
    const QHash<int, QByteArray> roles = roleNames();
    const auto it = roles.find(role);
    return it == roles.end() ? QVariant() : m_rows[static_cast<std::size_t>(index.row())].value(QString::fromLatin1(*it));
}

QHash<int, QByteArray> ShortcutCommandModel::roleNames() const {
    return {{CommandIdRole, "commandId"}, {NameRole, "name"},           {PremiereNameRole, "premiereName"},
            {GroupRole, "group"},         {ScopeRole, "scope"},         {ScopeNameRole, "scopeName"},
            {ApplicationRole, "application"}, {ShortcutsRole, "shortcuts"}, {ChordsRole, "chords"},
            {OverriddenRole, "overridden"},   {AvailableRole, "available"}};
}

QVariantMap ShortcutCommandModel::get(int row) const {
    return row >= 0 && row < static_cast<int>(m_rows.size()) ? m_rows[static_cast<std::size_t>(row)] : QVariantMap();
}

int ShortcutCommandModel::indexOf(const QString& commandId) const {
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].value(QStringLiteral("commandId")).toString() == commandId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void ShortcutCommandModel::reset(std::vector<QVariantMap> rows) {
    const bool countChanges = rows.size() != m_rows.size();
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    if (countChanges) {
        emit countChanged();
    }
}

// ---- ShortcutsBridge ------------------------------------------------------------------------------------

ShortcutsBridge::ShortcutsBridge(QObject* parent) : QObject(parent) {
    const QString stored = QSettings().value(kPresetKey).toString();
    if (!stored.isEmpty()) {
        m_preset = stored;
    }
}

ShortcutsBridge* ShortcutsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* shortcuts = new ShortcutsBridge;
    if (qmlEngine) {
        shortcuts->setDispatcher(qmlEngine->singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher"));
    }
    return shortcuts;
}

void ShortcutsBridge::setDispatcher(KeymapDispatcher* dispatcher) {
    if (m_dispatcher == dispatcher) {
        return;
    }
    if (m_dispatcher) {
        disconnect(m_dispatcher, nullptr, this, nullptr);
    }
    m_dispatcher = dispatcher;
    if (m_dispatcher) {
        connect(m_dispatcher, &KeymapDispatcher::keymapChanged, this, &ShortcutsBridge::onKeymapChanged);
    }
    onKeymapChanged();
}

const km::KeymapManager* ShortcutsBridge::keymap() const {
    return m_dispatcher ? &m_dispatcher->keymap() : nullptr;
}

km::Platform ShortcutsBridge::platform() const {
    return m_dispatcher ? m_dispatcher->keymap().platform() : km::currentPlatform();
}

bool ShortcutsBridge::macModifiers() const {
    return km::usesMacModifiers(platform());
}

void ShortcutsBridge::onKeymapChanged() {
    ++m_revision;
    rebuild();
    emit changed();
}

void ShortcutsBridge::setSearch(const QString& search) {
    if (m_search == search) {
        return;
    }
    m_search = search;
    rebuild();
    emit filterChanged();
}

void ShortcutsBridge::setScopeFilter(const QString& scope) {
    if (m_scopeFilter == scope) {
        return;
    }
    m_scopeFilter = scope;
    rebuild();
    emit filterChanged();
}

void ShortcutsBridge::rebuild() {
    const km::KeymapManager* keys = keymap();
    if (!keys) {
        m_commands.reset({});
        return;
    }
    const km::Platform p = platform();
    const QString needle = m_search.trimmed();
    std::vector<QVariantMap> rows;
    for (const km::CommandInfo& info : keys->registry().all()) {
        const bool application = info.scope == km::Scope::Application;
        const QString scope = fromView(km::scopeId(info.scope));
        if (m_scopeFilter == QLatin1String("application") ? !application
            : m_scopeFilter == QLatin1String("panels")    ? application
            : !m_scopeFilter.isEmpty()                    ? scope != m_scopeFilter
                                                          : false) {
            continue;
        }
        QStringList shortcuts;
        QStringList chords;
        for (const km::KeyChord& chord : keys->chordsFor(info.id)) {
            shortcuts << QString::fromStdString(km::formatKeyChord(chord, p, km::ChordFormat::Native));
            chords << QString::fromStdString(km::formatKeyChord(chord, p, km::ChordFormat::Text));
        }
        const QString id = fromView(info.id);
        const QString name = fromView(info.displayName);
        const QString premiereName = fromView(info.premiereName);
        const QString group = fromView(info.group);
        if (!needle.isEmpty()) {
            const auto has = [&](const QString& text) { return text.contains(needle, Qt::CaseInsensitive); };
            const bool chordMatch = std::ranges::any_of(chords, [&](const QString& c) {
                return c.compare(needle, Qt::CaseInsensitive) == 0;
            }) || std::ranges::any_of(shortcuts, [&](const QString& c) { return c.compare(needle, Qt::CaseInsensitive) == 0; });
            if (!has(name) && !has(premiereName) && !has(id) && !has(group) && !chordMatch) {
                continue;
            }
        }
        const bool available = ProjectController::handlesCommand(id) || KeymapCommandProvider::viewHandles(id) ||
                               (m_dispatcher && m_dispatcher->isExtraCommandAvailable(id)); // C.12 extensions
        rows.push_back(QVariantMap{
            {QStringLiteral("commandId"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("premiereName"), premiereName},
            {QStringLiteral("group"), group},
            {QStringLiteral("scope"), scope},
            {QStringLiteral("scopeName"), fromView(km::scopeDisplayName(info.scope))},
            {QStringLiteral("application"), application},
            {QStringLiteral("shortcuts"), shortcuts},
            {QStringLiteral("chords"), chords},
            {QStringLiteral("overridden"), keys->isOverridden(info.id)},
            {QStringLiteral("available"), available},
        });
    }
    m_commands.reset(std::move(rows));
}

QVariantList ShortcutsBridge::scopes() const {
    std::map<km::Scope, int> counts;
    if (const km::KeymapManager* keys = keymap()) {
        for (const km::CommandInfo& info : keys->registry().all()) {
            ++counts[info.scope];
        }
    }
    int panels = 0;
    for (const auto& [scope, count] : counts) {
        if (scope != km::Scope::Application) {
            panels += count;
        }
    }
    QVariantList out{
        QVariantMap{{QStringLiteral("id"), QString()}, {QStringLiteral("name"), tr("All commands")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("application")},
                    {QStringLiteral("name"), tr("Application")},
                    {QStringLiteral("count"), counts[km::Scope::Application]}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("panels")},
                    {QStringLiteral("name"), tr("All panels")},
                    {QStringLiteral("count"), panels}},
    };
    for (const auto& [scope, count] : counts) {
        if (scope == km::Scope::Application) {
            continue;
        }
        out.append(QVariantMap{{QStringLiteral("id"), fromView(km::scopeId(scope))},
                               {QStringLiteral("name"), fromView(km::scopeDisplayName(scope))},
                               {QStringLiteral("count"), count}});
    }
    return out;
}

QVariantList ShortcutsBridge::keyboard() const {
    const bool mac = macModifiers();
    QVariantList out;
    for (const KeyCap& cap : usKeyboard()) {
        QString label = QString::fromUtf8(cap.label);
        if (mac) {
            // On a Mac keyboard: Control ⌃ (Qt's Meta), Option ⌥, Command ⌘ (Qt's Control).
            if (cap.key == Qt::Key_Control) {
                label = QStringLiteral("⌘");
            } else if (cap.key == Qt::Key_Meta) {
                label = QStringLiteral("⌃");
            } else if (cap.key == Qt::Key_Alt) {
                label = QStringLiteral("⌥");
            } else if (cap.key == Qt::Key_Shift) {
                label = QStringLiteral("⇧");
            } else if (cap.key == Qt::Key_Return) {
                label = QStringLiteral("Return");
            } else if (cap.key == Qt::Key_Backspace) {
                label = QStringLiteral("Delete");
            } else if (cap.key == Qt::Key_Delete) {
                label = QStringLiteral("⌦");
            }
        }
        out.append(QVariantMap{
            {QStringLiteral("id"), keyId(cap.key, cap.keypad)},
            {QStringLiteral("key"), cap.key},
            {QStringLiteral("keypad"), cap.keypad},
            {QStringLiteral("label"), label},
            {QStringLiteral("x"), cap.x},
            {QStringLiteral("y"), cap.y},
            {QStringLiteral("w"), cap.w},
            {QStringLiteral("h"), cap.h},
            {QStringLiteral("modifier"), static_cast<int>(static_cast<std::uint32_t>(cap.modifier))},
        });
    }
    return out;
}

QVariantList ShortcutsBridge::modifiers() const {
    const auto bit = [](km::Modifier m) { return static_cast<int>(static_cast<std::uint32_t>(m)); };
    if (macModifiers()) {
        return {QVariantMap{{QStringLiteral("bit"), bit(km::Modifier::Control)}, {QStringLiteral("label"), tr("⌘ Command")}},
                QVariantMap{{QStringLiteral("bit"), bit(km::Modifier::Alt)}, {QStringLiteral("label"), tr("⌥ Option")}},
                QVariantMap{{QStringLiteral("bit"), bit(km::Modifier::Shift)}, {QStringLiteral("label"), tr("⇧ Shift")}},
                QVariantMap{{QStringLiteral("bit"), bit(km::Modifier::Meta)}, {QStringLiteral("label"), tr("⌃ Control")}}};
    }
    return {QVariantMap{{QStringLiteral("bit"), bit(km::Modifier::Control)}, {QStringLiteral("label"), tr("Ctrl")}},
            QVariantMap{{QStringLiteral("bit"), bit(km::Modifier::Shift)}, {QStringLiteral("label"), tr("Shift")}},
            QVariantMap{{QStringLiteral("bit"), bit(km::Modifier::Alt)}, {QStringLiteral("label"), tr("Alt")}}};
}

QVariantMap ShortcutsBridge::keyAssignments(int modifiers) const {
    const km::KeymapManager* keys = keymap();
    if (!keys) {
        return {};
    }
    const std::uint32_t wanted = static_cast<std::uint32_t>(modifiers) & kModifierBits;
    std::map<QString, std::pair<std::set<bool>, QStringList>> found; // keyId -> (application?, names)
    for (const km::Binding& binding : keys->bindings()) {
        const km::KeyChord chord = onKeyboard(binding.chord);
        if ((chord.mods.bits() & kModifierBits) != wanted) {
            continue;
        }
        const km::CommandInfo* info = keys->registry().find(binding.command);
        if (!info) {
            continue;
        }
        auto& entry = found[keyId(chord.key, chord.mods.has(km::Modifier::Keypad))];
        entry.first.insert(info->scope == km::Scope::Application);
        entry.second << fromView(info->displayName);
    }
    QVariantMap out;
    for (const auto& [id, entry] : found) {
        const QString kind = entry.first.size() == 2 ? QStringLiteral("both")
                             : *entry.first.begin()  ? QStringLiteral("application")
                                                     : QStringLiteral("panel");
        out.insert(id, QVariantMap{{QStringLiteral("kind"), kind}, {QStringLiteral("names"), entry.second}});
    }
    return out;
}

QVariantList ShortcutsBridge::commandsForKey(int key, int modifiers) const {
    const km::KeymapManager* keys = keymap();
    if (!keys) {
        return {};
    }
    const std::uint32_t wanted = static_cast<std::uint32_t>(modifiers) & (kModifierBits | kKeypadBit);
    QVariantList out;
    for (const km::Binding& binding : keys->bindings()) {
        const km::KeyChord chord = onKeyboard(binding.chord);
        if (chord.key != key || (chord.mods.bits() & (kModifierBits | kKeypadBit)) != wanted) {
            continue;
        }
        const km::CommandInfo* info = keys->registry().find(binding.command);
        if (!info) {
            continue;
        }
        out.append(QVariantMap{{QStringLiteral("commandId"), fromView(info->id)},
                               {QStringLiteral("name"), fromView(info->displayName)},
                               {QStringLiteral("scope"), fromView(km::scopeId(info->scope))},
                               {QStringLiteral("scopeName"), fromView(km::scopeDisplayName(info->scope))},
                               {QStringLiteral("application"), info->scope == km::Scope::Application},
                               {QStringLiteral("chordText"),
                                QString::fromStdString(km::formatKeyChord(binding.chord, platform()))}});
    }
    return out;
}

QString ShortcutsBridge::chordText(int key, int modifiers) const {
    const km::KeyChord chord = km::KeyChord::fromQt(key, static_cast<std::uint32_t>(modifiers)).normalized();
    if (!chord.isValid()) {
        return {};
    }
    return QString::fromStdString(km::formatKeyChord(chord, platform()));
}

QString ShortcutsBridge::displayChord(const QString& chordText) const {
    const auto chord = km::parseKeyChord(chordText.toStdString(), platform());
    return chord ? QString::fromStdString(km::formatKeyChord(*chord, platform(), km::ChordFormat::Native)) : chordText;
}

QString ShortcutsBridge::commandName(const QString& commandId) const {
    const km::KeymapManager* keys = keymap();
    const km::CommandInfo* info = keys ? keys->registry().find(commandId.toStdString()) : nullptr;
    return info ? fromView(info->displayName) : commandId;
}

QVariantMap ShortcutsBridge::checkAssign(const QString& commandId, const QString& chordText) const {
    QVariantMap out{{QStringLiteral("ok"), false},           {QStringLiteral("error"), QString()},
                    {QStringLiteral("chord"), QString()},     {QStringLiteral("chordText"), chordText},
                    {QStringLiteral("already"), false},       {QStringLiteral("conflicts"), QVariantList()},
                    {QStringLiteral("shadows"), QVariantList()}};
    const km::KeymapManager* keys = keymap();
    if (!keys) {
        out.insert(QStringLiteral("error"), tr("Keyboard shortcuts are not available"));
        return out;
    }
    const km::CommandInfo* info = keys->registry().find(commandId.toStdString());
    if (!info) {
        out.insert(QStringLiteral("error"), tr("Unknown command %1").arg(commandId));
        return out;
    }
    const auto parsed = km::parseKeyChord(chordText.toStdString(), platform());
    if (!parsed) {
        out.insert(QStringLiteral("error"), tr("%1 is not a shortcut Wabcut can use").arg(chordText));
        return out;
    }
    const km::KeyChord chord = onKeyboard(parsed->normalized());
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("chord"), QString::fromStdString(km::formatKeyChord(chord, platform(), km::ChordFormat::Native)));
    out.insert(QStringLiteral("chordText"), QString::fromStdString(km::formatKeyChord(chord, platform())));
    QVariantList conflicts;
    QVariantList shadows;
    for (const km::Binding& binding : keys->bindings()) {
        if (onKeyboard(binding.chord) != chord) {
            continue;
        }
        if (binding.command == info->id) {
            out.insert(QStringLiteral("already"), true);
            continue;
        }
        const km::CommandInfo* other = keys->registry().find(binding.command);
        if (!other) {
            continue;
        }
        const QVariantMap entry{{QStringLiteral("commandId"), fromView(other->id)},
                                {QStringLiteral("name"), fromView(other->displayName)},
                                {QStringLiteral("scopeName"), fromView(km::scopeDisplayName(other->scope))}};
        if (other->scope == info->scope) {
            conflicts.append(entry);
        } else if (other->scope == km::Scope::Application || info->scope == km::Scope::Application) {
            // Premiere's third warning: a panel's command wins over the application's while that panel has focus.
            const bool otherIsPanel = other->scope != km::Scope::Application;
            const km::Scope panel = otherIsPanel ? other->scope : info->scope;
            QVariantMap shadow = entry;
            shadow.insert(QStringLiteral("message"),
                          otherIsPanel ? tr("While the %1 has focus, “%2” runs instead")
                                             .arg(fromView(km::scopeDisplayName(panel)), fromView(other->displayName))
                                       : tr("“%1” runs everywhere except while the %2 has focus")
                                             .arg(fromView(other->displayName), fromView(km::scopeDisplayName(panel))));
            shadows.append(shadow);
        }
    }
    out.insert(QStringLiteral("conflicts"), conflicts);
    out.insert(QStringLiteral("shadows"), shadows);
    return out;
}

QString ShortcutsBridge::commit(const km::KeymapManager& next) {
    if (!m_dispatcher) {
        return tr("Keyboard shortcuts are not available");
    }
    const std::vector<km::CommandOverride> overrides = next.overrides();
    if (auto applied = m_dispatcher->setOverrides(overrides); !applied) {
        return QString::fromStdString(applied.error().message);
    }
    return {};
}

QVariantMap ShortcutsBridge::assign(const QString& commandId, const QString& chordText) {
    QVariantMap out{{QStringLiteral("ok"), false}, {QStringLiteral("error"), QString()}, {QStringLiteral("lost"), QStringList()}};
    const km::KeymapManager* keys = keymap();
    if (!keys) {
        out.insert(QStringLiteral("error"), tr("Keyboard shortcuts are not available"));
        return out;
    }
    const auto chord = km::parseKeyChord(chordText.toStdString(), platform());
    if (!chord) {
        out.insert(QStringLiteral("error"), tr("%1 is not a shortcut Wabcut can use").arg(chordText));
        return out;
    }
    km::KeymapManager next = *keys;
    const auto lost = next.assign(commandId.toStdString(), *chord);
    if (!lost) {
        out.insert(QStringLiteral("error"), QString::fromStdString(lost.error().message));
        return out;
    }
    if (const QString error = commit(next); !error.isEmpty()) {
        out.insert(QStringLiteral("error"), error);
        return out;
    }
    QStringList names;
    for (const km::CommandId& id : *lost) {
        if (const km::CommandInfo* info = keys->registry().find(id)) {
            names << fromView(info->displayName);
        }
    }
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("lost"), names);
    return out;
}

QString ShortcutsBridge::unassign(const QString& commandId, const QString& chordText) {
    const km::KeymapManager* keys = keymap();
    if (!keys) {
        return tr("Keyboard shortcuts are not available");
    }
    const auto chord = km::parseKeyChord(chordText.toStdString(), platform());
    if (!chord) {
        return tr("%1 is not a shortcut Wabcut can use").arg(chordText);
    }
    km::KeymapManager next = *keys;
    if (auto removed = next.unassign(commandId.toStdString(), *chord); !removed) {
        return QString::fromStdString(removed.error().message);
    }
    return commit(next);
}

bool ShortcutsBridge::resetCommand(const QString& commandId) {
    const km::KeymapManager* keys = keymap();
    if (!keys || !keys->isOverridden(commandId.toStdString())) {
        return false;
    }
    km::KeymapManager next = *keys;
    next.resetToDefault(commandId.toStdString());
    return commit(next).isEmpty();
}

void ShortcutsBridge::resetAll() {
    m_preset = QStringLiteral("premiere");
    QSettings().setValue(kPresetKey, m_preset);
    if (m_dispatcher) {
        (void)m_dispatcher->setOverrides({});
    }
    emit changed();
}

// ---- Key sets -------------------------------------------------------------------------------------------

QString ShortcutsBridge::setsFolder() const {
    const QString overrides = m_dispatcher ? m_dispatcher->overridesPath() : QString();
    return overrides.isEmpty() ? QString() : QFileInfo(overrides).absolutePath() + QStringLiteral("/sets");
}

QVariantList ShortcutsBridge::presets() const {
    QVariantList out{
        QVariantMap{{QStringLiteral("id"), QStringLiteral("premiere")},
                    {QStringLiteral("name"), tr("Premiere Pro (default)")},
                    {QStringLiteral("builtin"), true}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("wabcut")},
                    {QStringLiteral("name"), tr("Wabcut")},
                    {QStringLiteral("builtin"), true}},
    };
    if (const QString folder = setsFolder(); !folder.isEmpty()) {
        const QString suffix = km::usesMacModifiers(platform()) ? QStringLiteral("macos") : QStringLiteral("windows");
        for (const QFileInfo& file :
             QDir(folder).entryInfoList({QStringLiteral("*.") + suffix + QStringLiteral(".json")}, QDir::Files, QDir::Name)) {
            const QString name = file.fileName().chopped(suffix.size() + 6); // ".<os>.json"
            out.append(QVariantMap{{QStringLiteral("id"), kUserPrefix + name},
                                   {QStringLiteral("name"), name},
                                   {QStringLiteral("builtin"), false}});
        }
    }
    return out;
}

QString ShortcutsBridge::presetName() const {
    for (const QVariant& p : presets()) {
        const QVariantMap map = p.toMap();
        if (map.value(QStringLiteral("id")).toString() == m_preset) {
            return map.value(QStringLiteral("name")).toString();
        }
    }
    return tr("Premiere Pro (default)");
}

std::vector<km::CommandOverride> ShortcutsBridge::presetOverrides(const QString& id, QString* error) const {
    const km::KeymapManager* keys = keymap();
    if (!keys) {
        return {};
    }
    // Applied to a copy of the defaults and read back, so the result compares with overrides() directly.
    km::KeymapManager next = *keys;
    next.resetAllToDefaults();
    if (id == QLatin1String("wabcut")) {
        const auto preset = wabcutPreset(platform());
        if (!preset) {
            *error = QString::fromStdString(preset.error().message);
            return {};
        }
        if (auto applied = next.applyOverrides(*preset); !applied) {
            *error = QString::fromStdString(applied.error().message);
            return {};
        }
    } else if (id.startsWith(kUserPrefix)) {
        std::vector<km::CommandOverride> overrides;
        const QString suffix = km::usesMacModifiers(platform()) ? QStringLiteral("macos") : QStringLiteral("windows");
        const QString file = setsFolder() + QLatin1Char('/') + id.mid(kUserPrefix.size()) + QLatin1Char('.') + suffix +
                             QStringLiteral(".json");
        const QVariantMap report = readKeySet(QUrl::fromLocalFile(file), &overrides);
        if (!report.value(QStringLiteral("ok")).toBool()) {
            *error = report.value(QStringLiteral("error")).toString();
            return {};
        }
        if (auto applied = next.applyOverrides(overrides); !applied) {
            *error = QString::fromStdString(applied.error().message);
            return {};
        }
    } else if (id != QLatin1String("premiere")) {
        *error = tr("Unknown key set %1").arg(id);
        return {};
    }
    return next.overrides();
}

bool ShortcutsBridge::modified() const {
    const km::KeymapManager* keys = keymap();
    if (!keys) {
        return false;
    }
    QString error;
    const std::vector<km::CommandOverride> preset = presetOverrides(m_preset, &error);
    return !error.isEmpty() || !sameOverrides(keys->overrides(), preset);
}

QString ShortcutsBridge::applyPreset(const QString& id) {
    QString error;
    const std::vector<km::CommandOverride> overrides = presetOverrides(id, &error);
    if (!error.isEmpty()) {
        return error;
    }
    if (!m_dispatcher) {
        return tr("Keyboard shortcuts are not available");
    }
    m_preset = id;
    QSettings().setValue(kPresetKey, m_preset);
    if (auto applied = m_dispatcher->setOverrides(overrides); !applied) {
        return QString::fromStdString(applied.error().message);
    }
    return {};
}

QString ShortcutsBridge::savePreset(const QString& name) {
    const km::KeymapManager* keys = keymap();
    const QString trimmed = name.trimmed();
    if (!keys) {
        return tr("Keyboard shortcuts are not available");
    }
    if (trimmed.isEmpty()) {
        return tr("Name the key set");
    }
    static const QString kInvalid = QStringLiteral("\\/:*?\"<>|");
    if (std::ranges::any_of(trimmed, [](QChar c) { return kInvalid.contains(c) || c.unicode() < 0x20; })) {
        return tr("A key set name cannot contain \\ / : * ? \" < > |");
    }
    if (trimmed.compare(tr("Wabcut"), Qt::CaseInsensitive) == 0 ||
        trimmed.compare(tr("Premiere Pro (default)"), Qt::CaseInsensitive) == 0) {
        return tr("That name belongs to a built-in key set");
    }
    const QString folder = setsFolder();
    if (folder.isEmpty() || !QDir().mkpath(folder)) {
        return tr("Cannot create the folder for key sets");
    }
    const QString suffix = km::usesMacModifiers(platform()) ? QStringLiteral("macos") : QStringLiteral("windows");
    QSaveFile file(folder + QLatin1Char('/') + trimmed + QLatin1Char('.') + suffix + QStringLiteral(".json"));
    const std::string json = keys->saveOverrides();
    if (!file.open(QIODevice::WriteOnly) || file.write(json.data(), static_cast<qint64>(json.size())) < 0 || !file.commit()) {
        return tr("Cannot save the key set: %1").arg(file.errorString());
    }
    m_preset = kUserPrefix + trimmed;
    QSettings().setValue(kPresetKey, m_preset);
    emit changed();
    return {};
}

QString ShortcutsBridge::deletePreset(const QString& id) {
    if (!id.startsWith(kUserPrefix)) {
        return tr("Built-in key sets cannot be deleted");
    }
    const QString suffix = km::usesMacModifiers(platform()) ? QStringLiteral("macos") : QStringLiteral("windows");
    const QString file = setsFolder() + QLatin1Char('/') + id.mid(kUserPrefix.size()) + QLatin1Char('.') + suffix +
                         QStringLiteral(".json");
    if (!QFile::remove(file)) {
        return tr("Cannot delete the key set");
    }
    if (m_preset == id) {
        // The keys stay as they are; they now count as changes to Premiere's defaults.
        m_preset = QStringLiteral("premiere");
        QSettings().setValue(kPresetKey, m_preset);
    }
    emit changed();
    return {};
}

// ---- Import and export ----------------------------------------------------------------------------------

QVariantMap ShortcutsBridge::readKeySet(const QUrl& url, std::vector<km::CommandOverride>* overrides) const {
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    QVariantMap out{{QStringLiteral("ok"), false},         {QStringLiteral("error"), QString()},
                    {QStringLiteral("fileName"), QFileInfo(path).fileName()},
                    {QStringLiteral("changed"), 0},          {QStringLiteral("unmapped"), QStringList()},
                    {QStringLiteral("warnings"), QStringList()}};
    const km::KeymapManager* keys = keymap();
    if (!keys) {
        out.insert(QStringLiteral("error"), tr("Keyboard shortcuts are not available"));
        return out;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        out.insert(QStringLiteral("error"), tr("Cannot read %1: %2").arg(QFileInfo(path).fileName(), file.errorString()));
        return out;
    }
    const QByteArray bytes = file.readAll();
    const std::string_view text(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    km::KeymapManager next = *keys;
    if (path.endsWith(QLatin1String(".kys"), Qt::CaseInsensitive)) {
        out.insert(QStringLiteral("kind"), QStringLiteral("kys"));
        const auto imported = km::parseKys(text, keys->registry());
        if (!imported) {
            out.insert(QStringLiteral("error"), QString::fromStdString(imported.error().message));
            return out;
        }
        if (auto applied = km::applyKysImport(next, *imported); !applied) {
            out.insert(QStringLiteral("error"), QString::fromStdString(applied.error().message));
            return out;
        }
        QStringList unmapped;
        for (const std::string& name : imported->unmappedCommands) {
            unmapped << QString::fromStdString(name);
        }
        QStringList warnings;
        for (const std::string& warning : imported->warnings) {
            warnings << QString::fromStdString(warning);
        }
        out.insert(QStringLiteral("unmapped"), unmapped);
        out.insert(QStringLiteral("warnings"), warnings);
    } else {
        out.insert(QStringLiteral("kind"), QStringLiteral("json"));
        next.resetAllToDefaults();
        const auto loaded = next.loadOverrides(text);
        if (!loaded) {
            out.insert(QStringLiteral("error"), QString::fromStdString(loaded.error().message));
            return out;
        }
        QStringList warnings;
        for (const std::string& warning : loaded->warnings) {
            warnings << QString::fromStdString(warning);
        }
        out.insert(QStringLiteral("warnings"), warnings);
    }
    // How many commands end up with other keys than they have now.
    int changedCount = 0;
    for (const km::CommandInfo& info : keys->registry().all()) {
        if (keys->chordsFor(info.id) != next.chordsFor(info.id)) {
            ++changedCount;
        }
    }
    out.insert(QStringLiteral("changed"), changedCount);
    out.insert(QStringLiteral("ok"), true);
    if (overrides) {
        *overrides = next.overrides();
    }
    return out;
}

QVariantMap ShortcutsBridge::previewKeySet(const QUrl& file) const {
    return readKeySet(file, nullptr);
}

QVariantMap ShortcutsBridge::importKeySet(const QUrl& file) {
    std::vector<km::CommandOverride> overrides;
    QVariantMap report = readKeySet(file, &overrides);
    if (!report.value(QStringLiteral("ok")).toBool()) {
        return report;
    }
    if (!m_dispatcher) {
        report.insert(QStringLiteral("ok"), false);
        report.insert(QStringLiteral("error"), tr("Keyboard shortcuts are not available"));
        return report;
    }
    if (auto applied = m_dispatcher->setOverrides(overrides); !applied) {
        report.insert(QStringLiteral("ok"), false);
        report.insert(QStringLiteral("error"), QString::fromStdString(applied.error().message));
    }
    return report;
}

QString ShortcutsBridge::exportOverrides(const QUrl& url) const {
    const km::KeymapManager* keys = keymap();
    if (!keys) {
        return tr("Keyboard shortcuts are not available");
    }
    QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    if (!path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".json");
    }
    QSaveFile file(path);
    const std::string json = keys->saveOverrides();
    if (!file.open(QIODevice::WriteOnly) || file.write(json.data(), static_cast<qint64>(json.size())) < 0 || !file.commit()) {
        return tr("Cannot write %1: %2").arg(QFileInfo(path).fileName(), file.errorString());
    }
    return {};
}

} // namespace wabcut::app
