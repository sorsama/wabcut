#include "KeyboardTrainerBridge.h"

#include "KeymapDispatcher.h"

#include <wabcut/keymap/CommandRegistry.h>
#include <wabcut/keymap/Key.h>

#include <QQmlEngine>

#include <algorithm>

namespace wabcut::app {

namespace km = wabcut::keymap;

namespace {

constexpr int kRoundLength = 10;

// A custom map may bind a navigation key, or contain a conflict whose winning command is different.
// Only offer answers the trainer can actually receive and resolve; Escape and plain Tab retain navigation.
std::vector<km::KeyChord> playableChords(const km::KeymapManager& keys, std::string_view command) {
    const km::CommandInfo* info = keys.registry().find(command);
    if (!info)
        return {};
    auto chords = keys.chordsFor(command);
    std::erase_if(chords, [&](const km::KeyChord& chord) {
        const auto mods = chord.mods.bits();
        constexpr auto navigationMods = static_cast<std::uint32_t>(Qt::ShiftModifier)
                                      | static_cast<std::uint32_t>(Qt::KeypadModifier);
        const bool plain = (mods & ~navigationMods) == 0;
        const bool navigation = chord.key == Qt::Key_Escape
            || (plain && (chord.key == Qt::Key_Tab || chord.key == Qt::Key_Backtab));
        return navigation || keys.resolve(info->scope, chord) != info->id;
    });
    return chords;
}

} // namespace

const std::vector<KeyboardTrainerBridge::Prompt>& KeyboardTrainerBridge::allPrompts() {
    // Premiere's everyday keys, by what an editor does with them. M5n: copy and paste (Paste Attributes), trim, the
    // markers beside the marks, and the multi-camera keys. Every prompt has a key in both of Premiere's default sets.
    static const std::vector<Prompt> prompts{
        // Editing
        {"edit.undo", "editing"},
        {"edit.redo", "editing"},
        {"edit.rippleDelete", "editing"},
        {"edit.clear", "editing"},
        {"sequence.addEdit", "editing"},
        {"sequence.addEditToAllTracks", "editing"},
        {"clip.insert", "editing"},
        {"clip.overwrite", "editing"},
        {"sequence.lift", "editing"},
        {"sequence.extract", "editing"},
        {"sequence.extendEditToPlayhead", "editing"},
        {"clip.speedDuration", "editing"},
        {"clip.increaseVolume", "editing"}, // 0.4: ] and [ (Increase / Decrease Clip Volume)
        {"clip.decreaseVolume", "editing"},
        // Copy and paste (C.11: Paste Attributes; Remove Attributes has no key, as in Premiere)
        {"edit.cut", "clipboard"},
        {"edit.copy", "clipboard"},
        {"edit.paste", "clipboard"},
        {"edit.pasteInsert", "clipboard"},
        {"edit.pasteAttributes", "clipboard"},
        {"edit.duplicate", "clipboard"},
        {"edit.selectAll", "clipboard"},
        {"edit.deselectAll", "clipboard"},
        // Marks and markers
        {"marker.markIn", "marks"},
        {"marker.markOut", "marks"},
        {"marker.markClip", "marks"},
        {"marker.markSelection", "marks"},
        {"marker.goToIn", "marks"},
        {"marker.goToOut", "marks"},
        {"marker.clearIn", "marks"},
        {"marker.clearOut", "marks"},
        {"marker.clearInAndOut", "marks"},
        {"marker.add", "marks"},
        {"marker.goToNext", "marks"},
        {"marker.goToPrevious", "marks"},
        {"marker.clearSelected", "marks"},
        {"marker.clearAll", "marks"},
        // Trim (M5i's trim mode and the ripple trims to the playhead)
        {"sequence.trimEdit", "trim"},
        {"sequence.toggleTrimType", "trim"},
        {"sequence.trimBackward", "trim"},
        {"sequence.trimForward", "trim"},
        {"sequence.trimBackwardMany", "trim"},
        {"sequence.trimForwardMany", "trim"},
        {"sequence.rippleTrimPreviousToPlayhead", "trim"},
        {"sequence.rippleTrimNextToPlayhead", "trim"},
        // Playback and navigation
        {"transport.playStop", "playback"},
        {"transport.shuttleForward", "playback"},
        {"transport.shuttleBackward", "playback"},
        {"transport.shuttleStop", "playback"},
        {"transport.stepForward", "playback"},
        {"transport.stepBackward", "playback"},
        {"transport.goToStart", "playback"},
        {"transport.goToEnd", "playback"},
        {"transport.goToNextEdit", "playback"},
        {"transport.goToPreviousEdit", "playback"},
        {"sequence.matchFrame", "playback"},
        {"transport.shuttleSlowForward", "playback"},    // 0.4: Shift+L
        {"transport.playAround", "playback"},            // 0.4: Shift+K
        {"transport.playFromPlayheadToOut", "playback"}, // 0.4: Ctrl+Space
        // Tools
        {"tool.selection", "tools"},
        {"tool.trackSelectForward", "tools"},
        {"tool.rippleEdit", "tools"},
        {"tool.rollingEdit", "tools"},
        {"tool.rateStretch", "tools"},
        {"tool.razor", "tools"},
        {"tool.slip", "tools"},
        {"tool.slide", "tools"},
        {"tool.pen", "tools"},
        {"tool.hand", "tools"},
        {"tool.zoom", "tools"},
        // The timeline
        {"sequence.zoomIn", "timeline"},
        {"sequence.zoomOut", "timeline"},
        {"sequence.zoomToSequence", "timeline"},
        {"sequence.snap", "timeline"},
        {"sequence.selectClipAtPlayhead", "timeline"},
        {"sequence.applyDefaultTransitions", "timeline"},
        {"timeline.nudgeClipLeft", "timeline"},
        {"timeline.nudgeClipRight", "timeline"},
        {"timeline.nudgeClipUp", "timeline"},   // 0.4: Alt+Up / Opt+Up
        {"timeline.nudgeClipDown", "timeline"}, // 0.4: Alt+Down / Opt+Down
        // Multi-camera (M5l: select an angle, cut to it, the multi-camera view; Cut to Camera 9 has no default key)
        {"multicam.selectCamera1", "multicam"},
        {"multicam.selectCamera2", "multicam"},
        {"multicam.selectCamera3", "multicam"},
        {"multicam.selectCamera4", "multicam"},
        {"multicam.selectCamera5", "multicam"},
        {"multicam.selectCamera6", "multicam"},
        {"multicam.selectCamera7", "multicam"},
        {"multicam.selectCamera8", "multicam"},
        {"multicam.selectCamera9", "multicam"},
        {"multicam.cutToCamera1", "multicam"},
        {"multicam.cutToCamera2", "multicam"},
        {"multicam.cutToCamera3", "multicam"},
        {"multicam.cutToCamera4", "multicam"},
        {"multicam.cutToCamera5", "multicam"},
        {"multicam.cutToCamera6", "multicam"},
        {"multicam.cutToCamera7", "multicam"},
        {"multicam.cutToCamera8", "multicam"},
        {"multicam.toggleView", "multicam"},
    };
    return prompts;
}

KeyboardTrainerBridge::KeyboardTrainerBridge(QObject* parent) : QObject(parent) {}

KeyboardTrainerBridge* KeyboardTrainerBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new KeyboardTrainerBridge;
    if (qmlEngine)
        bridge->setDispatcher(qmlEngine->singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher"));
    return bridge;
}

void KeyboardTrainerBridge::setDispatcher(KeymapDispatcher* dispatcher) {
    if (m_dispatcher)
        disconnect(m_dispatcher, nullptr, this, nullptr);
    m_dispatcher = dispatcher;
    if (dispatcher)
        connect(dispatcher, &KeymapDispatcher::keymapChanged, this, &KeyboardTrainerBridge::keymapChanged);
    emit keymapChanged();
}

void KeyboardTrainerBridge::setKeymap(const km::KeymapManager* keymap) {
    m_keymap = keymap;
    emit keymapChanged();
}

const km::KeymapManager* KeyboardTrainerBridge::keymap() const {
    if (m_keymap)
        return m_keymap;
    return m_dispatcher ? &m_dispatcher->keymap() : nullptr;
}

QVariantList KeyboardTrainerBridge::categories() const {
    const struct {
        const char* key;
        QString label;
    } list[] = {{"all", tr("All")},           {"editing", tr("Editing")},
                {"clipboard", tr("Clipboard")},
                //: Keyboard Trainer category: Mark In / Out and the markers
                {"marks", tr("Markers")},
                {"trim", tr("Trim")},         {"playback", tr("Playback")},
                {"tools", tr("Tools")},       {"timeline", tr("Timeline")},
                {"multicam", tr("Multicam")}};
    const km::KeymapManager* keys = keymap();
    QVariantList out;
    for (const auto& c : list) {
        int count = 0;
        for (const Prompt& p : allPrompts()) {
            if ((std::string_view(c.key) == "all" || std::string_view(c.key) == p.category) && keys &&
                !playableChords(*keys, p.command).empty())
                ++count;
        }
        out << QVariantMap{{QStringLiteral("key"), QString::fromLatin1(c.key)},
                           {QStringLiteral("label"), c.label},
                           {QStringLiteral("count"), count}};
    }
    return out;
}

const km::CommandInfo* KeyboardTrainerBridge::currentInfo() const {
    const km::KeymapManager* keys = keymap();
    if (!keys || m_index < 0 || m_index >= static_cast<int>(m_queue.size()))
        return nullptr;
    return keys->registry().find(m_queue[static_cast<std::size_t>(m_index)]);
}

QString KeyboardTrainerBridge::prompt() const {
    const km::CommandInfo* info = currentInfo();
    return info ? QString::fromUtf8(info->displayName.data(), static_cast<qsizetype>(info->displayName.size())) : QString();
}

QString KeyboardTrainerBridge::promptGroup() const {
    const km::CommandInfo* info = currentInfo();
    return info ? QString::fromUtf8(info->group.data(), static_cast<qsizetype>(info->group.size())) : QString();
}

QString KeyboardTrainerBridge::promptCommand() const {
    const km::CommandInfo* info = currentInfo();
    return info ? QString::fromUtf8(info->id.data(), static_cast<qsizetype>(info->id.size())) : QString();
}

QString KeyboardTrainerBridge::chordText(const km::KeyChord& chord) const {
    const km::KeymapManager* keys = keymap();
    return keys ? QString::fromStdString(km::formatKeyChord(chord, keys->platform(), km::ChordFormat::Native)) : QString();
}

QString KeyboardTrainerBridge::hint() const {
    if (!m_hintShown)
        return {};
    const km::KeymapManager* keys = keymap();
    const km::CommandInfo* info = currentInfo();
    if (!keys || !info)
        return {};
    const auto chords = playableChords(*keys, info->id);
    return chords.empty() ? QString() : chordText(chords.front());
}

void KeyboardTrainerBridge::setState(const QString& state) {
    m_state = state;
    emit stateChanged();
}

bool KeyboardTrainerBridge::start(const QString& category) {
    const km::KeymapManager* keys = keymap();
    std::vector<std::string> pool;
    for (const Prompt& p : allPrompts()) {
        if ((category == QLatin1String("all") || category == QLatin1String(p.category)) && keys &&
            !playableChords(*keys, p.command).empty())
            pool.push_back(p.command);
    }
    if (pool.empty())
        return false;
    std::shuffle(pool.begin(), pool.end(), m_random);
    pool.resize(std::min<std::size_t>(pool.size(), kRoundLength));
    m_queue = std::move(pool);
    m_category = category;
    m_index = 0;
    m_hintShown = false;
    m_missed = false;
    m_lastPressed.clear();
    m_lastPressedAction.clear();
    m_correct = 0;
    m_streak = 0;
    m_score = 0;
    m_lastPoints = 0;
    m_lastMs = 0;
    m_totalMs = 0;
    m_answered = 0;
    m_clock.start();
    setState(QStringLiteral("asking"));
    return true;
}

QString KeyboardTrainerBridge::press(int key, int modifiers) {
    if ((m_state != QLatin1String("asking") && m_state != QLatin1String("wrong")) || key == 0 ||
        km::isModifierKey(key) || key == Qt::Key_unknown)
        return QStringLiteral("ignored");
    const km::KeymapManager* keys = keymap();
    const km::CommandInfo* info = currentInfo();
    if (!keys || !info)
        return QStringLiteral("ignored");
    const km::KeyChord chord = km::KeyChord::fromQt(key, static_cast<std::uint32_t>(modifiers)).normalized();
    std::vector<km::KeyChord> candidates{chord};
    if (const auto unshifted = km::unshiftedUsLayout(chord))
        candidates.push_back(*unshifted);
    // The answer is right when the key runs the command in the command's own scope (as when its panel has focus).
    bool right = false;
    std::optional<km::CommandId> runs;
    for (const km::KeyChord& c : candidates) {
        const auto resolved = keys->resolve(info->scope, c);
        if (resolved && !runs)
            runs = resolved;
        right = right || (resolved && *resolved == info->id);
    }
    m_lastPressed = chordText(candidates.back());
    m_lastPressedAction.clear();
    if (runs) {
        if (const km::CommandInfo* other = keys->registry().find(*runs))
            m_lastPressedAction = QString::fromUtf8(other->displayName.data(), static_cast<qsizetype>(other->displayName.size()));
    }
    if (!right) {
        m_missed = true;
        m_streak = 0;
        setState(QStringLiteral("wrong"));
        return QStringLiteral("wrong");
    }
    const qint64 ms = m_clock.elapsed();
    m_lastMs = static_cast<int>(ms);
    m_totalMs += ms;
    ++m_answered;
    if (!m_missed)
        ++m_correct;
    ++m_streak;
    m_best = std::max(m_best, m_streak);
    int points = 100 + static_cast<int>(std::max<qint64>(0, 50 - ms / 40)) + 10 * (m_streak - 1);
    if (m_hintShown)
        points /= 2;
    m_lastPoints = points;
    m_score += points;
    setState(QStringLiteral("correct"));
    emit answeredCorrectly();
    return QStringLiteral("correct");
}

void KeyboardTrainerBridge::showHint() {
    if (m_state != QLatin1String("asking") && m_state != QLatin1String("wrong"))
        return;
    m_hintShown = true;
    m_missed = true;
    emit stateChanged();
}

void KeyboardTrainerBridge::next() {
    if (m_state == QLatin1String("idle") || m_state == QLatin1String("finished"))
        return;
    if (m_state != QLatin1String("correct"))
        m_streak = 0; // skipped
    ++m_index;
    m_hintShown = false;
    m_missed = false;
    m_lastPressed.clear();
    m_lastPressedAction.clear();
    if (m_index >= static_cast<int>(m_queue.size())) {
        m_index = static_cast<int>(m_queue.size()) - 1;
        setState(QStringLiteral("finished"));
        return;
    }
    m_clock.restart();
    setState(QStringLiteral("asking"));
}

void KeyboardTrainerBridge::stop() {
    m_queue.clear();
    m_index = 0;
    setState(QStringLiteral("idle"));
}

void KeyboardTrainerBridge::setSavedBest(int best) {
    if (best > m_best) {
        m_best = best;
        emit stateChanged();
    }
}

} // namespace wabcut::app
