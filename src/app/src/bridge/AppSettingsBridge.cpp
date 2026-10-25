#include "AppSettingsBridge.h"

#include "CommandPalette.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QSettings>

#include <algorithm>
#include <array>
#include <vector>

namespace wabcut::app {

namespace {

const QString kLaunchActionKey = QStringLiteral("startup/launchAction");

constexpr const char* kContext = "wabcut::app::AppSettingsBridge";

QString tx(const char* text) {
    return QCoreApplication::translate(kContext, text);
}

/// One row of the Settings window. `keywords` is a "|"-separated, translatable list of other words people search for.
struct IndexEntry {
    const char* id;
    const char* page;  ///< SettingsWindow category key
    const char* row;   ///< the row's key on its page (SettingsRow / SettingsSection rowKey; SettingsPage.reveal)
    const char* title;
    const char* section;
    const char* keywords;
    const char* opens; ///< SettingsWindow.runAction key, "" for a row on the page
};

// clang-format off
constexpr std::array kIndex{
    // General
    IndexEntry{"startWorkspace", "general", "startWorkspace", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Workspace at start-up"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Start-up"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "open in|workspace|launch|start"), ""},
    IndexEntry{"launchAction", "general", "launchAction", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "When Wabcut starts"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Start-up"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "home|reopen|last project|launch|startup|open"), ""},
    IndexEntry{"nativeFrame", "general", "nativeFrame", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Use native window frame"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Start-up"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "title bar|window|frameless|borders|system"), ""},
    IndexEntry{"projectLocation", "general", "projectLocation", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Default location for new projects"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Projects"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "folder|directory|path|save|new project"), ""},
    IndexEntry{"recentProjects", "general", "recentProjects", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Recent projects to show"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Projects"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "recent|history|home|list|keep|open recent"), ""},
    IndexEntry{"lockProjects", "general", "lockProjects", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Enable project locking"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Projects"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "lock|locking|shared|read-only|collaboration|team"), ""},
    IndexEntry{"saveStatus", "general", "saveStatus", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Project save status"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Projects"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "saved|unsaved|file|path"), ""},
    IndexEntry{"autosave", "general", "autosave", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Autosave interval"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Projects"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "autosave|backup|recover|recovery|minutes"), ""},
    IndexEntry{"reviewAuthor", "general", "reviewAuthor", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Sign review notes as"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Review"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "review|notes|name|author|comments"), ""},
    IndexEntry{"keepAwake", "general", "keepAwake", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Keep the computer awake while exporting"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Background work"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "sleep|power|awake|standby|export|render|long jobs"), ""},
    // Appearance
    IndexEntry{"theme", "appearance", "theme", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Theme"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Theme"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "echo|daylight|graphite|midnight|paper|import theme|export theme|save theme|look"), ""},
    IndexEntry{"mode", "appearance", "mode", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Dark or light mode"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Mode and accent"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "dark|light|mode|night|day"), ""},
    IndexEntry{"accent", "appearance", "accent", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Accent colour"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Mode and accent"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "accent|color|colour|violet|highlight"), ""},
    IndexEntry{"highContrast", "appearance", "highContrast", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "High contrast"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Mode and accent"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "contrast|accessibility|a11y|visibility"), ""},
    IndexEntry{"reduceMotion", "appearance", "reduceMotion", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Reduce motion"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Mode and accent"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "animation|motion|accessibility|a11y|transitions"), ""},
    IndexEntry{"launchIntro", "appearance", "launchIntro", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Play the intro when Wabcut starts"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Mode and accent"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "intro|logo|animation|splash|startup"), ""},
    IndexEntry{"uiScale", "appearance", "uiScale", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Interface size"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Mode and accent"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "scale|zoom|size|dpi|bigger|smaller|125|150"), ""},
    IndexEntry{"customColors", "appearance", "customColors", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Custom colours"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Custom colours"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "colors|colours|clip colors|label colors|palette"), ""},
    // Interface
    IndexEntry{"gpuStatus", "interface", "gpuStatus", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "GPU status in the title bar"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Title bar"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "gpu|graphics|adapter|pill"), ""},
    IndexEntry{"saveStatusPill", "interface", "saveStatusPill", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Save status in the title bar"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Title bar"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "saved|autosave|pill"), ""},
    IndexEntry{"liveResize", "interface", "liveResize", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Resize panels live"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Panels"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "panels|resize|docking|layout"), ""},
    // Timeline & editing
    IndexEntry{"linkedSelection", "timeline", "linkedSelection", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Linked selection"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Editing"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "link|linked|audio and video|select"), ""},
    IndexEntry{"snapIndicator", "timeline", "snapIndicator", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Snap line"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Editing"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "snap|snapping|indicator|magnet"), ""},
    IndexEntry{"trimLargeOffset", "timeline", "trimLargeOffset", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Large trim offset"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Editing"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "trim|frames|nudge|offset"), ""},
    IndexEntry{"playAroundEdit", "timeline", "playAroundEdit", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Play around the edit"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Editing"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "preroll|postroll|pre-roll|post-roll|trim|playback|play around|shift+k"), ""},
    IndexEntry{"fitClip", "timeline", "fitClip", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Four-point edits that do not fit"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Editing"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "fit clip|four-point|4-point|speed|ignore"), ""},
    IndexEntry{"playbackScroll", "timeline", "playbackScroll", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Playback auto-scroll"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Editing"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "auto-scroll|autoscroll|scroll|page|smooth|follow|playback"), ""},
    IndexEntry{"snapPlayhead", "timeline", "snapPlayhead", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Snap the playhead"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Editing"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "snap|playhead|ruler|scrub"), ""},
    IndexEntry{"filmstrip", "timeline", "filmstrip", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Video thumbnails on clips"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Tracks"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "filmstrip|thumbnails|head|tail|continuous"), ""},
    IndexEntry{"rectifiedWaveforms", "timeline", "rectifiedWaveforms", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Rectified waveforms"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Tracks"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "waveform|audio|rectified"), ""},
    IndexEntry{"logarithmicWaveforms", "timeline", "logarithmicWaveforms", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Logarithmic waveforms"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Tracks"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "waveform|audio|log|logarithmic"), ""},
    IndexEntry{"throughEdits", "timeline", "throughEdits", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Through edits"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Tracks"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "through edit|cuts|join"), ""},
    IndexEntry{"duplicateFrames", "timeline", "duplicateFrames", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Duplicate frame markers"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Tracks"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "duplicate|frames|reused"), ""},
    IndexEntry{"clipMarkers", "timeline", "clipMarkers", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Clip markers"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Tracks"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "markers|clip markers"), ""},
    IndexEntry{"audioTrackLane", "timeline", "audioTrackLane", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Audio track lane"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Tracks"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "volume|pan|fader|keyframes|automation"), ""},
    IndexEntry{"clipSpeed", "timeline", "clipSpeed", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Clip speed"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Tracks"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "speed|percent|retime|slow motion"), ""},
    IndexEntry{"fxBadges", "timeline", "fxBadges", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "fx badges"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Tracks"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "fx|effects|badge|keyframes"), ""},
    IndexEntry{"clipTooltips", "timeline", "clipTooltips", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Clip tooltips"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Tracks"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "tooltip|hover|info|details"), ""},
    IndexEntry{"keyframeLane", "timeline", "keyframeLane", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Keyframe lane in Effect Controls"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Panels"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "keyframes|effect controls|lane"), ""},
    IndexEntry{"safeMargins", "timeline", "safeMargins", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Safe margins"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Panels"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "action safe|title safe|guides|monitor"), ""},
    IndexEntry{"defaultVideoTransition", "timeline", "defaultVideoTransition", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Default video transition"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Media and timeline defaults"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "transition|cross dissolve|dissolve|dip|wipe|apply video transition|ctrl+d"), ""},
    IndexEntry{"defaultAudioTransition", "timeline", "defaultAudioTransition", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Default audio transition"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Media and timeline defaults"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "transition|crossfade|constant power|constant gain|audio fade|apply audio transition"), ""},
    IndexEntry{"editingDefaults", "timeline", "editingDefaults", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Media scaling and default durations"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Media and timeline defaults"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "scale to frame size|set to frame size|transition duration|still image duration|xmp|preferences|media|timeline"), "editing"},
    // Playback & performance
    IndexEntry{"rendering", "performance", "rendering", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Rendering"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Graphics"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "gpu|graphics|direct3d|d3d11|d3d12|vulkan|metal|opengl"), ""},
    IndexEntry{"decoding", "performance", "decoding", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Video decoding"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Graphics"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "hardware decoding|nvdec|gpu|decoder"), ""},
    IndexEntry{"mediaCache", "performance", "mediaCache", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Proxies, preview quality and disk caches"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Media & cache"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "proxy|proxies|cache|preview|playback resolution|disk"), "media"},
    // Audio
    IndexEntry{"voiceOver", "audio", "voiceOver", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Microphone, count-in and latency"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Voice-over"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "microphone|mic|recording|voice-over|input|latency|audio device"), "audio"},
    IndexEntry{"audioPlugins", "audio", "audioPlugins", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Audio plug-ins"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Plug-ins"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "vst|vst3|clap|plugins|effects"), "plugins"},
    // AI
    IndexEntry{"aiFeatures", "ai", "aiFeatures", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "AI features"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "AI"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "ai|offline|turn off|disable"), ""},
    IndexEntry{"assistantModel", "ai", "assistantModel", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Assistant model"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "AI"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "assistant|model|llm|chat"), ""},
    IndexEntry{"aiProviders", "ai", "aiProviders", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "AI providers and keys"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "AI"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "api key|provider|account|cloud|openai|anthropic"), ""},
    IndexEntry{"aiModels", "ai", "aiModels", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "AI models"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "AI"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "models|download|transcription|whisper"), ""},
    IndexEntry{"aiPrivacy", "ai", "aiPrivacy", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "AI privacy"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "AI"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "privacy|data|upload|local only"), ""},
    IndexEntry{"aiUsage", "ai", "aiUsage", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "AI usage"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "AI"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "usage|cost|tokens|spending"), ""},
    IndexEntry{"localAssistant", "ai", "localAssistant", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Local assistant"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "AI"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "local|offline|llm|on device"), ""},
    // Keyboard
    IndexEntry{"shortcuts", "keyboard", "shortcuts", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Keyboard shortcuts"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Shortcuts"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "keys|hotkeys|keymap|premiere|kys|bindings"), "shortcuts"},
    IndexEntry{"trainer", "keyboard", "trainer", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Keyboard trainer"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Shortcuts"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "learn|practice|game|keys"), "trainer"},
    // Plugins & extensions
    IndexEntry{"extensions", "extensions", "extensions", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Extensions"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Extensions"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "extensions|scripts|add-ons|permissions|logs"), "extensions"},
    IndexEntry{"pluginFolders", "extensions", "pluginFolders", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Plug-in folders and scanning"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Audio plug-ins"), QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "vst|vst3|clap|scan|rescan|blocked"), "plugins"},
};
// clang-format on

