#include "CommandPalette.h"

#include "AppSettingsBridge.h"
#include "ClipAudioBridge.h"
#include "CommandBridge.h"
#include "EditingBridge.h"
#include "EffectControls.h"
#include "ExtensionsBridge.h"
#include "KeymapDispatcher.h"
#include "LibraryBridge.h"
#include "MonitorBridge.h"
#include "MulticamEditBridge.h"
#include "ProjectController.h"
#include "Titles.h"

#include <wabcut/keymap/CommandRegistry.h>

#include <QQmlEngine>
#include <QSet>
#include <QSettings>

#include <algorithm>

namespace wabcut::app {

namespace {

constexpr int kMaxRecent = 8;
constexpr int kMaxResults = 80;

bool isBoundary(QStringView text, qsizetype i) {
    if (i == 0)
        return true;
    const QChar prev = text[i - 1];
    const QChar cur = text[i];
    if (!prev.isLetterOrNumber())
        return true;
    return prev.isLower() && cur.isUpper(); // camelCase
}

QString normalizedShortcut(QString text) {
    text = text.toLower();
    text.remove(QLatin1Char(' '));
    return text;
}

int initialsScore(QStringView pattern, QStringView text) {
    QString initials;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i].isLetterOrNumber() && isBoundary(text, i))
            initials += text[i].toLower();
    }
    return !pattern.isEmpty() && initials.startsWith(pattern.toString().toLower()) ? 40 * static_cast<int>(pattern.size()) : 0;
}

} // namespace

int fuzzyScore(QStringView pattern, QStringView text) {
    if (pattern.isEmpty())
        return 1;
    int score = 0;
    qsizetype p = 0;
    qsizetype previous = -2;
    qsizetype first = -1;
    for (qsizetype i = 0; i < text.size() && p < pattern.size(); ++i) {
        if (text[i].toLower() != pattern[p].toLower())
            continue;
        int s = 10;
        if (isBoundary(text, i))
            s += 20;
        if (previous == i - 1)
            s += 15;
        score += s;
        if (first < 0)
            first = i;
        previous = i;
        ++p;
    }
    if (p < pattern.size())
        return 0;
    score -= static_cast<int>(std::min<qsizetype>(first, 20)); // an early first match is better
    return std::max(1, score);
}

// ---- Providers ------------------------------------------------------------------------------------------------------

QVariantMap PaletteProvider::preview(const PaletteItem&, const QVariantMap&) {
    return {};
}

KeymapCommandProvider::KeymapCommandProvider(std::vector<Command> commands,
                                             std::function<QString(const QString&)> shortcutText,
                                             std::function<QVariantMap(const QString&)> availability,
                                             std::function<bool(const QString&)> trigger)
    : m_commands(std::move(commands)), m_shortcutText(std::move(shortcutText)),
      m_availability(std::move(availability)), m_trigger(std::move(trigger)) {}

