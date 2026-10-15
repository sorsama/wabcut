#include "KeymapDispatcher.h"

#include <wabcut/keymap/Key.h>
#include <wabcut/keymap/KysImport.h>
#include <wabcut/keymap/Scope.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtQml/qqml.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace wabcut::app {
namespace km = wabcut::keymap;

/// The built-in commands and the run-time ones, with the storage their string views point into.
struct KeymapDispatcher::Registry {
    std::deque<std::string> strings;
    std::vector<km::CommandInfo> commands;
    std::optional<km::CommandRegistry> registry; ///< over `commands`, built once they are final
};

namespace {

Q_STATIC_LOGGING_CATEGORY(lcKeymap, "wabcut.keymap")

// ---- The Qt adapter -------------------------------------------------------------------------------------
// The keymap module is Qt-free but uses Qt's numeric key and modifier values, so a QKeyEvent converts to a
// KeyChord without a lookup table. These checks keep the two in lockstep.

#define WABCUT_SAME_KEY(name)                                                                                \
    static_assert(km::name == static_cast<int>(Qt::name), "keymap " #name " differs from Qt::" #name)
WABCUT_SAME_KEY(Key_Space);
WABCUT_SAME_KEY(Key_Exclam);
WABCUT_SAME_KEY(Key_QuoteDbl);
WABCUT_SAME_KEY(Key_NumberSign);
WABCUT_SAME_KEY(Key_Dollar);
WABCUT_SAME_KEY(Key_Percent);
WABCUT_SAME_KEY(Key_Ampersand);
WABCUT_SAME_KEY(Key_Apostrophe);
WABCUT_SAME_KEY(Key_ParenLeft);
WABCUT_SAME_KEY(Key_ParenRight);
WABCUT_SAME_KEY(Key_Asterisk);
WABCUT_SAME_KEY(Key_Plus);
WABCUT_SAME_KEY(Key_Comma);
WABCUT_SAME_KEY(Key_Minus);
WABCUT_SAME_KEY(Key_Period);
WABCUT_SAME_KEY(Key_Slash);
WABCUT_SAME_KEY(Key_0);
WABCUT_SAME_KEY(Key_9);
WABCUT_SAME_KEY(Key_Colon);
WABCUT_SAME_KEY(Key_Semicolon);
WABCUT_SAME_KEY(Key_Less);
WABCUT_SAME_KEY(Key_Equal);
WABCUT_SAME_KEY(Key_Greater);
WABCUT_SAME_KEY(Key_Question);
WABCUT_SAME_KEY(Key_At);
WABCUT_SAME_KEY(Key_A);
WABCUT_SAME_KEY(Key_Z);
WABCUT_SAME_KEY(Key_BracketLeft);
WABCUT_SAME_KEY(Key_Backslash);
WABCUT_SAME_KEY(Key_BracketRight);
WABCUT_SAME_KEY(Key_AsciiCircum);
WABCUT_SAME_KEY(Key_Underscore);
WABCUT_SAME_KEY(Key_QuoteLeft);
WABCUT_SAME_KEY(Key_BraceLeft);
WABCUT_SAME_KEY(Key_Bar);
WABCUT_SAME_KEY(Key_BraceRight);
WABCUT_SAME_KEY(Key_AsciiTilde);
WABCUT_SAME_KEY(Key_Escape);
WABCUT_SAME_KEY(Key_Tab);
WABCUT_SAME_KEY(Key_Backtab);
WABCUT_SAME_KEY(Key_Backspace);
WABCUT_SAME_KEY(Key_Return);
WABCUT_SAME_KEY(Key_Enter);
WABCUT_SAME_KEY(Key_Insert);
WABCUT_SAME_KEY(Key_Delete);
WABCUT_SAME_KEY(Key_Pause);
WABCUT_SAME_KEY(Key_Print);
WABCUT_SAME_KEY(Key_Clear);
WABCUT_SAME_KEY(Key_Home);
WABCUT_SAME_KEY(Key_End);
WABCUT_SAME_KEY(Key_Left);
WABCUT_SAME_KEY(Key_Up);
WABCUT_SAME_KEY(Key_Right);
WABCUT_SAME_KEY(Key_Down);
WABCUT_SAME_KEY(Key_PageUp);
WABCUT_SAME_KEY(Key_PageDown);
WABCUT_SAME_KEY(Key_Shift);
WABCUT_SAME_KEY(Key_Control);
WABCUT_SAME_KEY(Key_Meta);
WABCUT_SAME_KEY(Key_Alt);
WABCUT_SAME_KEY(Key_AltGr);
WABCUT_SAME_KEY(Key_CapsLock);
WABCUT_SAME_KEY(Key_NumLock);
WABCUT_SAME_KEY(Key_ScrollLock);
WABCUT_SAME_KEY(Key_F1);
WABCUT_SAME_KEY(Key_F12);
WABCUT_SAME_KEY(Key_F35);
WABCUT_SAME_KEY(Key_unknown);
#undef WABCUT_SAME_KEY

#define WABCUT_SAME_MODIFIER(name, qt)                                                                       \
    static_assert(static_cast<std::uint32_t>(km::Modifier::name) == static_cast<std::uint32_t>(Qt::qt),      \
                  "keymap Modifier::" #name " differs from Qt::" #qt)
WABCUT_SAME_MODIFIER(Shift, ShiftModifier);
WABCUT_SAME_MODIFIER(Control, ControlModifier);
WABCUT_SAME_MODIFIER(Alt, AltModifier);
WABCUT_SAME_MODIFIER(Meta, MetaModifier);
WABCUT_SAME_MODIFIER(Keypad, KeypadModifier);
WABCUT_SAME_MODIFIER(GroupSwitch, GroupSwitchModifier);
#undef WABCUT_SAME_MODIFIER

constexpr int kGestureTickMs = 50;

QString toQString(std::string_view s) {
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString defaultOverridesPath(km::Platform platform) {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
           QStringLiteral("/keymap/overrides-%1.json").arg(toQString(km::platformId(platform)));
}

KeymapDispatcher::TransportKind kindOf(km::TransportIntent::Kind kind) {
    switch (kind) {
    case km::TransportIntent::Kind::ShuttleStep:
        return KeymapDispatcher::TransportKind::ShuttleStep;
    case km::TransportIntent::Kind::SetSpeed:
        return KeymapDispatcher::TransportKind::SetSpeed;
    case km::TransportIntent::Kind::StepFrames:
        return KeymapDispatcher::TransportKind::StepFrames;
    case km::TransportIntent::Kind::Stop:
        break;
    }
    return KeymapDispatcher::TransportKind::Stop;
}

const KeymapScopeAttached* attachedScope(const QQuickItem* item) {
    return qobject_cast<const KeymapScopeAttached*>(qmlAttachedPropertiesObject<KeymapScope>(item, false));
}

/// Qt Quick Controls popups (menus, dialogs, combo box lists) live under the window's overlay.
bool isInPopup(const QQuickItem* item) {
    for (; item != nullptr; item = item->parentItem()) {
        if (item->inherits("QQuickPopupItem") || item->inherits("QQuickOverlay")) {
            return true;
        }
    }
    return false;
}

/// TextInput / TextEdit (and so TextField, TextArea and NumberField) set this flag while editable.
bool isTextEditor(const QQuickItem* item) {
    return item->flags().testFlag(QQuickItem::ItemAcceptsInputMethod);
}

} // namespace

// ---- KeymapScope ----------------------------------------------------------------------------------------

KeymapScopeAttached::KeymapScopeAttached(QObject* parent) : QObject(parent) {}

void KeymapScopeAttached::setScope(const QString& scope) {
    if (scope == m_scope) {
        return;
    }
    if (!scope.isEmpty() && !km::scopeFromId(scope.toStdString())) {
        qCWarning(lcKeymap) << "KeymapScope.scope: unknown scope" << scope << "on" << parent();
    }
    m_scope = scope;
    emit scopeChanged();
}

void KeymapScopeAttached::setSuppressShortcuts(bool suppress) {
    if (suppress == m_suppress) {
        return;
    }
    m_suppress = suppress;
    emit suppressShortcutsChanged();
}

KeymapScopeAttached* KeymapScope::qmlAttachedProperties(QObject* object) {
    return new KeymapScopeAttached(object);
}

// ---- KeymapDispatcher -----------------------------------------------------------------------------------

KeymapDispatcher::KeymapDispatcher(QObject* parent)
    : QObject(parent), m_platform(km::currentPlatform()), m_overridesPath(defaultOverridesPath(m_platform)) {
    m_gestureTimer.setInterval(kGestureTickMs);
    connect(&m_gestureTimer, &QTimer::timeout, this, [this] {
        deliver(m_dispatcher->tick());
        updateGestureTimer();
    });
    reload();
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::focusWindowChanged, this, &KeymapDispatcher::attachIfUnset);
        attachIfUnset(QGuiApplication::focusWindow());
    }
}

KeymapDispatcher::~KeymapDispatcher() {
    if (m_window) {
        m_window->removeEventFilter(this);
    }
    for (const QPointer<QQuickWindow>& window : std::as_const(m_panelWindows)) {
        if (window) {
            window->removeEventFilter(this);
        }
    }
}

void KeymapDispatcher::addWindow(QQuickWindow* window) {
    m_panelWindows.removeAll(nullptr);
    if (window == nullptr || window == m_window || m_panelWindows.contains(window)) {
        return;
    }
    m_panelWindows.append(window);
    window->installEventFilter(this);
    connect(window, &QWindow::activeChanged, this, [this, window] {
        if (!window->isActive()) {
            resetGesture();
        }
    });
}

void KeymapDispatcher::removeWindow(QQuickWindow* window) {
    if (window == nullptr || !m_panelWindows.contains(window)) {
        return;
    }
    m_panelWindows.removeAll(window);
    window->removeEventFilter(this);
    disconnect(window, nullptr, this, nullptr);
    resetGesture();
}

void KeymapDispatcher::setWindow(QQuickWindow* window) {
    m_explicitWindow = true;
    if (window == m_window) {
        return;
    }
    if (m_window) {
        m_window->removeEventFilter(this);
        disconnect(m_window, nullptr, this, nullptr);
    }
    resetGesture();
    m_window = window;
    if (m_window) {
        m_window->installEventFilter(this);
        connect(m_window, &QWindow::activeChanged, this, [this] {
            if (m_window && !m_window->isActive()) {
                resetGesture();
            }
        });
    }
    emit windowChanged();
}

void KeymapDispatcher::attachIfUnset(QWindow* window) {
    auto* quickWindow = qobject_cast<QQuickWindow*>(window);
    if (m_explicitWindow || m_window || quickWindow == nullptr || quickWindow->transientParent() != nullptr) {
        return;
    }
    setWindow(quickWindow);
    m_explicitWindow = false; // keep following explicit assignments only
}

QString KeymapDispatcher::platform() const {
    return toQString(km::platformId(m_platform));
}

void KeymapDispatcher::setPlatform(const QString& platform) {
    const auto parsed = km::platformFromId(platform.toStdString());
    if (!parsed) {
        qCWarning(lcKeymap) << "KeymapDispatcher.platform: unknown platform" << platform;
        return;
    }
    if (*parsed == m_platform) {
        return;
    }
    m_platform = *parsed;
    if (!m_customOverridesPath) {
        m_overridesPath = defaultOverridesPath(m_platform);
    }
    reload();
    changed();
}

void KeymapDispatcher::setOverridesPath(const QString& path) {
    m_customOverridesPath = true;
    if (path == m_overridesPath) {
        return;
    }
    m_overridesPath = path;
    reload();
    changed();
}

void KeymapDispatcher::reload() {
    if (m_dispatcher) {
        deliver(m_dispatcher->focusLost()); // the dispatcher is about to be replaced: end any slow play
    }
    QFile file(m_overridesPath);
    QByteArray data;
    bool readable = false;
    if (file.exists()) {
        readable = file.open(QIODevice::ReadOnly);
        if (readable) {
            data = file.readAll();
        }
    }
    // The run-time commands, and the "ext:" ids the saved overrides bind (kept while their extension is gone).
    QSet<QString> bound;
    const QJsonObject saved = QJsonDocument::fromJson(data).object().value(QStringLiteral("overrides")).toObject();
    for (auto it = saved.begin(); it != saved.end(); ++it) {
        if (it.key().startsWith(QLatin1String("ext:"))) {
            bound.insert(it.key());
        }
    }
    if (m_registry) {
        m_retiredRegistries.push_back(std::move(m_registry));
    }
    m_registry = buildRegistry(bound);
    const km::CommandRegistry& registry = m_registry ? *m_registry->registry : km::CommandRegistry::builtin();

    auto defaults = km::KeymapManager::withDefaults(m_platform, registry);
    if (defaults) {
        m_keymap = std::make_unique<km::KeymapManager>(std::move(*defaults));
    } else {
        qCCritical(lcKeymap) << "built-in keymap failed to load:" << toQString(defaults.error().message);
        m_keymap = std::make_unique<km::KeymapManager>(m_platform, registry);
    }

    if (file.exists()) {
        if (readable) {
            const auto loaded = m_keymap->loadOverrides(
                std::string_view(data.constData(), static_cast<std::size_t>(data.size())));
            if (!loaded) {
                qCWarning(lcKeymap) << "ignoring keyboard shortcut overrides in" << m_overridesPath << ":"
                                    << toQString(loaded.error().message);
            } else {
                for (const std::string& warning : loaded->warnings) {
                    qCWarning(lcKeymap) << m_overridesPath << ":" << toQString(warning);
                }
            }
        } else {
            qCWarning(lcKeymap) << "cannot read" << m_overridesPath << ":" << file.errorString();
        }
    }

    m_dispatcher = std::make_unique<km::KeyDispatcher>(*m_keymap);
    m_consumedKeys.clear();
    updateGestureTimer();
}

bool KeymapDispatcher::saveOverrides() {
    if (!m_keymap->hasOverrides()) {
        if (QFile::exists(m_overridesPath) && !QFile::remove(m_overridesPath)) {
            qCWarning(lcKeymap) << "cannot remove" << m_overridesPath;
            return false;
        }
        return true;
    }
    QDir().mkpath(QFileInfo(m_overridesPath).absolutePath());
    QSaveFile file(m_overridesPath);
    const std::string json = m_keymap->saveOverrides();
    if (!file.open(QIODevice::WriteOnly) || file.write(json.data(), static_cast<qint64>(json.size())) < 0 ||
        !file.commit()) {
        qCWarning(lcKeymap) << "cannot save keyboard shortcuts to" << m_overridesPath << ":"
                            << file.errorString();
        return false;
    }
    return true;
}

void KeymapDispatcher::changed() {
    ++m_revision;
    emit keymapChanged();
}

// ---- Run-time commands (C.12 extensions) ------------------------------------------------------------------

std::unique_ptr<KeymapDispatcher::Registry> KeymapDispatcher::buildRegistry(const QSet<QString>& bound) const {
    const km::CommandRegistry& builtin = km::CommandRegistry::builtin();
    QList<ExtraCommand> extra;
    QSet<QString> ids;
    for (const ExtraCommand& command : m_extra) {
        if (!command.id.isEmpty() && !ids.contains(command.id) && !builtin.contains(command.id.toStdString())) {
            ids.insert(command.id);
            extra.append(command);
        }
    }
    QStringList leftovers(bound.begin(), bound.end());
    leftovers.sort();
    for (const QString& id : leftovers) {
        if (!ids.contains(id) && !builtin.contains(id.toStdString())) {
            ids.insert(id);
            //: A keyboard shortcut's command whose extension is off or not installed any more
            extra.append(ExtraCommand{id, tr("%1 (unavailable)").arg(id)});
        }
    }
    if (extra.isEmpty()) {
        return nullptr;
    }
    auto out = std::make_unique<Registry>();
    out->commands.assign(builtin.all().begin(), builtin.all().end());
    for (const ExtraCommand& command : extra) {
        const std::string& id = out->strings.emplace_back(command.id.toStdString());
        const std::string& name = out->strings.emplace_back(command.title.toStdString());
        out->commands.push_back(km::CommandInfo{id, km::Scope::Application, "Extensions", name, "", "", "", false});
    }
    out->registry.emplace(out->commands);
    return out;
}

void KeymapDispatcher::setExtraCommands(const QList<ExtraCommand>& commands) {
    if (commands == m_extra) {
        return;
    }
    m_extra = commands;
    QSet<QString> bound;
    const std::vector<km::CommandOverride> overrides = m_keymap->overrides();
    for (const km::CommandOverride& o : overrides) {
        const QString id = toQString(o.command);
        if (id.startsWith(QLatin1String("ext:"))) {
            bound.insert(id);
        }
    }
    std::unique_ptr<Registry> registry = buildRegistry(bound);
    auto next = km::KeymapManager::withDefaults(m_platform, registry ? *registry->registry : km::CommandRegistry::builtin());
    if (!next) {
        qCWarning(lcKeymap) << "extension commands not added:" << toQString(next.error().message);
        return;
    }
    if (auto applied = next->applyOverrides(overrides); !applied) {
        qCWarning(lcKeymap) << "extension commands not added:" << toQString(applied.error().message);
        return;
    }
    // A new dispatcher: the old one remembers held keys by their (old) registry entries.
    deliver(m_dispatcher->focusLost());
    *m_keymap = std::move(*next);
    m_dispatcher = std::make_unique<km::KeyDispatcher>(*m_keymap);
    m_consumedKeys.clear();
    updateGestureTimer();
    if (m_registry) {
        m_retiredRegistries.push_back(std::move(m_registry));
    }
    m_registry = std::move(registry);
    changed();
}

bool KeymapDispatcher::isExtraCommandAvailable(const QString& commandId) const {
    return std::ranges::any_of(m_extra, [&](const ExtraCommand& c) { return c.id == commandId; });
}

// ---- Queries and commands from QML ----------------------------------------------------------------------

QString KeymapDispatcher::shortcutText(const QString& commandId) const {
    return toQString(m_keymap->shortcutText(commandId.toStdString(), km::ChordFormat::Native));
}

QStringList KeymapDispatcher::shortcutTexts(const QString& commandId) const {
    QStringList out;
    for (const km::KeyChord& chord : m_keymap->chordsFor(commandId.toStdString())) {
        out << toQString(km::formatKeyChord(chord, m_platform, km::ChordFormat::Native));
    }
    return out;
}

bool KeymapDispatcher::trigger(const QString& commandId) {
    const km::KeyResult result = m_dispatcher->trigger(commandId.toStdString());
    deliver(result);
    return result.handled;
}

QString KeymapDispatcher::scopeAt(QQuickItem* item) const {
    return toQString(km::scopeId(scopeFor(item)));
}

void KeymapDispatcher::syncTransportSpeed(int speedNum, int speedDen) {
    if (speedDen <= 0) {
        qCWarning(lcKeymap) << "syncTransportSpeed: the denominator must be positive";
        return;
    }
    m_dispatcher->syncTransportSpeed({speedNum, speedDen});
}

QVariantMap KeymapDispatcher::assignShortcut(const QString& commandId, const QString& chordText) {
    const auto fail = [](const std::string& message) {
        return QVariantMap{{QStringLiteral("ok"), false},
                           {QStringLiteral("lost"), QStringList()},
                           {QStringLiteral("error"), toQString(message)}};
    };
    const auto chord = km::parseKeyChord(chordText.toStdString(), m_platform);
    if (!chord) {
        return fail(chord.error().message);
    }
    const auto lost = m_keymap->assign(commandId.toStdString(), *chord);
    if (!lost) {
        return fail(lost.error().message);
    }
    QStringList lostIds;
    for (const km::CommandId& id : *lost) {
        lostIds << toQString(id);
    }
    const bool saved = saveOverrides();
    changed();
    return {{QStringLiteral("ok"), saved},
            {QStringLiteral("lost"), lostIds},
            {QStringLiteral("error"),
             saved ? QString() : QStringLiteral("could not save the keyboard shortcuts")}};
}

bool KeymapDispatcher::resetShortcut(const QString& commandId) {
    const std::string id = commandId.toStdString();
    if (!m_keymap->isOverridden(id)) {
        return false;
    }
    m_keymap->resetToDefault(id);
    saveOverrides();
    changed();
    return true;
}

void KeymapDispatcher::resetAllShortcuts() {
    m_keymap->resetAllToDefaults();
    saveOverrides();
    changed();
}

QString KeymapDispatcher::importPremiereKeySet(const QUrl& file) {
    QFile input(file.isLocalFile() ? file.toLocalFile() : file.toString());
    if (!input.open(QIODevice::ReadOnly)) {
        return input.errorString();
    }
    const QByteArray xml = input.readAll();
    const auto imported =
        km::parseKys(std::string_view(xml.constData(), static_cast<std::size_t>(xml.size())));
    if (!imported) {
        return toQString(imported.error().message);
    }
    if (const auto applied = km::applyKysImport(*m_keymap, *imported); !applied) {
        return toQString(applied.error().message);
    }
    for (const std::string& warning : imported->warnings) {
        qCWarning(lcKeymap) << "Premiere key set:" << toQString(warning);
    }
    const bool saved = saveOverrides();
    changed();
    return saved ? QString() : QStringLiteral("could not save the keyboard shortcuts");
}

// ---- Keyboard Shortcuts editor (M5f) --------------------------------------------------------------------

std::expected<void, Error> KeymapDispatcher::setOverrides(std::span<const km::CommandOverride> overrides) {
    // On a copy: the KeyDispatcher keeps referring to *m_keymap, which is replaced in place only on success.
    km::KeymapManager next = *m_keymap;
    next.resetAllToDefaults();
    if (auto applied = next.applyOverrides(overrides); !applied) {
        return std::unexpected(std::move(applied.error()));
    }
    *m_keymap = std::move(next);
    saveOverrides();
    changed();
    return {};
}

// ---- Key routing ----------------------------------------------------------------------------------------

bool KeymapDispatcher::eventFilter(QObject* watched, QEvent* event) {
    QQuickWindow* window = nullptr;
    if (watched == m_window) {
        window = m_window;
    } else if (auto* panelWindow = qobject_cast<QQuickWindow*>(watched);
               panelWindow && m_panelWindows.contains(panelWindow)) {
        window = panelWindow;
    }
    if (window) {
        switch (event->type()) {
        case QEvent::KeyPress:
            return handleKeyPress(static_cast<QKeyEvent*>(event), window);
        case QEvent::KeyRelease:
            return handleKeyRelease(static_cast<QKeyEvent*>(event));
        case QEvent::FocusOut:
        case QEvent::WindowDeactivate:
        case QEvent::Hide:
            resetGesture(); // the releases of held keys will not reach this window
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

bool KeymapDispatcher::handleKeyPress(QKeyEvent* event, QQuickWindow* window) {
    if (event->key() == 0 || km::isModifierKey(event->key()) || event->key() == Qt::Key_unknown) {
        return false;
    }
    QQuickItem* focus = window->activeFocusItem();
    if (isSuppressed(event, focus)) {
        return false;
    }
    // Tab and Shift+Tab always move keyboard focus (M5j, accessibility: every panel can be left with the keyboard).
    // Premiere binds them in the Project panel to step between a rename's column fields, which Wabcut's Library does
    // not have; Ctrl+Tab and other chords with Tab still resolve.
    if ((event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab) &&
        !(event->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier))) {
        return false;
    }
    const auto chord = chordOf(event);
    if (!chord) {
        return false;
    }
    const km::KeyResult result = m_dispatcher->keyPress(scopeFor(focus), *chord, event->isAutoRepeat());
    if (result.handled) {
        m_consumedKeys.insert(chord->normalized().key);
        event->accept();
    }
    deliver(result);
    updateGestureTimer();
    return result.handled;
}

bool KeymapDispatcher::handleKeyRelease(QKeyEvent* event) {
    if (event->key() == 0 || km::isModifierKey(event->key()) || event->key() == Qt::Key_unknown) {
        return false;
    }
    // Releases always reach the dispatcher, even when focus moved into a text field meanwhile: a held K must
    // still come up.
    const km::KeyChord chord =
        km::KeyChord::fromQt(event->key(), static_cast<std::uint32_t>(event->modifiers().toInt()));
    const int key = chord.normalized().key;
    const bool consumed = m_consumedKeys.contains(key);
    if (consumed && !event->isAutoRepeat()) {
        m_consumedKeys.remove(key);
    }
    deliver(m_dispatcher->keyRelease(chord, event->isAutoRepeat()));
    updateGestureTimer();
    if (consumed) {
        event->accept();
    }
    return consumed;
}

bool KeymapDispatcher::isSuppressed(QKeyEvent* event, QQuickItem* focus) const {
    if (focus == nullptr) {
        return false;
    }
    // Popups and dialogs (Qt Quick Controls put them under the overlay) and opted-out items keep every key.
    if (isInPopup(focus)) {
        return true;
    }
    for (const QQuickItem* item = focus; item != nullptr; item = item->parentItem()) {
        if (const KeymapScopeAttached* scope = attachedScope(item); scope && scope->suppressShortcuts()) {
            return true;
        }
    }
    // IME composition in progress.
    if (!focus->property("preeditText").toString().isEmpty()) {
        return true;
    }
    // The focused item (or an ancestor) claims the key, as Qt's shortcut system would ask it to.
    if (focusClaimsKey(event, focus)) {
        return true;
    }
    // A button the keyboard moved to (Tab: visual focus) takes Space, as buttons do everywhere (M5j: keyboard-only
    // use). A button focused by a click leaves Space to Play/Stop, Premiere's habit.
    if (event->key() == Qt::Key_Space && !(event->modifiers() & ~Qt::KeypadModifier) &&
        focus->inherits("QQuickAbstractButton") && focus->property("visualFocus").toBool()) {
        return true;
    }
    // Text editing (TextField, TextArea, NumberField): typing, navigation (NumberField's Up/Down), Enter and
    // Alt/Option characters stay in the editor. Ctrl/Cmd shortcuts the editor did not claim (Save, Export...)
    // still work.
    return isTextEditor(focus) && !(event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier));
}

bool KeymapDispatcher::focusClaimsKey(QKeyEvent* event, QQuickItem* focus) const {
    // Same delivery as Qt Quick's: the focused item first, then its parents, until one accepts.
    QKeyEvent probe(QEvent::ShortcutOverride, event->key(), event->modifiers(), event->nativeScanCode(),
                    event->nativeVirtualKey(), event->nativeModifiers(), event->text(), event->isAutoRepeat(),
                    static_cast<quint16>(event->count()));
    for (QQuickItem* item = focus; item != nullptr; item = item->parentItem()) {
        probe.accept();
        QCoreApplication::sendEvent(item, &probe);
        if (probe.isAccepted()) {
            return true;
        }
    }
    return false;
}

std::optional<km::KeyChord> KeymapDispatcher::chordOf(const QKeyEvent* event) const {
    const Qt::KeyboardModifiers mods = event->modifiers();
    // AltGr: on Windows it arrives as Ctrl+Alt together with the character it types ("€", "@", "{"). That is
    // typing, never a Ctrl+Alt shortcut. (A real Ctrl+Alt+key carries no printable text.) macOS has no AltGr:
    // Option+key shortcuts are real there.
    if (!km::usesMacModifiers(m_platform) && mods.testFlag(Qt::ControlModifier) &&
        mods.testFlag(Qt::AltModifier)) {
        const QString text = event->text();
        if (!text.isEmpty() && text.at(0).isPrint()) {
            return std::nullopt;
        }
    }
    return km::KeyChord::fromQt(event->key(), static_cast<std::uint32_t>(mods.toInt()));
}

km::Scope KeymapDispatcher::scopeFor(QQuickItem* item) const {
    for (; item != nullptr; item = item->parentItem()) {
        if (const KeymapScopeAttached* attached = attachedScope(item);
            attached && !attached->scope().isEmpty()) {
            if (const auto scope = km::scopeFromId(attached->scope().toStdString())) {
                return *scope;
            }
        }
    }
    return km::Scope::Application;
}

void KeymapDispatcher::deliver(const km::KeyResult& result) {
    deliver(result.transport);
    for (const km::CommandId& id : result.commands) {
        emit commandTriggered(toQString(id));
    }
}

void KeymapDispatcher::deliver(const std::vector<km::TransportIntent>& intents) {
    for (const km::TransportIntent& intent : intents) {
        emit transportIntent(kindOf(intent.kind), intent.speed.num, intent.speed.den, intent.frames);
    }
}

void KeymapDispatcher::updateGestureTimer() {
    if (m_dispatcher->isGestureActive()) {
        if (!m_gestureTimer.isActive()) {
            m_gestureTimer.start();
        }
    } else {
        m_gestureTimer.stop();
    }
}

void KeymapDispatcher::resetGesture() {
    if (!m_dispatcher) {
        return;
    }
    m_consumedKeys.clear();
    deliver(m_dispatcher->focusLost());
    updateGestureTimer();
}

} // namespace wabcut::app