/// The Settings window's category titles (SettingsWindow.categories' labels).
struct PageTitle {
    const char* page;
    const char* title;
};
constexpr std::array kPageTitles{
    PageTitle{"general", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "General")},
    PageTitle{"appearance", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Appearance")},
    PageTitle{"interface", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Interface")},
    PageTitle{"timeline", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Timeline & editing")},
    PageTitle{"performance", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Playback & performance")},
    PageTitle{"audio", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Audio")},
    PageTitle{"ai", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "AI")},
    PageTitle{"keyboard", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Keyboard")},
    PageTitle{"extensions", QT_TRANSLATE_NOOP("wabcut::app::AppSettingsBridge", "Plugins & extensions")},
};

QString pageTitle(const QString& page) {
    for (const PageTitle& t : kPageTitles) {
        if (page == QLatin1String(t.page))
            return tx(t.title);
    }
    return page;
}

QStringList keywordsOf(const IndexEntry& e) {
    return tx(e.keywords).split(QLatin1Char('|'), Qt::SkipEmptyParts);
}

QVariantMap entryMap(const IndexEntry& e) {
    const QString page = QString::fromLatin1(e.page);
    const QString section = tx(e.section);
    const QString pageName = pageTitle(page);
    //: Where a setting lives in the Settings window, e.g. "General › Start-up"
    const QString detail = QCoreApplication::translate("wabcut::app::AppSettingsBridge", "%1 › %2").arg(pageName, section);
    return {{QStringLiteral("id"), QString::fromLatin1(e.id)},
            {QStringLiteral("page"), page},
            {QStringLiteral("row"), QString::fromLatin1(e.row)},
            {QStringLiteral("title"), tx(e.title)},
            {QStringLiteral("section"), section},
            {QStringLiteral("pageTitle"), pageName},
            {QStringLiteral("detail"), detail},
            {QStringLiteral("keywords"), keywordsOf(e)},
            {QStringLiteral("opens"), QString::fromLatin1(e.opens)}};
}

const IndexEntry* findEntry(const QString& id) {
    for (const IndexEntry& e : kIndex) {
        if (id == QLatin1String(e.id))
            return &e;
    }
    return nullptr;
}

/// Whether one of the space-separated words of `text` starts with `word` (both lower case).
bool wordStartsWith(const QString& text, const QString& word) {
    if (text.startsWith(word))
        return true;
    for (qsizetype at = text.indexOf(QLatin1Char(' ')); at >= 0; at = text.indexOf(QLatin1Char(' '), at + 1)) {
        if (QStringView(text).mid(at + 1).startsWith(word))
            return true;
    }
    return false;
}

/// How well one query word matches an entry (0 = not at all).
int wordScore(const QString& word, const QString& title, const QStringList& keywords, const QString& section,
              const QString& page) {
    if (wordStartsWith(title, word))
        return 100;
    if (title.contains(word))
        return 70;
    int best = 0;
    for (const QString& k : keywords) {
        if (wordStartsWith(k, word))
            best = std::max(best, 60);
        else if (k.contains(word))
            best = std::max(best, 40);
    }
    if (best > 0)
        return best;
    if (section.contains(word) || page.contains(word))
        return 25;
    return word.size() >= 3 && fuzzyScore(word, title) > 0 ? 10 : 0;
}

} // namespace

AppSettingsBridge::AppSettingsBridge(QObject* parent) : QObject(parent) {}

QString AppSettingsBridge::launchAction() const {
    return QSettings().value(kLaunchActionKey).toString() == QLatin1String("reopen") ? QStringLiteral("reopen")
                                                                                      : QStringLiteral("home");
}

void AppSettingsBridge::setLaunchAction(const QString& action) {
    const QString next = action == QLatin1String("reopen") ? QStringLiteral("reopen") : QStringLiteral("home");
    if (next == launchAction())
        return;
    QSettings().setValue(kLaunchActionKey, next);
    emit launchActionChanged();
}

QVariantList AppSettingsBridge::index() const {
    QVariantList out;
    for (const IndexEntry& e : kIndex)
        out.append(entryMap(e));
    return out;
}

QUrl AppSettingsBridge::projectToReopen(const QVariantList& recent) const {
    if (launchAction() != QLatin1String("reopen") || recent.isEmpty())
        return {};
    const QVariantMap newest = recent.front().toMap();
    if (newest.value(QStringLiteral("missing")).toBool())
        return {};
    QUrl url = newest.value(QStringLiteral("url")).toUrl();
    if (url.isEmpty()) {
        const QString path = newest.value(QStringLiteral("path")).toString();
        if (!path.isEmpty())
            url = QUrl::fromLocalFile(path);
    }
    if (!url.isLocalFile() || !QFileInfo(url.toLocalFile()).isFile())
        return {};
    return url;
}

QVariantList AppSettingsBridge::search(const QString& query, int limit) const {
    const QStringList words = query.simplified().toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty())
        return {};
    struct Hit {
        int score;
        std::size_t order;
    };
    std::vector<Hit> hits;
    for (std::size_t i = 0; i < kIndex.size(); ++i) {
        const IndexEntry& e = kIndex[i];
        const QString title = tx(e.title).toLower();
        const QString section = tx(e.section).toLower();
        const QString page = pageTitle(QString::fromLatin1(e.page)).toLower();
        QStringList keywords = keywordsOf(e);
        for (QString& k : keywords)
            k = k.toLower();
        int total = 0;
        bool all = true;
        for (const QString& word : words) {
            const int s = wordScore(word, title, keywords, section, page);
            if (s == 0) {
                all = false;
                break;
            }
            total += s;
        }
        if (all)
            hits.push_back({total, i});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    QVariantList out;
    for (const Hit& hit : hits) {
        if (limit > 0 && out.size() >= limit)
            break;
        QVariantMap map = entryMap(kIndex[hit.order]);
        map.insert(QStringLiteral("score"), hit.score);
        out.append(map);
    }
    return out;
}

QVariantMap AppSettingsBridge::entry(const QString& id) const {
    const IndexEntry* e = findEntry(id);
    return e ? entryMap(*e) : QVariantMap{};
}

bool AppSettingsBridge::openSetting(const QString& id) {
    const IndexEntry* e = findEntry(id);
    if (!e)
        return false;
    emit openRequested(QString::fromLatin1(e->page), QString::fromLatin1(e->row));
    return true;
}

std::shared_ptr<PaletteProvider> AppSettingsBridge::paletteProvider(AppSettingsBridge* bridge) {
    QPointer<AppSettingsBridge> target(bridge);
    std::vector<ActionProvider::Action> actions;
    actions.reserve(kIndex.size());
    for (const IndexEntry& e : kIndex) {
        const QVariantMap map = entryMap(e);
        QStringList keywords = map.value(QStringLiteral("keywords")).toStringList();
        keywords << QStringLiteral("settings") << QStringLiteral("preferences")
                 << map.value(QStringLiteral("pageTitle")).toString() << map.value(QStringLiteral("section")).toString();
        const QString id = QString::fromLatin1(e.id);
        actions.push_back({QStringLiteral("settings.") + id, map.value(QStringLiteral("title")).toString(),
                           QCoreApplication::translate("wabcut::app::AppSettingsBridge", "Settings"),
                           map.value(QStringLiteral("detail")).toString(),
                           keywords, {}, [target, id] {
                               if (!target || !target->openSetting(id))
                                   return QCoreApplication::translate("wabcut::app::AppSettingsBridge", "Settings are not available");
                               return QString();
                           }});
    }
    return std::make_shared<ActionProvider>(std::move(actions));
}

} // namespace wabcut::app