bool KeymapCommandProvider::viewHandles(const QString& id) {
    // Main.qml's router: the timeline view (zoom, tools, snap), the playback/workspace commands and the File menu
    // (ProjectFile, I.1).
    static const QSet<QString> kView{
        QStringLiteral("sequence.zoomIn"),           QStringLiteral("sequence.zoomOut"),
        QStringLiteral("sequence.zoomToSequence"),   QStringLiteral("sequence.snap"),
        QStringLiteral("tool.selection"),            QStringLiteral("tool.trackSelectForward"),
        QStringLiteral("tool.trackSelectBackward"),  QStringLiteral("tool.rippleEdit"),
        QStringLiteral("tool.rollingEdit"),          QStringLiteral("tool.slip"),
        QStringLiteral("tool.slide"),                QStringLiteral("tool.razor"),
        QStringLiteral("tool.hand"),                 QStringLiteral("tool.zoom"), // 0.1.1: the timeline's view tools
        QStringLiteral("tool.rateStretch"),          QStringLiteral("tool.pen"),  // 0.1.2
        QStringLiteral("tool.type"),                 // 0.1.2: the Program Monitor's Type tool
        QStringLiteral("transport.playStop"),        QStringLiteral("file.exportMedia"),
        QStringLiteral("transport.shuttleBackward"), QStringLiteral("transport.shuttleStop"),
        QStringLiteral("transport.shuttleForward"),
        QStringLiteral("clip.speedDuration"), // opens the Speed/Duration dialog
        QStringLiteral("clip.audioGain"),     // 0.1.2: G, the Audio Gain dialog
        QStringLiteral("clip.audioChannels"), // 0.1.2: Shift+G, the Audio Channels dialog
        QStringLiteral("file.newProject"),           QStringLiteral("file.openProject"),
        QStringLiteral("file.save"),                 QStringLiteral("file.saveAs"),
        QStringLiteral("file.import"),               QStringLiteral("file.exit"),
        QStringLiteral("edit.keyboardShortcuts"),    // the Keyboard Shortcuts editor (M5f)
        // The Source Monitor and the Program Monitor's buttons (M5h). Add Marker (M) is routed too, but the palette
        // keeps the typed marker.add, which asks for a name and a colour.
        QStringLiteral("clip.insert"),               QStringLiteral("clip.overwrite"),
        QStringLiteral("sequence.lift"),             QStringLiteral("sequence.extract"),
        QStringLiteral("sequence.matchFrame"),       QStringLiteral("sequence.reverseMatchFrame"),
        QStringLiteral("window.sourceMonitor"),      QStringLiteral("window.programMonitor"),
        // M5m: the attributes dialog (C.11) and voice-over recording (A.8).
        QStringLiteral("edit.pasteAttributes"),      QStringLiteral("edit.removeAttributes"),
        QStringLiteral("record.toggle"),             QStringLiteral("record.start"),
        QStringLiteral("record.stop"),
        // UX rework (stream S): the rest of what Main.qml's runCommand routes, whose keys already worked while the
        // palette said "Not available yet". The panel focus keys and Premiere's workspace keys (Alt+Shift+1..9) ...
        QStringLiteral("window.projects"),           QStringLiteral("window.effects"),
        QStringLiteral("window.mediaBrowser"),       QStringLiteral("window.timeline"),
        QStringLiteral("window.audioTrackMixer"),    QStringLiteral("window.audioClipMixer"),
        QStringLiteral("window.workspace1"),         QStringLiteral("window.workspace2"),
        QStringLiteral("window.workspace3"),         QStringLiteral("window.workspace4"),
        QStringLiteral("window.workspace5"),         QStringLiteral("window.workspace6"),
        QStringLiteral("window.workspace7"),         QStringLiteral("window.workspace8"),
        QStringLiteral("window.workspace9"),
        // ... the shell's window and file commands (Effect Controls Shift+5, Reset to Saved Layout Alt+Shift+0,
        // Maximize Frame Shift+`, Close Ctrl+W = the active panel, Close Project Ctrl+Shift+W, Save a Copy Ctrl+Alt+S),
        // Send to Media Encoder (the export queue) and Settings (Edit > Settings..., Ctrl+,) ...
        QStringLiteral("window.effectControls"),     QStringLiteral("window.resetToSavedLayout"),
        QStringLiteral("window.maximizeFrame"),      QStringLiteral("file.close"),
        QStringLiteral("file.closeProject"),         QStringLiteral("file.saveCopy"),
        QStringLiteral("file.sendToMediaEncoder"),   QStringLiteral("edit.preferences"),
        // ... Sequence > Render In to Out, Trim Edit and the trim keys (Trim), the timeline's display, track height
        // and sequence tab keys (TimelinePanel.runCommand) ...
        QStringLiteral("sequence.renderInToOut"),    QStringLiteral("sequence.trimEdit"),
        QStringLiteral("sequence.toggleTrimType"),   QStringLiteral("sequence.trimBackward"),
        QStringLiteral("sequence.trimForward"),      QStringLiteral("sequence.trimBackwardMany"),
        QStringLiteral("sequence.trimForwardMany"),  QStringLiteral("timeline.showVideoFadeHandles"),
        QStringLiteral("timeline.showAudioFadeHandles"),
        QStringLiteral("timeline.increaseVideoTrackHeight"), QStringLiteral("timeline.decreaseVideoTrackHeight"),
        QStringLiteral("timeline.increaseAudioTrackHeight"), QStringLiteral("timeline.decreaseAudioTrackHeight"),
        QStringLiteral("timeline.expandAllTracks"),  QStringLiteral("timeline.minimizeAllTracks"),
        QStringLiteral("timeline.nextSequenceTab"),  QStringLiteral("timeline.previousSequenceTab"),
        // ... the markers the Markers bridge runs (Add Marker and Clear All stay typed edits, see above), Essential
        // Graphics' layer keys (Graphics) and the gaming moments (Moments).
        QStringLiteral("marker.goToNext"),           QStringLiteral("marker.goToPrevious"),
        QStringLiteral("marker.clearSelected"),
        QStringLiteral("graphics.newTextLayer"),     QStringLiteral("graphics.newRectangleLayer"),
        QStringLiteral("graphics.newEllipseLayer"),  QStringLiteral("graphics.bringToFront"),
        QStringLiteral("graphics.bringForward"),     QStringLiteral("graphics.sendBackward"),
        QStringLiteral("graphics.sendToBack"),       QStringLiteral("graphics.selectNextLayer"),
        QStringLiteral("graphics.selectPreviousLayer"),
        QStringLiteral("moments.slowMo"),            QStringLiteral("moments.punchIn"),
        // 0.4 (stream B): slow shuttle (Shift+J / Shift+L: KeymapDispatcher's gesture), Play Around (Shift+K: Main.qml),
        // Show Next / Previous Screen (PageDown / PageUp: TimelinePanel.runCommand) ...
        QStringLiteral("transport.shuttleSlowBackward"), QStringLiteral("transport.shuttleSlowForward"),
        QStringLiteral("transport.playAround"),
        QStringLiteral("timeline.showNextScreen"),   QStringLiteral("timeline.showPreviousScreen"),
        // ... File > Import Folder... and Import from Media Browser (stream D) and Background Tasks (stream E: the top
        // bar's activity popover), all routed by Main.qml.
        QStringLiteral("file.importFolder"),         QStringLiteral("file.importFromMediaBrowser"),
        QStringLiteral("window.backgroundTasks"),
    };
    // P.2: Premiere's editing and sequence commands (E5) and the monitors' commands (E6), which Main.qml routes to the
    // Editing and Monitors bridges; the Library's (Project panel) and Multi-Camera's keys, which it routes to them.
    // 0.4: Effect Controls' own commands (stream C: Remove Selected Effect) and the clip volume keys (ClipAudio).
    return kView.contains(id) || EditingBridge::handlesCommand(id) || MonitorBridge::handlesCommand(id) ||
           LibraryBridge::handlesCommand(id) || MulticamEditBridge::handlesCommand(id) ||
           EffectControlsModel::handlesCommand(id) || ClipAudioBridge::handlesCommand(id);
}

std::shared_ptr<KeymapCommandProvider> KeymapCommandProvider::forApp(KeymapDispatcher* dispatcher,
                                                                    ProjectController* controller,
                                                                    std::function<QVariantMap(const QString&)> moreAvailability) {
    std::vector<Command> commands;
    for (const keymap::CommandInfo& info : keymap::CommandRegistry::builtin().all()) {
        const QString id = QString::fromUtf8(info.id.data(), static_cast<qsizetype>(info.id.size()));
        // Listed once: the typed edit command runs it until the app wires the key.
        if (!ProjectController::handlesCommand(id) && !viewHandles(id) && CommandBridge::offers(id))
            continue;
        commands.push_back({id,
                            QString::fromUtf8(info.displayName.data(), static_cast<qsizetype>(info.displayName.size())),
                            QString::fromUtf8(info.group.data(), static_cast<qsizetype>(info.group.size())),
                            QString::fromUtf8(info.premiereName.data(), static_cast<qsizetype>(info.premiereName.size()))});
    }
    QPointer<KeymapDispatcher> keys(dispatcher);
    QPointer<ProjectController> project(controller);
    return std::make_shared<KeymapCommandProvider>(
        std::move(commands), [keys](const QString& id) { return keys ? keys->shortcutText(id) : QString(); },
        [project, more = std::move(moreAvailability)](const QString& id) {
            QVariantMap state = project ? project->commandAvailability(id) : QVariantMap{};
            if (!state.value(QStringLiteral("handled")).toBool() && more)
                state = more(id);
            return state;
        },
        [keys](const QString& id) { return keys && keys->trigger(id); });
}

std::vector<PaletteItem> KeymapCommandProvider::items(const QString&) const {
    std::vector<PaletteItem> out;
    out.reserve(m_commands.size());
    for (const Command& c : m_commands) {
        PaletteItem item;
        item.key = QStringLiteral("command:") + c.id;
        item.title = c.title;
        item.group = c.group;
        item.shortcut = m_shortcutText ? m_shortcutText(c.id) : QString();
        item.section = QStringLiteral("commands");
        item.keywords = {c.id, c.premiereName};
        const QVariantMap state = stateOf(c.id);
        item.enabled = state.value(QStringLiteral("enabled")).toBool();
        item.reason = state.value(QStringLiteral("reason")).toString();
        out.push_back(std::move(item));
    }
    return out;
}

QVariantMap KeymapCommandProvider::stateOf(const QString& id) const {
    const QVariantMap state = m_availability ? m_availability(id) : QVariantMap{};
    if (state.value(QStringLiteral("handled")).toBool())
        return {{QStringLiteral("enabled"), state.value(QStringLiteral("enabled")).toBool()},
                {QStringLiteral("reason"), state.value(QStringLiteral("reason")).toString()}};
    if (viewHandles(id))
        return {{QStringLiteral("enabled"), true}, {QStringLiteral("reason"), QString()}};
    return {{QStringLiteral("enabled"), false}, {QStringLiteral("reason"), CommandPalette::tr("Not available yet")}};
}

QString KeymapCommandProvider::activate(const PaletteItem& item, const QVariantMap&) {
    const QString id = item.key.mid(item.key.indexOf(QLatin1Char(':')) + 1);
    return m_trigger && m_trigger(id) ? QString() : CommandPalette::tr("The command did not run");
}

EditCommandProvider::EditCommandProvider(CommandBridge* bridge) : m_bridge(bridge) {}

std::vector<PaletteItem> EditCommandProvider::items(const QString&) const {
    std::vector<PaletteItem> out;
    if (!m_bridge)
        return out;
    for (const QVariant& v : m_bridge->commands()) {
        const QVariantMap c = v.toMap();
        PaletteItem item;
        item.key = QStringLiteral("edit:") + c.value(QStringLiteral("name")).toString();
        item.title = c.value(QStringLiteral("title")).toString();
        item.group = c.value(QStringLiteral("group")).toString();
        item.detail = c.value(QStringLiteral("description")).toString();
        item.enabled = c.value(QStringLiteral("enabled")).toBool();
        item.reason = c.value(QStringLiteral("reason")).toString();
        item.params = c.value(QStringLiteral("params")).toList();
        item.section = QStringLiteral("edits");
        item.keywords = {c.value(QStringLiteral("name")).toString()};
        out.push_back(std::move(item));
    }
    return out;
}

QString EditCommandProvider::activate(const PaletteItem& item, const QVariantMap& params) {
    if (!m_bridge)
        return CommandPalette::tr("The command did not run");
    const QVariantMap result = m_bridge->apply(item.key.mid(item.key.indexOf(QLatin1Char(':')) + 1), params);
    return result.value(QStringLiteral("ok")).toBool() ? QString() : result.value(QStringLiteral("error")).toString();
}

QVariantMap EditCommandProvider::preview(const PaletteItem& item, const QVariantMap& params) {
    if (!m_bridge)
        return {};
    return m_bridge->preview(item.key.mid(item.key.indexOf(QLatin1Char(':')) + 1), params);
}

std::vector<PaletteItem> AskAiProvider::items(const QString& query) const {
    m_query = query;
    PaletteItem item;
    item.key = QStringLiteral("ai:ask");
    item.title = CommandPalette::tr("Ask AI…");
    item.detail = query.trimmed().isEmpty() ? CommandPalette::tr("Describe an edit in your own words")
                                            : CommandPalette::tr("“%1”").arg(query.trimmed());
    item.group = CommandPalette::tr("Assistant");
    item.section = QStringLiteral("ai");
    item.pinned = true;
    return {item};
}

QString AskAiProvider::activate(const PaletteItem&, const QVariantMap&) {
    if (m_open)
        m_open(m_query.trimmed());
    return {};
}

std::vector<PaletteItem> ActionProvider::items(const QString&) const {
    std::vector<PaletteItem> out;
    for (const Action& a : m_actions) {
        PaletteItem item;
        item.key = QStringLiteral("action:") + a.id;
        item.title = a.title;
        item.group = a.group;
        item.detail = a.detail;
        item.section = QStringLiteral("commands");
        item.keywords = a.keywords;
        item.reason = a.reason ? a.reason() : QString();
        item.enabled = item.reason.isEmpty();
        out.push_back(std::move(item));
    }
    return out;
}

QString ActionProvider::activate(const PaletteItem& item, const QVariantMap&) {
    for (const Action& a : m_actions) {
        if (QStringLiteral("action:") + a.id == item.key)
            return a.run ? a.run() : QString();
    }
    return CommandPalette::tr("Unknown command");
}

int ShellActionProvider::setActions(const QVariantList& actions) {
    std::vector<PaletteItem> rows;
    QSet<QString> seen;
    for (const QVariant& value : actions) {
        const QVariantMap a = value.toMap();
        const QString id = a.value(QStringLiteral("id")).toString();
        const QString title = a.value(QStringLiteral("title")).toString();
        if (id.isEmpty() || title.isEmpty() || seen.contains(id))
            continue;
        seen.insert(id);
        PaletteItem item;
        item.key = QStringLiteral("shell:") + id;
        item.title = title;
        item.group = a.value(QStringLiteral("group")).toString();
        item.detail = a.value(QStringLiteral("detail")).toString();
        item.shortcut = a.value(QStringLiteral("shortcut")).toString();
        item.section = QStringLiteral("commands");
        item.keywords = a.value(QStringLiteral("keywords")).toStringList();
        item.enabled = a.value(QStringLiteral("enabled"), true).toBool();
        if (!item.enabled) {
            item.reason = a.value(QStringLiteral("reason")).toString();
            if (item.reason.isEmpty())
                item.reason = CommandPalette::tr("Not available now");
        }
        rows.push_back(std::move(item));
    }
    m_items = std::move(rows);
    notifyChanged();
    return static_cast<int>(m_items.size());
}

std::vector<PaletteItem> ShellActionProvider::items(const QString&) const {
    return m_items;
}

QString ShellActionProvider::activate(const PaletteItem& item, const QVariantMap&) {
    if (m_run)
        m_run(item.key.mid(item.key.indexOf(QLatin1Char(':')) + 1));
    return {};
}

// ---- Palette --------------------------------------------------------------------------------------------------------

CommandPalette::CommandPalette(QObject* parent) : QObject(parent) {}

CommandPalette::~CommandPalette() {
    // A provider can outlive the palette (ExtensionsBridge keeps its own): its change callback captured this palette.
    for (const std::shared_ptr<PaletteProvider>& provider : m_providers)
        provider->setChangedCallback({});
}

CommandPalette* CommandPalette::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* palette = new CommandPalette;
    palette->m_persistRecent = true;
    if (qmlEngine) {
        auto* controller = qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        auto* keys = qmlEngine->singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
        auto* bridge = qmlEngine->singletonInstance<CommandBridge*>("Wabcut", "CommandBridge");
        palette->m_bridge = bridge;
        // The Library's and Multi-Camera's keys say themselves whether they can run now ("Select items in the Library
        // first", "Turn on the multi-camera view first"); their bridges are asked when the palette lists them.
        QPointer<QQmlEngine> engine(qmlEngine);
        const auto bridgeAvailability = [engine](const QString& id) -> QVariantMap {
            if (!engine)
                return {};
            if (LibraryBridge::handlesCommand(id)) {
                auto* library = engine->singletonInstance<LibraryBridge*>("Wabcut", "Library");
                return library ? library->commandAvailability(id) : QVariantMap{};
            }
            if (MulticamEditBridge::handlesCommand(id)) {
                auto* multicam = engine->singletonInstance<MulticamEditBridge*>("Wabcut", "Multicam");
                return multicam ? multicam->commandAvailability(id) : QVariantMap{};
            }
            if (ClipAudioBridge::handlesCommand(id)) { // 0.4: ] [ and Shift+] [ ("Select clips with sound")
                auto* clipAudio = engine->singletonInstance<ClipAudioBridge*>("Wabcut", "ClipAudio");
                return clipAudio ? clipAudio->commandAvailability(id) : QVariantMap{};
            }
            return {};
        };
        palette->addProvider(KeymapCommandProvider::forApp(keys, controller, bridgeAvailability));
        if (bridge)
            palette->addProvider(std::make_shared<EditCommandProvider>(bridge));
        if (auto* titles = qmlEngine->singletonInstance<TitlesBridge*>("Wabcut", "Titles")) {
            QPointer<TitlesBridge> t(titles);
            palette->addProvider(std::make_shared<ActionProvider>(std::vector<ActionProvider::Action>{
                {QStringLiteral("title.new"), tr("New Title"), tr("Graphics"),
                 tr("A text clip at the playhead on the targeted video track"),
                 {QStringLiteral("text"), QStringLiteral("essential graphics")},
                 [t] { return t ? t->unavailableReason() : QString(); },
                 [t] { return t ? t->createTitle() : QString(); }},
            }));
        }
        // C.12 (M5n): the extensions' commands ("ext:<extension>/<command>"), an "Extensions" group.
        if (auto* extensions = qmlEngine->singletonInstance<ExtensionsBridge*>("Wabcut", "Extensions"))
            palette->addProvider(extensions->paletteProvider());
        // UX rework (L): every row of the Settings window ("action:settings.<id>", a "Settings" group); running one
        // asks Main to show it (AppSettings.openRequested).
        if (auto* settings = qmlEngine->singletonInstance<AppSettingsBridge*>("Wabcut", "AppSettings"))
            palette->addProvider(AppSettingsBridge::paletteProvider(settings));
    }
    // Help (M5j onboarding): Main runs them (Onboarding.run()).
    const auto help = [palette](const char* what) {
        return [palette, what] {
            emit palette->helpRequested(QString::fromLatin1(what));
            return QString();
        };
    };
    palette->addProvider(std::make_shared<ActionProvider>(std::vector<ActionProvider::Action>{
        {QStringLiteral("help.welcome"), tr("Welcome and Setup"), tr("Help"),
         tr("Coming from Premiere? Choose your keys, workspace and accent"),
         {QStringLiteral("onboarding"), QStringLiteral("premiere"), QStringLiteral("getting started"),
          QStringLiteral("first run")},
         {},
         help("welcome")},
        {QStringLiteral("help.tour"), tr("Take the Tour"), tr("Help"), tr("Library, Timeline and Export in three steps"),
         {QStringLiteral("onboarding"), QStringLiteral("coach marks"), QStringLiteral("getting started")},
         {},
         help("tour")},
        {QStringLiteral("help.keyboardTrainer"), tr("Keyboard Trainer"), tr("Help"),
         tr("Practise shortcuts: press the key for each action"),
         {QStringLiteral("shortcuts"), QStringLiteral("practice"), QStringLiteral("game"), QStringLiteral("keys")},
         {},
         help("trainer")},
        {QStringLiteral("help.whatsNew"), tr("What's New"), tr("Help"), tr("Highlights of this version"),
         {QStringLiteral("release notes"), QStringLiteral("changes"), QStringLiteral("news")},
         {},
         help("whatsNew")},
    }));
    palette->addProvider(std::make_shared<AskAiProvider>([palette](const QString& q) { emit palette->aiRequested(q); }));
    QSettings settings;
    palette->m_recent = settings.value(QStringLiteral("palette/recent")).toStringList();
    return palette;
}

void CommandPalette::addProvider(std::shared_ptr<PaletteProvider> provider) {
    provider->setChangedCallback([this] { refresh(); });
    if (!m_keymap) // commandState answers from the keymap's rows
        m_keymap = std::dynamic_pointer_cast<KeymapCommandProvider>(provider);
    m_providers.push_back(std::move(provider));
    refresh();
}

void CommandPalette::setQuery(const QString& query) {
    if (query == m_query)
        return;
    m_query = query;
    emit queryChanged();
    refresh(); // AI rows depend on the query
}

void CommandPalette::refresh() {
    m_entries.clear();
    for (const auto& provider : m_providers) {
        for (PaletteItem& item : provider->items(m_query))
            m_entries.push_back({std::move(item), provider.get()});
    }
    rebuild();
}

void CommandPalette::rebuild() {
    const QString query = m_query.trimmed();
    const QStringList tokens = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    struct Scored {
        const Entry* entry;
        int score;
        bool recent;
    };
    std::vector<Scored> ranked;
    std::vector<const Entry*> pinned;
    for (const Entry& e : m_entries) {
        if (e.item.pinned) {
            pinned.push_back(&e);
            continue;
        }
        const qsizetype recentIndex = m_recent.indexOf(e.item.key);
        const bool recent = recentIndex >= 0;
        if (tokens.isEmpty()) {
            ranked.push_back({&e, recent ? 1000 - static_cast<int>(recentIndex) : 0, recent});
            continue;
        }
        if (!e.provider->fuzzy()) {
            ranked.push_back({&e, 500, recent});
            continue;
        }
        // Typing a shortcut ("ctrl+k") finds its command whatever the title.
        const bool shortcutHit = !e.item.shortcut.isEmpty() && normalizedShortcut(e.item.shortcut) == normalizedShortcut(query);
        int total = 0;
        bool all = true;
        for (const QString& token : tokens) {
            int best = 3 * fuzzyScore(token, e.item.title);
            best = std::max(best, initialsScore(token, e.item.title));
            best = std::max(best, fuzzyScore(token, e.item.group));
            for (const QString& keyword : e.item.keywords)
                best = std::max(best, fuzzyScore(token, keyword));
            if (best == 0) {
                all = false;
                break;
            }
            total += best;
        }
        if (!all && !shortcutHit)
            continue;
        if (!all)
            total = 0;
        if (shortcutHit)
            total += 600;
        if (e.item.title.compare(query, Qt::CaseInsensitive) == 0)
            total += 400;
        else if (e.item.title.startsWith(query, Qt::CaseInsensitive))
            total += 200;
        total += initialsScore(QString(query).remove(QLatin1Char(' ')), e.item.title);
        if (recent)
            total += 120 - 10 * static_cast<int>(recentIndex);
        if (!e.item.enabled)
            total -= 60;
        ranked.push_back({&e, total, recent});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [&](const Scored& a, const Scored& b) {
        if (tokens.isEmpty()) {
            if (a.recent != b.recent)
                return a.recent;
            if (a.recent)
                return a.score > b.score;
            if (a.entry->item.enabled != b.entry->item.enabled)
                return a.entry->item.enabled; // runnable commands first
            return false; // keep registry order
        }
        return a.score > b.score;
    });
    QVariantList results;
    const int limit = tokens.isEmpty() ? static_cast<int>(ranked.size()) : kMaxResults;
    const auto toVariant = [](const PaletteItem& item, bool recent) {
        return QVariantMap{
            {QStringLiteral("key"), item.key},          {QStringLiteral("title"), item.title},
            {QStringLiteral("group"), item.group},      {QStringLiteral("shortcut"), item.shortcut},
            {QStringLiteral("detail"), item.detail},    {QStringLiteral("enabled"), item.enabled},
            {QStringLiteral("reason"), item.reason},    {QStringLiteral("section"), recent ? QStringLiteral("recent") : item.section},
            {QStringLiteral("params"), item.params},    {QStringLiteral("pinned"), item.pinned},
            {QStringLiteral("recent"), recent},
        };
    };
    for (int i = 0; i < static_cast<int>(ranked.size()) && i < limit; ++i)
        results << toVariant(ranked[static_cast<std::size_t>(i)].entry->item,
                             tokens.isEmpty() && ranked[static_cast<std::size_t>(i)].recent);
    for (const Entry* e : pinned)
        results << toVariant(e->item, false);
    m_results = std::move(results);
    emit resultsChanged();
}

const CommandPalette::Entry* CommandPalette::find(const QString& key) const {
    for (const Entry& e : m_entries) {
        if (e.item.key == key)
            return &e;
    }
    return nullptr;
}

QString CommandPalette::activate(const QString& key, const QVariantMap& params) {
    const Entry* entry = find(key);
    if (!entry)
        return tr("Unknown command");
    if (!entry->item.enabled)
        return entry->item.reason.isEmpty() ? tr("Not available") : entry->item.reason;
    const PaletteItem item = entry->item; // the provider may refresh the rows while it runs
    PaletteProvider* provider = entry->provider;
    const QString error = provider->activate(item, params);
    if (error.isEmpty() && !item.pinned)
        remember(key);
    return error;
}

QVariantMap CommandPalette::preview(const QString& key, const QVariantMap& params) {
    const Entry* entry = find(key);
    return entry ? entry->provider->preview(entry->item, params) : QVariantMap{};
}

QVariantMap CommandPalette::defaults(const QString& key) const {
    if (key.startsWith(QLatin1String("edit:")) && m_bridge)
        return m_bridge->defaults(key.mid(5));
    return {};
}

int CommandPalette::setShellActions(const QVariantList& actions) {
    if (m_shell)
        return m_shell->setActions(actions); // refreshes the rows
    // First call: after the app's providers, so an empty query lists the keymap's commands first.
    m_shell = std::make_shared<ShellActionProvider>([this](const QString& id) { emit shellActionRequested(id); });
    const int kept = m_shell->setActions(actions);
    addProvider(m_shell);
    return kept;
}

QVariantMap CommandPalette::commandState(const QString& id) const {
    const QByteArray utf8 = id.toUtf8();
    const keymap::CommandInfo* info = keymap::CommandRegistry::builtin().find(std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
    QVariantMap out{{QStringLiteral("known"), info != nullptr}};
    if (info) {
        out.insert(QStringLiteral("title"),
                   QString::fromUtf8(info->displayName.data(), static_cast<qsizetype>(info->displayName.size())));
        out.insert(QStringLiteral("group"), QString::fromUtf8(info->group.data(), static_cast<qsizetype>(info->group.size())));
    }
    out.insert(QStringLiteral("shortcut"), m_keymap ? m_keymap->shortcutOf(id) : QString());
    QVariantMap state = m_keymap ? m_keymap->stateOf(id)
                                 : QVariantMap{{QStringLiteral("enabled"), false},
                                               {QStringLiteral("reason"), tr("Not available yet")}};
    // A key whose command the palette lists as a typed edit (Add Marker asks for a name there): Main.qml runs the key.
    if (!state.value(QStringLiteral("enabled")).toBool() &&
        state.value(QStringLiteral("reason")).toString() == tr("Not available yet") && info && CommandBridge::offers(id))
        state = {{QStringLiteral("enabled"), true}, {QStringLiteral("reason"), QString()}};
    out.insert(state);
    return out;
}

void CommandPalette::remember(const QString& key) {
    m_recent.removeAll(key);
    m_recent.prepend(key);
    while (m_recent.size() > kMaxRecent)
        m_recent.removeLast();
    if (m_persistRecent) {
        QSettings settings;
        settings.setValue(QStringLiteral("palette/recent"), m_recent);
    }
    emit recentChanged();
}

} // namespace wabcut::app
