#include "EditingBridge.h"

#include "AppAudioServices.h"
#include "BridgeIds.h"
#include "CommandJob.h"
#include "LibraryBridge.h"
#include "LibraryModel.h" // parseMediaKey
#include "ProjectController.h"
#include "SequenceTabsBridge.h"
#include "SourceMonitorBridge.h"

#include <wabcut/command/EditingServices.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/SourceServices.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/RawSourceSettings.h>
#include <wabcut/model/SequencePresets.h>
#include <wabcut/model/Timecode.h>

#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QSet>
#include <QSettings>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

using command::Json;

namespace {

const QString kScalingKey = QStringLiteral("media/defaultMediaScaling");
const QString kChapterMarkersKey = QStringLiteral("media/chapterMarkersOnImport"); // P0 round
const QString kStillKey = QStringLiteral("timeline/stillImageDuration");
const QString kVideoTransitionKey = QStringLiteral("timeline/videoTransitionDuration");
const QString kAudioTransitionKey = QStringLiteral("timeline/audioTransitionDuration");
// 0.2 UX (stream T): Set as Default Transition.
const QString kDefaultVideoTransitionKey = QStringLiteral("timeline/defaultVideoTransition");
const QString kDefaultAudioTransitionKey = QStringLiteral("timeline/defaultAudioTransition");
/// Premiere's defaults: stills 5 seconds, transitions 1 second.
constexpr double kStillSeconds = 5.0;
constexpr double kTransitionSeconds = 1.0;

/// The built-in transition `id` of `kind` (VideoTransition / AudioTransition), or null.
const EffectDescriptor* transitionOfKind(const std::string& id, EffectKind kind) {
    const EffectDescriptor* d = id.empty() ? nullptr : EffectRegistry::builtin().find(id);
    return d && d->kind == kind ? d : nullptr;
}

/// The saved default transition of `kind`, or `fallback` when none is saved or it no longer resolves.
std::string savedTransition(const QString& key, EffectKind kind, const std::string& fallback) {
    const std::string saved = QSettings().value(key).toString().toStdString();
    return transitionOfKind(saved, kind) ? saved : fallback;
}

QString text(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

Json idArray(const std::vector<ClipId>& clips) {
    Json out = Json::array();
    for (const ClipId id : clips)
        out.push_back(id.value);
    return out;
}

Json mediaArray(const std::vector<MediaId>& media) {
    Json out = Json::array();
    for (const MediaId id : media)
        out.push_back(id.value);
    return out;
}

double seconds(const QString& key, double fallback) {
    bool ok = false;
    const double v = QSettings().value(key, fallback).toDouble(&ok);
    return ok && std::isfinite(v) && v > 0.0 ? v : fallback;
}

void storeSeconds(const QString& key, double value) { QSettings().setValue(key, std::clamp(value, 0.04, 3600.0)); }

/// A QVariantMap of command params (numbers, strings, booleans) as JSON.
Json jsonOf(const QVariantMap& map) {
    Json out = Json::object();
    for (auto it = map.begin(); it != map.end(); ++it) {
        const std::string key = it.key().toStdString();
        const QVariant& v = it.value();
        switch (v.typeId()) {
        case QMetaType::Bool:
            out[key] = v.toBool();
            break;
        case QMetaType::Int:
        case QMetaType::LongLong:
        case QMetaType::UInt:
        case QMetaType::ULongLong:
            out[key] = v.toLongLong();
            break;
        case QMetaType::Double:
        case QMetaType::Float:
            out[key] = v.toDouble();
            break;
        case QMetaType::QVariantMap:
            out[key] = jsonOf(v.toMap());
            break;
        default:
            if (v.isNull())
                out[key] = nullptr;
            else
                out[key] = v.toString().toStdString();
            break;
        }
    }
    return out;
}

} // namespace

EditingBridge::EditingBridge(QObject* parent) : QObject(parent), m_reloadJob(std::make_unique<CommandJob>()) {}

EditingBridge::~EditingBridge() {
    if (m_reloadJob)
        m_reloadJob->stop(); // the worker reads files and must not outlive the bridge
}

void EditingBridge::setSynchronous(bool synchronous) {
    m_reloadJob->setSynchronous(synchronous);
}

EditingBridge* EditingBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new EditingBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setLibrary(qmlEngine->singletonInstance<LibraryBridge*>("Wabcut", "Library"));
        bridge->setSourceMonitor(qmlEngine->singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor"));
        bridge->setTabs(qmlEngine->singletonInstance<SequenceTabsBridge*>("Wabcut", "SequenceTabs"));
    }
    return bridge;
}

void EditingBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::historyChanged, this, &EditingBridge::historyChanged);
        connect(controller, &ProjectController::revisionChanged, this, &EditingBridge::historyChanged);
    }
    emit historyChanged();
}

void EditingBridge::setLibrary(LibraryBridge* library) { m_library = library; }
void EditingBridge::setSourceMonitor(SourceMonitorBridge* source) { m_source = source; }
void EditingBridge::setTabs(SequenceTabsBridge* tabs) { m_tabs = tabs; }

bool EditingBridge::handlesCommand(const QString& id) {
    static const QSet<QString> kCommands{
        // Clip > Video Options
        QStringLiteral("clip.scaleToFrameSize"),
        QStringLiteral("clip.nearestNeighborScaling"),
        QStringLiteral("clip.setToFrameSize"),
        QStringLiteral("clip.setToFillFrame"),
        QStringLiteral("clip.frameHoldOptions"),
        QStringLiteral("clip.addFrameHold"),
        QStringLiteral("clip.insertFrameHoldSegment"),
        QStringLiteral("clip.fieldOptions"),
        // Edit > Preferences, Duplicate, Edit Original
        QStringLiteral("edit.preferencesMedia"),
        QStringLiteral("edit.preferencesTimeline"),
        QStringLiteral("edit.duplicate"),
        QStringLiteral("edit.editOriginal"),
        // File > New > Sequence From Clip; Sequence > Sequence Settings, Replace With Clip, Make Subsequence, Join
        // Through Edits, Go to Gap, Close Gap
        QStringLiteral("file.newSequenceFromClip"),
        QStringLiteral("file.newSequencesFromClips"),
        QStringLiteral("file.newSequenceFromClipsTogether"),
        QStringLiteral("sequence.settings"),
        QStringLiteral("sequence.replaceFromSourceMonitor"),
        QStringLiteral("sequence.replaceFromSourceMonitorMatchFrame"),
        QStringLiteral("sequence.replaceFromBin"),
        QStringLiteral("sequence.makeSubsequence"),
        QStringLiteral("sequence.makeSubsequenceFromInToOut"),
        QStringLiteral("sequence.joinThroughEdits"),
        QStringLiteral("sequence.nextInSequence"),
        QStringLiteral("sequence.previousInSequence"),
        QStringLiteral("sequence.nextInTrack"),
        QStringLiteral("sequence.previousInTrack"),
        QStringLiteral("sequence.closeGap"),
        // 0.7 (cards 123, 122): Sequence > Remove Transitions (the selection's), Remove All Transitions, Remove Disabled Clips
        QStringLiteral("sequence.removeTransitions"),
        QStringLiteral("sequence.removeAllTransitions"),
        QStringLiteral("sequence.removeDisabledClips"),
        // Clip > Merge Clips, Modify, Replace Footage, Make Offline
        QStringLiteral("clip.mergeClips"),
        QStringLiteral("clip.interpretFootage"),
        QStringLiteral("clip.rawSourceSettings"),
        QStringLiteral("clip.modifyTimecode"),
        QStringLiteral("clip.replaceFootage"),
        QStringLiteral("clip.makeOffline"),
        QStringLiteral("clip.reloadFootage"),
        // 0.2 UX (stream T): Reveal in Project (no default key)
        QStringLiteral("clip.revealInProject"),
        // Window > History, and the History panel's Step Backward / Step Forward (Left / Right in its scope)
        QStringLiteral("window.history"),
        QStringLiteral("history.stepBackward"),
        QStringLiteral("history.stepForward"),
        // P1 round (#4): the History panel's Delete (Backspace) and its menu's Clear History (both confirm first)
        QStringLiteral("history.delete"),
        QStringLiteral("history.clear"),
    };
    return kCommands.contains(id);
}

// ---- Preferences ------------------------------------------------------------------------------------------------

MediaScaling EditingBridge::defaultMediaScalingSetting() {
    const QString saved = QSettings().value(kScalingKey).toString();
    return mediaScalingFromString(saved.toStdString()).value_or(MediaScaling::None);
}

bool EditingBridge::chapterMarkersSetting() { return QSettings().value(kChapterMarkersKey, true).toBool(); }

void EditingBridge::setChapterMarkersOnImport(bool on) {
    if (on == chapterMarkersSetting())
        return;
    QSettings().setValue(kChapterMarkersKey, on);
    emit preferencesChanged();
}

std::optional<Ticks> EditingBridge::stillDurationSetting() {
    const double s = seconds(kStillKey, kStillSeconds);
    if (std::abs(s - kStillSeconds) < 1e-9)
        return std::nullopt;
    return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))};
}

edit::DefaultTransitions EditingBridge::defaultTransitionsSetting() {
    edit::DefaultTransitions out;
    const double video = seconds(kVideoTransitionKey, kTransitionSeconds);
    const double audio = seconds(kAudioTransitionKey, kTransitionSeconds);
    if (std::abs(video - kTransitionSeconds) > 1e-9)
        out.videoDuration = Ticks{static_cast<std::int64_t>(std::llround(video * static_cast<double>(kTicksPerSecond)))};
    if (std::abs(audio - kTransitionSeconds) > 1e-9)
        out.audioDuration = Ticks{static_cast<std::int64_t>(std::llround(audio * static_cast<double>(kTicksPerSecond)))};
    // 0.2 UX (stream T): the chosen transitions; one that no longer resolves falls back to the built-in default.
    out.video = savedTransition(kDefaultVideoTransitionKey, EffectKind::VideoTransition, out.video);
    out.audio = savedTransition(kDefaultAudioTransitionKey, EffectKind::AudioTransition, out.audio);
    return out;
}

QString EditingBridge::defaultMediaScaling() const { return text(toString(defaultMediaScalingSetting())); }

void EditingBridge::setDefaultMediaScaling(const QString& scaling) {
    const auto value = mediaScalingFromString(scaling.toStdString());
    if (!value || *value == defaultMediaScalingSetting())
        return;
    QSettings().setValue(kScalingKey, scaling);
    emit preferencesChanged();
}

double EditingBridge::stillDuration() const { return seconds(kStillKey, kStillSeconds); }
void EditingBridge::setStillDuration(double s) {
    if (std::abs(s - stillDuration()) < 1e-9)
        return;
    storeSeconds(kStillKey, s);
    emit preferencesChanged();
}
double EditingBridge::videoTransitionDuration() const { return seconds(kVideoTransitionKey, kTransitionSeconds); }
void EditingBridge::setVideoTransitionDuration(double s) {
    if (std::abs(s - videoTransitionDuration()) < 1e-9)
        return;
    storeSeconds(kVideoTransitionKey, s);
    emit preferencesChanged();
}
double EditingBridge::audioTransitionDuration() const { return seconds(kAudioTransitionKey, kTransitionSeconds); }
void EditingBridge::setAudioTransitionDuration(double s) {
    if (std::abs(s - audioTransitionDuration()) < 1e-9)
        return;
    storeSeconds(kAudioTransitionKey, s);
    emit preferencesChanged();
}

// ---- Default transitions (0.2 UX, stream T) ------------------------------------------------------------------------

QString EditingBridge::defaultVideoTransition() const { return QString::fromStdString(defaultTransitionsSetting().video); }
QString EditingBridge::defaultAudioTransition() const { return QString::fromStdString(defaultTransitionsSetting().audio); }

void EditingBridge::setDefaultVideoTransition(const QString& effectId) {
    if (!transitionOfKind(effectId.toStdString(), EffectKind::VideoTransition) || effectId == defaultVideoTransition())
        return;
    QSettings().setValue(kDefaultVideoTransitionKey, effectId);
    emit preferencesChanged();
}

void EditingBridge::setDefaultAudioTransition(const QString& effectId) {
    if (!transitionOfKind(effectId.toStdString(), EffectKind::AudioTransition) || effectId == defaultAudioTransition())
        return;
    QSettings().setValue(kDefaultAudioTransitionKey, effectId);
    emit preferencesChanged();
}

bool EditingBridge::setDefaultTransition(const QString& effectId) {
    const std::string id = effectId.toStdString();
    if (transitionOfKind(id, EffectKind::VideoTransition)) {
        setDefaultVideoTransition(effectId);
        return true;
    }
    if (transitionOfKind(id, EffectKind::AudioTransition)) {
        setDefaultAudioTransition(effectId);
        return true;
    }
    return false;
}

bool EditingBridge::isDefaultTransition(const QString& effectId) const {
    return !effectId.isEmpty() && (effectId == defaultVideoTransition() || effectId == defaultAudioTransition());
}

QVariantList EditingBridge::transitionChoices(const QString& kind) const {
    const EffectKind wanted = kind == QLatin1String("audio") ? EffectKind::AudioTransition : EffectKind::VideoTransition;
    if (kind != QLatin1String("audio") && kind != QLatin1String("video"))
        return {};
    const QString current = wanted == EffectKind::AudioTransition ? defaultAudioTransition() : defaultVideoTransition();
    std::vector<const EffectDescriptor*> found;
    for (const EffectDescriptor& d : EffectRegistry::builtin().all()) {
        if (d.kind == wanted && !d.intrinsic)
            found.push_back(&d);
    }
    std::ranges::sort(found, [](const EffectDescriptor* a, const EffectDescriptor* b) { return a->displayName < b->displayName; });
    QVariantList out;
    for (const EffectDescriptor* d : found) {
        const QString id = text(d->id);
        out << QVariantMap{{QStringLiteral("id"), id},
                           {QStringLiteral("name"), text(d->displayName)},
                           {QStringLiteral("category"), text(d->category)},
                           {QStringLiteral("isDefault"), id == current}};
    }
    return out;
}

void EditingBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

// ---- Commands -----------------------------------------------------------------------------------------------------

command::CommandResult EditingBridge::run(const std::string& name, Json params) {
    if (!m_controller) {
        command::CommandResult r;
        r.error = command::CommandError{command::ErrorCode::InvalidRequest, "no project", nullptr};
        return r;
    }
    command::EnginePolicy policy;
    // Replace Footage reads the new file with the project's locator rules.
    const std::filesystem::path projectFile = m_controller->projectFile();
    policy.services = std::make_shared<command::MediaProbeServices>(
        [projectFile](const std::filesystem::path& file) { return editor::describeMediaFile(file, projectFile); });
    command::CommandEngine engine(m_controller->session(), command::builtinCommands(), policy);
    command::CommandRequest request;
    request.command = name;
    if (!params.contains("sequence") && m_controller->currentSequenceId().isValid())
        params["sequence"] = m_controller->currentSequenceId().value;
    request.params = std::move(params);
    return engine.execute(request);
}

QString EditingBridge::result(const command::CommandResult& r) {
    setMessage(commandErrorText(r));
    return m_message;
}

bool EditingBridge::runCommand(const QString& id) {
    if (!handlesCommand(id))
        return false;
    const QStringList librarySelection = m_library ? m_library->selection() : QStringList{};
    if (id == QLatin1String("clip.scaleToFrameSize")) {
        // The key toggles it, like the menu's check mark.
        const std::vector<ClipId> clips = targetClips({});
        const QVariantMap info = clips.empty() ? QVariantMap{} : clipVideoInfo(clipKey(clips.front()));
        scaleToFrameSize({}, !info.value(QStringLiteral("scaleToFrame")).toBool());
    } else if (id == QLatin1String("clip.nearestNeighborScaling")) {
        const std::vector<ClipId> clips = targetClips({});
        const QVariantMap info = clips.empty() ? QVariantMap{} : clipVideoInfo(clipKey(clips.front()));
        nearestNeighborScaling({}, !info.value(QStringLiteral("nearestNeighbor")).toBool());
    } else if (id == QLatin1String("clip.setToFrameSize")) {
        setToFrameSize({});
    } else if (id == QLatin1String("clip.setToFillFrame")) {
        setToFillFrame({});
    } else if (id == QLatin1String("clip.frameHoldOptions")) {
        const std::vector<ClipId> clips = targetClips({});
        openFrameHoldOptions(clips.empty() ? QString() : clipKey(clips.front()));
    } else if (id == QLatin1String("clip.fieldOptions")) {
        const std::vector<ClipId> clips = targetClips({});
        openFieldOptions(clips.empty() ? QString() : clipKey(clips.front()));
    } else if (id == QLatin1String("clip.addFrameHold") || id == QLatin1String("clip.insertFrameHoldSegment")) {
        addFrameHold({}, id == QLatin1String("clip.insertFrameHoldSegment"));
    } else if (id == QLatin1String("edit.preferencesMedia")) {
        emit preferencesRequested(QStringLiteral("media"));
    } else if (id == QLatin1String("edit.preferencesTimeline")) {
        emit preferencesRequested(QStringLiteral("timeline"));
    } else if (id == QLatin1String("edit.duplicate")) {
        duplicate(librarySelection);
    } else if (id == QLatin1String("edit.editOriginal")) {
        // The Library's selected item, else the media of the selected clip.
        QString key = librarySelection.isEmpty() ? QString() : librarySelection.front();
        if (!parseMediaKey(key)) {
            const std::vector<ClipId> clips = targetClips({});
            const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
            const auto loc = seq && !clips.empty() ? seq->findClip(clips.front()) : std::nullopt;
            key = loc ? mediaKey(loc->clip().media) : QString();
        }
        if (!editOriginal(key) && m_message.isEmpty())
            setMessage(tr("Select a clip of a media file"));
    } else if (id == QLatin1String("file.newSequenceFromClip")) {
        newSequenceFromClip(librarySelection.isEmpty() ? QString() : librarySelection.front());
    } else if (id == QLatin1String("file.newSequencesFromClips") || id == QLatin1String("file.newSequenceFromClipsTogether")) {
        newSequencesFromClips(librarySelection, id == QLatin1String("file.newSequenceFromClipsTogether"));
    } else if (id == QLatin1String("clip.reloadFootage")) {
        reloadFootage(librarySelection);
    } else if (id == QLatin1String("sequence.settings")) {
        openSequenceSettings(currentSequenceKey());
    } else if (id == QLatin1String("sequence.replaceFromSourceMonitor")) {
        replaceWithClip(QStringLiteral("sourceMonitor"));
    } else if (id == QLatin1String("sequence.replaceFromSourceMonitorMatchFrame")) {
        replaceWithClip(QStringLiteral("matchFrame"));
    } else if (id == QLatin1String("sequence.replaceFromBin")) {
        replaceWithClip(QStringLiteral("bin"));
    } else if (id == QLatin1String("sequence.makeSubsequence")) {
        makeSubsequence(false);
    } else if (id == QLatin1String("sequence.makeSubsequenceFromInToOut")) {
        makeSubsequence(true);
    } else if (id == QLatin1String("sequence.joinThroughEdits")) {
        joinThroughEdits({});
    } else if (id == QLatin1String("sequence.nextInSequence")) {
        goToGap(true, true);
    } else if (id == QLatin1String("sequence.previousInSequence")) {
        goToGap(false, true);
    } else if (id == QLatin1String("sequence.nextInTrack")) {
        goToGap(true, false);
    } else if (id == QLatin1String("sequence.previousInTrack")) {
        goToGap(false, false);
    } else if (id == QLatin1String("sequence.closeGap")) {
        closeGap();
    } else if (id == QLatin1String("sequence.removeTransitions")) {
        removeTransitions(false);
    } else if (id == QLatin1String("sequence.removeAllTransitions")) {
        removeTransitions(true);
    } else if (id == QLatin1String("sequence.removeDisabledClips")) {
        removeDisabledClips();
    } else if (id == QLatin1String("clip.mergeClips")) {
        openMergeClips(librarySelection);
    } else if (id == QLatin1String("clip.interpretFootage")) {
        openInterpretFootage(librarySelection);
    } else if (id == QLatin1String("clip.rawSourceSettings")) {
        openRawSourceSettings(librarySelection);
    } else if (id == QLatin1String("clip.modifyTimecode")) {
        openTimecode(librarySelection);
    } else if (id == QLatin1String("clip.replaceFootage")) {
        openReplaceFootage(librarySelection.isEmpty() ? QString() : librarySelection.front());
    } else if (id == QLatin1String("clip.makeOffline")) {
        makeOffline(librarySelection);
    } else if (id == QLatin1String("clip.revealInProject")) {
        revealInProject({}); // the primary selected clip
    } else if (id == QLatin1String("window.history")) {
        emit historyRequested();
    } else if (id == QLatin1String("history.stepBackward") || id == QLatin1String("history.stepForward")) {
        // Premiere's History panel: one state back or forward (an undo or a redo); nothing at either end.
        if (m_controller) {
            const int current = static_cast<int>(m_controller->session().undoCount());
            goToHistoryStep(current + (id == QLatin1String("history.stepForward") ? 1 : -1));
        }
    } else if (id == QLatin1String("history.delete")) {
        // P1 round (#4): the current step (a click on a step makes it current) and the ones after it, once confirmed.
        const int current = m_controller ? static_cast<int>(m_controller->session().undoCount()) : 0;
        if (current > 0)
            emit historyEditRequested(QStringLiteral("delete"), current);
        else
            setMessage(tr("The project as it was opened cannot be deleted"));
    } else if (id == QLatin1String("history.clear")) {
        if (m_controller && (m_controller->canUndo() || m_controller->canRedo()))
            emit historyEditRequested(QStringLiteral("clear"), -1);
        else
            setMessage(tr("The history is empty"));
    }
    return true;
}

QVariantMap EditingBridge::commandAvailability(const QString& id) const {
    const auto state = [](bool enabled, const QString& reason = {}) {
        return QVariantMap{{QStringLiteral("handled"), true}, {QStringLiteral("enabled"), enabled},
                           {QStringLiteral("reason"), enabled ? QString() : reason}};
    };
    if (!handlesCommand(id))
        return {{QStringLiteral("handled"), false}, {QStringLiteral("enabled"), false}, {QStringLiteral("reason"), QString()}};
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const QStringList librarySelection = m_library ? m_library->selection() : QStringList{};
    const std::vector<ClipId> clips = targetClips({});
    // Clip > Video Options: the selected clips (Frame Hold and Field Options: a video one).
    const auto hasVideo = [&] {
        return std::ranges::any_of(clips, [&](ClipId c) {
            const auto loc = seq ? seq->findClip(c) : std::nullopt;
            return loc && loc->clip().stream.kind == StreamKind::Video;
        });
    };
    if (id == QLatin1String("clip.scaleToFrameSize") || id == QLatin1String("clip.setToFrameSize") ||
        id == QLatin1String("clip.setToFillFrame") || id == QLatin1String("clip.nearestNeighborScaling"))
        return state(!clips.empty(), tr("Select a clip first"));
    if (id == QLatin1String("clip.frameHoldOptions") || id == QLatin1String("clip.fieldOptions") ||
        id == QLatin1String("clip.addFrameHold") || id == QLatin1String("clip.insertFrameHoldSegment"))
        return state(hasVideo(), tr("Select a video clip"));
    // The Library's selection.
    if (id == QLatin1String("edit.duplicate"))
        return state(!librarySelection.isEmpty(), tr("Select items in the Library to duplicate"));
    if (id == QLatin1String("file.newSequenceFromClip") || id == QLatin1String("clip.replaceFootage"))
        return state(!librarySelection.isEmpty() && parseMediaKey(librarySelection.front()).has_value(),
                     tr("Select a clip in the Library"));
    if (id == QLatin1String("file.newSequencesFromClips") || id == QLatin1String("file.newSequenceFromClipsTogether"))
        return state(mediaOf({}).size() >= 2, tr("Select two or more clips in the Library"));
    if (id == QLatin1String("clip.reloadFootage"))
        return state(!mediaOf({}).empty(), tr("Select clips in the Library to reload"));
    if (id == QLatin1String("clip.mergeClips"))
        return state(mediaOf({}).size() >= 2, tr("Select a video clip and the audio clips to merge with it"));
    if (id == QLatin1String("clip.interpretFootage"))
        return state(!mediaOf({}).empty(), tr("Select clips in the Library to interpret"));
    if (id == QLatin1String("clip.modifyTimecode"))
        return state(!mediaOf({}).empty(), tr("Select clips in the Library"));
    if (id == QLatin1String("clip.makeOffline"))
        return state(!mediaOf({}).empty(), tr("Select clips in the Library to make offline"));
    if (id == QLatin1String("clip.rawSourceSettings"))
        return state(!rawMediaOf({}).empty(),
                     tr("Source Settings develop camera RAW clips (ProRes RAW, CinemaDNG): select one in the Library"));
    if (id == QLatin1String("edit.editOriginal")) {
        const bool libraryItem = !librarySelection.isEmpty() && parseMediaKey(librarySelection.front()).has_value();
        const bool clipMedia = std::ranges::any_of(clips, [&](ClipId c) {
            const auto loc = seq ? seq->findClip(c) : std::nullopt;
            return loc && loc->clip().media.isValid();
        });
        return state(libraryItem || clipMedia, tr("Select a clip of a media file"));
    }
    if (id == QLatin1String("clip.revealInProject"))
        return state(revealInfo({}).value(QStringLiteral("inProject")).toBool(), tr("Select a clip first"));
    // The History panel's keys.
    if (id == QLatin1String("history.stepBackward"))
        return state(m_controller && m_controller->canUndo(), tr("Nothing to undo"));
    if (id == QLatin1String("history.stepForward"))
        return state(m_controller && m_controller->canRedo(), tr("Nothing to redo"));
    if (id == QLatin1String("history.delete"))
        return state(m_controller && m_controller->canUndo(), tr("The project as it was opened cannot be deleted"));
    if (id == QLatin1String("history.clear"))
        return state(m_controller && (m_controller->canUndo() || m_controller->canRedo()), tr("The history is empty"));
    // The rest act on the open sequence.
    if (id == QLatin1String("edit.preferencesMedia") || id == QLatin1String("edit.preferencesTimeline") ||
        id == QLatin1String("window.history"))
        return state(true);
    if (!seq)
        return state(false, tr("Open a sequence first"));
    if (id == QLatin1String("sequence.replaceFromSourceMonitor") ||
        id == QLatin1String("sequence.replaceFromSourceMonitorMatchFrame")) {
        if (clips.empty())
            return state(false, tr("Select the clip to replace"));
        return state(m_source && m_source->mediaId().isValid(),
                     tr("Load the clip to replace it with in the Source Monitor"));
    }
    if (id == QLatin1String("sequence.replaceFromBin")) {
        if (clips.empty())
            return state(false, tr("Select the clip to replace"));
        return state(!mediaOf({}).empty(), tr("Select the clip to replace it with in the Library"));
    }
    if (id == QLatin1String("sequence.makeSubsequenceFromInToOut"))
        return state(seq->markedRange().has_value(), tr("Set an In and an Out in the sequence first"));
    if (id == QLatin1String("sequence.makeSubsequence"))
        return state(!clips.empty() || seq->markedRange().has_value(), tr("Select clips or set an In and an Out first"));
    if (id == QLatin1String("sequence.closeGap")) {
        // Between the selected clips, else every gap of the sequence (the command's own rule, run on a copy).
        const ProjectSnapshot snap = m_controller->snapshot();
        return state(snap && edit::closeGaps(*snap, seq->id, clips).has_value(), tr("There is no gap to close"));
    }
    // 0.7 (cards 123, 122): enabled when the command would do something (its own rules, run on a copy).
    if (id == QLatin1String("sequence.removeTransitions")) {
        if (clips.empty())
            return state(false, tr("Select clips first"));
        const ProjectSnapshot snap = m_controller->snapshot();
        return state(snap && edit::removeTransitions(*snap, seq->id, clips).has_value(), tr("The selected clips have no transitions"));
    }
    if (id == QLatin1String("sequence.removeAllTransitions")) {
        const ProjectSnapshot snap = m_controller->snapshot();
        return state(snap && edit::removeTransitions(*snap, seq->id, {}).has_value(), tr("The sequence has no transitions to remove"));
    }
    if (id == QLatin1String("sequence.removeDisabledClips")) {
        const ProjectSnapshot snap = m_controller->snapshot();
        return state(snap && edit::removeDisabledClips(*snap, seq->id).has_value(), tr("There are no disabled clips to remove"));
    }
    return state(true); // Sequence Settings, Join Through Edits, Go to Gap: the open sequence is enough
}

std::vector<ClipId> EditingBridge::targetClips(const QString& key) const {
    if (!m_controller)
        return {};
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return {};
    std::vector<ClipId> selected;
    for (const ClipId id : m_controller->selection()) {
        if (seq->findClip(id))
            selected.push_back(id);
    }
    const auto id = parseClipKey(key);
    if (!id || std::find(selected.begin(), selected.end(), *id) != selected.end())
        return selected;
    return seq->findClip(*id) ? std::vector<ClipId>{*id} : std::vector<ClipId>{};
}

QString EditingBridge::runOnClips(const char* name, const std::vector<ClipId>& clips, Json extra) {
    if (!m_controller || clips.empty()) {
        setMessage(tr("Select a clip first"));
        return m_message;
    }
    extra["clips"] = idArray(clips);
    return result(run(name, std::move(extra)));
}

std::vector<MediaId> EditingBridge::mediaOf(const QStringList& keys) const {
    std::vector<MediaId> out;
    for (const QString& key : keys.isEmpty() && m_library ? m_library->selection() : keys) {
        if (const auto id = parseMediaKey(key))
            out.push_back(*id);
    }
    return out;
}

std::vector<MediaId> EditingBridge::rawMediaOf(const QStringList& keys) const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    std::vector<MediaId> out;
    for (const MediaId id : mediaOf(keys)) {
        const MediaItem* item = snap ? snap->findMedia(id) : nullptr;
        if (item && !item->isGenerator() && wabcut::isRawMedia(item->info))
            out.push_back(id);
    }
    return out;
}

// ---- Clip > Video Options -------------------------------------------------------------------------------------------

QVariantMap EditingBridge::clipVideoInfo(const QString& key) const {
    const auto id = parseClipKey(key);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    if (!loc)
        return {};
    // The clip itself or, for an audio clip, a linked video partner.
    const Clip* video = loc->clip().stream.kind == StreamKind::Video ? &loc->clip() : nullptr;
    if (!video && loc->clip().linkGroup.isValid()) {
        seq->forEachTrack([&](const Track& t) {
            if (video || t.kind != TrackKind::Video)
                return;
            for (const Clip& c : t.clips) {
                if (c.linkGroup == loc->clip().linkGroup) {
                    video = &c;
                    return;
                }
            }
        });
    }
    // A through edit at either edge of the clip.
    const Track& track = *loc->track;
    const std::size_t i = loc->index;
    const bool through = (i > 0 && edit::isThroughEdit(track.clips[i - 1], track.clips[i])) ||
                         (i + 1 < track.clips.size() && edit::isThroughEdit(track.clips[i], track.clips[i + 1]));
    return {{QStringLiteral("video"), video != nullptr},
            {QStringLiteral("scaleToFrame"), video && video->content().scaleToFrame},
            {QStringLiteral("nearestNeighbor"), video && video->content().nearestNeighbor},
            {QStringLiteral("frozen"), video && video->isFreeze()},
            {QStringLiteral("throughEdit"), through}};
}

QString EditingBridge::scaleToFrameSize(const QString& key, bool enabled) {
    return runOnClips("clip.scaleToFrameSize", targetClips(key), Json{{"enabled", enabled}});
}

QString EditingBridge::nearestNeighborScaling(const QString& key, bool enabled) {
    return runOnClips("clip.nearestNeighborScaling", targetClips(key), Json{{"enabled", enabled}});
}

QString EditingBridge::setToFrameSize(const QString& key) { return runOnClips("clip.setToFrameSize", targetClips(key)); }

QString EditingBridge::setToFillFrame(const QString& key) { return runOnClips("clip.setToFillFrame", targetClips(key)); }

QString EditingBridge::addFrameHold(const QString& key, bool insertSegment) {
    if (!m_controller)
        return {};
    // The video clip under the playhead among the targets.
    const SequencePtr seq = m_controller->currentSequence();
    const Ticks playhead = m_controller->playhead();
    for (const ClipId id : targetClips(key)) {
        const auto loc = seq ? seq->findClip(id) : std::nullopt;
        if (!loc || loc->clip().stream.kind != StreamKind::Video || playhead < loc->clip().start() ||
            playhead >= loc->clip().end())
            continue;
        Json params{{"clip", id.value}, {"time", playhead.value}};
        if (insertSegment)
            params["duration"] = (Ticks::fromSeconds(2)).value;
        return result(run("clip.frameHold", std::move(params)));
    }
    setMessage(tr("Put the playhead over the selected video clip"));
    return m_message;
}

QVariantMap EditingBridge::frameHoldInfo(const QString& key) const {
    const auto id = parseClipKey(key);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    if (!loc || loc->clip().stream.kind != StreamKind::Video)
        return {{QStringLiteral("available"), false}};
    const Clip& c = loc->clip();
    const Ticks playhead = std::clamp(m_controller->playhead(), c.start(), std::max(c.start(), c.end() - Ticks{1}));
    return {{QStringLiteral("available"), true},
            {QStringLiteral("frozen"), c.isFreeze()},
            {QStringLiteral("holdFilters"), c.content().holdFilters},
            {QStringLiteral("playhead"), m_controller->formatTimecode(playhead)},
            {QStringLiteral("name"), QString::fromStdString(c.name())}};
}

QString EditingBridge::applyFrameHoldOptions(const QString& key, const QVariantMap& options) {
    const auto id = parseClipKey(key);
    if (!id) {
        setMessage(tr("Choose a video clip"));
        return m_message;
    }
    Json params = jsonOf(options);
    params["clip"] = id->value;
    if (!params.contains("time") && m_controller)
        params["time"] = m_controller->playhead().value;
    return result(run("clip.frameHoldOptions", std::move(params)));
}

void EditingBridge::openFrameHoldOptions(const QString& key) {
    if (!parseClipKey(key)) {
        setMessage(tr("Select a video clip"));
        return;
    }
    emit frameHoldOptionsRequested(key);
}

QVariantMap EditingBridge::fieldOptionsInfo(const QString& key) const {
    const auto id = parseClipKey(key);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    if (!loc || loc->clip().stream.kind != StreamKind::Video)
        return {{QStringLiteral("available"), false}};
    const Clip& c = loc->clip();
    const ClipFieldOptions& options = c.content().fieldOptions;
    FieldOrder order = FieldOrder::Progressive;
    if (const ProjectSnapshot snap = m_controller->snapshot(); snap && !c.isNested()) {
        if (const MediaItem* item = snap->findMedia(c.media))
            order = effectiveFieldOrder(*item, c.stream.index);
    }
    return {{QStringLiteral("available"), true},
            {QStringLiteral("name"), QString::fromStdString(c.name())},
            {QStringLiteral("reverseFieldDominance"), options.reverseFieldDominance},
            {QStringLiteral("processing"), QString::fromUtf8(toString(options.processing))},
            {QStringLiteral("fieldOrder"), QString::fromUtf8(toString(order))}};
}

QString EditingBridge::applyFieldOptions(const QString& key, const QVariantMap& options) {
    Json params{{"reverseFieldDominance", options.value(QStringLiteral("reverseFieldDominance")).toBool()},
                {"processing", options.value(QStringLiteral("processing"), QStringLiteral("none")).toString().toStdString()}};
    return runOnClips("clip.setFieldOptions", targetClips(key), std::move(params));
}

void EditingBridge::openFieldOptions(const QString& key) {
    if (!parseClipKey(key)) {
        setMessage(tr("Select a video clip"));
        return;
    }
    emit fieldOptionsRequested(key);
}

// ---- Duplicate, New Sequence From Clip, Sequence Settings ---------------------------------------------------------------

QString EditingBridge::duplicate(const QStringList& keys) {
    if (!m_controller || keys.isEmpty()) {
        setMessage(tr("Select items in the Library to duplicate"));
        return m_message;
    }
    command::CommandBatch batch;
    batch.label = "Duplicate";
    for (const QString& key : keys) {
        command::CommandRequest r;
        if (const auto media = parseMediaKey(key)) {
            r.command = "media.duplicate";
            r.params = Json{{"media", media->value}};
        } else if (const auto seq = parseSequenceKey(key)) {
            r.command = "sequence.duplicate";
            r.params = Json{{"sequence", seq->value}};
        } else {
            continue; // bins are not duplicated
        }
        batch.commands.push_back(std::move(r));
    }
    if (batch.commands.empty()) {
        setMessage(tr("Only clips and sequences are duplicated"));
        return m_message;
    }
    command::CommandEngine engine(m_controller->session(), command::builtinCommands());
    return result(engine.execute(batch));
}

QString EditingBridge::newSequenceFromClip(const QString& key) {
    const auto media = parseMediaKey(key);
    if (!media) {
        setMessage(tr("Select a clip in the Library"));
        return m_message;
    }
    const command::CommandResult r = run("sequence.newFromClip", Json{{"media", media->value}});
    if (r.ok() && !r.createdIds.empty() && m_tabs)
        m_tabs->open(sequenceKey(SequenceId{r.createdIds.front()}));
    return result(r);
}

QString EditingBridge::newSequencesFromClips(const QStringList& keys, bool together) {
    const std::vector<MediaId> media = mediaOf(keys);
    if (media.empty()) {
        setMessage(tr("Select clips in the Library"));
        return m_message;
    }
    Json params{{"media", mediaArray(media)}, {"mode", together ? "together" : "each"}};
    const command::CommandResult r = run("sequence.newFromClips", std::move(params));
    if (r.ok() && !r.createdIds.empty() && m_tabs)
        m_tabs->open(sequenceKey(SequenceId{r.createdIds.front()}));
    return result(r);
}

QString EditingBridge::currentSequenceKey() const {
    return m_controller ? sequenceKey(m_controller->currentSequenceId()) : QString();
}

QVariantMap EditingBridge::sequenceSettings(const QString& key) const {
    const auto id = parseSequenceKey(key);
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const Sequence* seq = id && snap ? snap->findSequence(*id) : nullptr;
    if (!seq)
        return {};
    return {{QStringLiteral("name"), QString::fromStdString(seq->name)},
            {QStringLiteral("fps"), seq->frameRate.toDouble()},
            {QStringLiteral("frameRateNum"), seq->frameRate.num},
            {QStringLiteral("frameRateDen"), seq->frameRate.den},
            {QStringLiteral("width"), seq->width},
            {QStringLiteral("height"), seq->height},
            {QStringLiteral("pixelAspect"), seq->pixelAspect.toDouble()},
            {QStringLiteral("fields"), text(toString(seq->fields))},
            {QStringLiteral("audioRate"), seq->audioRate.num / std::max(1, seq->audioRate.den)},
            {QStringLiteral("dropFrame"), seq->dropFrame},
            {QStringLiteral("dropFrameAvailable"), supportsDropFrame(seq->frameRate)},
            {QStringLiteral("previewFormat"), QString::fromStdString(seq->previewFormat)},
            // 0.7 (VR / 360 video)
            {QStringLiteral("vrProjection"), text(toString(seq->vr.projection))},
            {QStringLiteral("vrStereo"), text(toString(seq->vr.stereo))},
            {QStringLiteral("videoDisplay"), text(toString(seq->videoDisplay))},
            {QStringLiteral("audioDisplay"), text(toString(seq->audioDisplay))},
            {QStringLiteral("startTimecode"),
             QString::fromStdString(ticksToTimecode(seq->startTimecode, seq->frameRate, seq->dropFrame))},
            // 0.7 (card 046): the work area as timecodes ("" when there is none)
            {QStringLiteral("workAreaIn"),
             seq->workArea ? QString::fromStdString(ticksToTimecode(seq->workArea->start, seq->frameRate, seq->dropFrame)) : QString()},
            {QStringLiteral("workAreaOut"),
             seq->workArea ? QString::fromStdString(ticksToTimecode(seq->workArea->end(), seq->frameRate, seq->dropFrame)) : QString()}};
}

QString EditingBridge::applySequenceSettings(const QString& key, const QVariantMap& settings) {
    const auto id = parseSequenceKey(key);
    if (!id) {
        setMessage(tr("Choose a sequence"));
        return m_message;
    }
    Json params = jsonOf(settings);
    params["sequence"] = id->value;
    return result(run("sequence.setSettings", std::move(params)));
}

QVariantList EditingBridge::sequencePresets() const {
    QVariantList out;
    for (const SequencePreset& p : wabcut::sequencePresets()) {
        out.append(QVariantMap{{QStringLiteral("id"), text(p.id)},
                               {QStringLiteral("name"), text(p.name)},
                               {QStringLiteral("group"), text(p.group)},
                               {QStringLiteral("width"), p.width},
                               {QStringLiteral("height"), p.height},
                               {QStringLiteral("fps"), p.frameRate.toDouble()},
                               {QStringLiteral("dropFrame"), p.dropFrame}});
    }
    return out;
}

void EditingBridge::openSequenceSettings(const QString& key) {
    if (!parseSequenceKey(key)) {
        setMessage(tr("Open a sequence first"));
        return;
    }
    emit sequenceSettingsRequested(key);
}

// ---- Replace, Replace Footage, Make Offline, Edit Original ------------------------------------------------------------

QString EditingBridge::replaceWithClip(const QString& mode) {
    const std::vector<ClipId> clips = targetClips({});
    if (clips.empty()) {
        setMessage(tr("Select the clip to replace"));
        return m_message;
    }
    Json params;
    if (mode == QLatin1String("bin")) {
        const std::vector<MediaId> media = mediaOf({});
        if (media.empty()) {
            setMessage(tr("Select the clip to replace it with in the Library"));
            return m_message;
        }
        params["media"] = media.front().value;
    } else {
        if (!m_source || !m_source->mediaId().isValid()) {
            setMessage(tr("Load the clip to replace it with in the Source Monitor"));
            return m_message;
        }
        params["media"] = m_source->mediaId().value;
        if (mode == QLatin1String("matchFrame") && m_controller)
            params["matchFrame"] = Json{{"sourceTime", m_source->positionTicks().value}, {"at", m_controller->playhead().value}};
    }
    return runOnClips("clip.replace", clips, std::move(params));
}

bool EditingBridge::replaceModifierHeld(bool fromSourceMonitor) const {
    const Qt::KeyboardModifiers mods = QGuiApplication::queryKeyboardModifiers();
    return fromSourceMonitor ? mods.testFlag(Qt::ShiftModifier)
                             : (mods.testFlag(Qt::AltModifier) || mods.testFlag(Qt::ShiftModifier));
}

bool EditingBridge::replaceDrop(const QString& clipKeyText, const QString& mediaKeyText, bool fromSourceMonitor) {
    const auto clip = parseClipKey(clipKeyText);
    if (!clip)
        return false;
    MediaId media;
    if (const auto parsed = parseMediaKey(mediaKeyText))
        media = *parsed;
    else if (fromSourceMonitor && m_source)
        media = m_source->mediaId();
    if (!media.isValid())
        return false;
    return result(run("clip.replace", Json{{"clips", Json::array({clip->value})}, {"media", media.value}})).isEmpty();
}

QString EditingBridge::replaceFootage(const QString& key, const QUrl& file, bool rename) {
    const auto media = parseMediaKey(key);
    if (!media || !file.isLocalFile()) {
        setMessage(tr("Choose a clip and the file to replace its footage with"));
        return m_message;
    }
    return result(run("media.replaceFootage",
                      Json{{"media", media->value}, {"path", file.toLocalFile().toStdString()}, {"rename", rename}}));
}

QString EditingBridge::makeOffline(const QStringList& keys) {
    const std::vector<MediaId> media = mediaOf(keys);
    if (media.empty()) {
        setMessage(tr("Select clips in the Library to make offline"));
        return m_message;
    }
    return result(run("media.makeOffline", Json{{"media", mediaArray(media)}}));
}

QString EditingBridge::reloadFootage(const QStringList& keys) {
    const std::vector<MediaId> media = mediaOf(keys);
    if (media.empty()) {
        setMessage(tr("Select clips in the Library to reload"));
        return m_message;
    }
    if (!m_controller) {
        setMessage(tr("There is no project"));
        return m_message;
    }
    if (m_reloading) {
        setMessage(tr("Reload Footage is already running"));
        return m_message;
    }
    // The files are read on a worker thread (a probe opens each file: a network drive can take seconds) with progress
    // and Cancel; the reloaded project is committed on the GUI thread as one undo step (CommandJob).
    const std::filesystem::path projectFile = m_controller->projectFile();
    auto services = std::make_shared<command::MediaProbeServices>(
        [projectFile](const std::filesystem::path& file) { return editor::describeMediaFile(file, projectFile); });
    setMessage({});
    m_reloadFailure.clear();
    setReload(true, tr("Reading the files"), -1);
    QPointer<EditingBridge> self(this);
    const bool started = m_reloadJob->start(
        this, m_controller, std::move(services),
        {"media.reload", Json{{"media", mediaArray(media)}}, QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Reload Footage")},
        [self](const QString& text, double fraction) {
            if (self)
                self->setReload(true, text.isEmpty() ? self->m_reloadPhase : text, fraction);
        },
        [self](const command::CommandResult& r) {
            if (!self)
                return;
            self->setReload(false, {}, -1);
            self->finishReload(r);
        });
    if (!started) {
        setReload(false, {}, -1);
        setMessage(tr("Reload Footage is already running"));
        return m_message;
    }
    return m_reloadFailure; // only a job that already ended (tests run it on this thread) can have failed by now
}

void EditingBridge::cancelReload() {
    if (m_reloadJob)
        m_reloadJob->cancel();
}

void EditingBridge::setReload(bool running, const QString& phase, double progress) {
    if (m_reloading == running && m_reloadPhase == phase && qFuzzyCompare(m_reloadProgress + 1.0, progress + 1.0))
        return;
    m_reloading = running;
    m_reloadPhase = phase;
    m_reloadProgress = progress;
    emit reloadChanged();
}

void EditingBridge::finishReload(const command::CommandResult& r) {
    if (!r.ok()) {
        m_reloadFailure = result(r);
        return;
    }
    // What happened, in words: changed files, files that read the same, what could not be read, clips cut short.
    const auto count = [&](const char* key) { return r.data.contains(key) ? r.data[key].get<int>() : 0; };
    const int reloaded = count("reloaded");
    const int failed = r.data.contains("failed") ? static_cast<int>(r.data["failed"].size()) : 0;
    const int warned = r.data.contains("warnings") ? static_cast<int>(r.data["warnings"].size()) : 0;
    QString said = reloaded > 0 ? tr("Reloaded %n clip(s) from disk.", "", reloaded) : tr("The files read the same as before: nothing to reload.");
    if (failed > 0)
        said += QLatin1Char(' ') + tr("%n file(s) could not be read.", "", failed);
    if (warned > 0)
        said += QLatin1Char(' ') + tr("%n clip(s) now run past the end of a shorter file.", "", warned);
    setMessage(said);
}

bool EditingBridge::editOriginal(const QString& key) {
    const auto media = parseMediaKey(key);
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const MediaItem* item = media && snap ? snap->findMedia(*media) : nullptr;
    if (!item || item->isGenerator() || item->locator.path.empty())
        return false;
    const QString path = QString::fromStdString(item->locator.path);
    if (!QFileInfo::exists(path)) {
        setMessage(tr("%1 is offline: its file is not there").arg(QString::fromStdString(item->name)));
        return false;
    }
    setMessage({});
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void EditingBridge::openReplaceFootage(const QString& key) {
    if (!parseMediaKey(key)) {
        setMessage(tr("Select a clip in the Library"));
        return;
    }
    emit replaceFootageRequested(key);
}

// ---- Subsequence, through edits, gaps --------------------------------------------------------------------------------

QString EditingBridge::makeSubsequence(bool fromInOut) {
    const std::vector<ClipId> clips = fromInOut ? std::vector<ClipId>{} : targetClips({});
    Json params = Json::object();
    if (!clips.empty())
        params["clips"] = idArray(clips);
    const command::CommandResult r = run("sequence.makeSubsequence", std::move(params));
    if (r.ok())
        emit libraryRequested();
    return result(r);
}

QString EditingBridge::joinThroughEdits(const QString& key) {
    const std::vector<ClipId> clips = key.isEmpty() ? targetClips({}) : targetClips(key);
    Json params = Json::object();
    if (!clips.empty())
        params["clips"] = idArray(clips);
    return result(run("sequence.joinThroughEdits", std::move(params)));
}

bool EditingBridge::goToGap(bool next, bool inSequence) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return false;
    Json params{{"time", m_controller->playhead().value},
                {"direction", next ? "next" : "previous"},
                {"scope", inSequence ? "sequence" : "track"}};
    // Next / Previous in Track look at the targeted tracks; with none targeted every track counts (the key still works).
    const std::vector<TrackId> targeted = edit::targetedTracks(*seq);
    if (!inSequence && targeted.empty()) {
        Json all = Json::array();
        seq->forEachTrack([&](const Track& t) { all.push_back(t.id.value); });
        params["tracks"] = std::move(all);
    }
    const command::CommandResult r = run("sequence.goToGap", std::move(params));
    result(r);
    if (!r.ok() || !r.data.contains("time") || r.data["time"].is_null())
        return false;
    const Ticks at{r.data["time"].value("ticks", std::int64_t{0})};
    m_controller->setPlayhead(at);
    if (!inSequence)
        selectGapAtPlayhead(at, targeted);
    return true;
}

// 0.7 (card 007): the in-Track jumps also select the gap they land on, so Delete / Backspace / Shift+Delete take it out
// as they do a clicked gap. The gap that starts at `at` on the first of the targeted tracks (every track when none is
// targeted) wins; failing that, a gap on any of them that covers `at`.
void EditingBridge::selectGapAtPlayhead(Ticks at, const std::vector<TrackId>& targeted) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return;
    std::vector<TrackId> tracks = targeted;
    if (tracks.empty())
        seq->forEachTrack([&](const Track& t) { tracks.push_back(t.id); });
    for (const bool exactStart : {true, false}) {
        for (const TrackId track : tracks) {
            const auto gap = m_controller->gapAt(track, at);
            if (gap && (!exactStart || gap->range.start == at) && m_controller->selectGapAt(track, at))
                return;
        }
    }
}

QString EditingBridge::closeGap() {
    const std::vector<ClipId> clips = targetClips({});
    Json params = Json::object();
    if (!clips.empty())
        params["clips"] = idArray(clips);
    return result(run("sequence.closeGap", std::move(params)));
}

QString EditingBridge::removeTransitions(bool wholeSequence) {
    const std::vector<ClipId> clips = wholeSequence ? std::vector<ClipId>{} : targetClips({});
    if (!wholeSequence && clips.empty()) {
        setMessage(tr("Select clips first"));
        return m_message;
    }
    Json params = Json::object();
    if (!clips.empty())
        params["clips"] = idArray(clips);
    return result(run("sequence.removeTransitions", std::move(params)));
}

QString EditingBridge::removeDisabledClips() {
    return result(run("sequence.removeDisabledClips", Json::object()));
}

QString EditingBridge::closeGapsOnTrack(const QString& trackKey) {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto id = parseTrackKey(trackKey);
    const Track* track = id && seq ? seq->findTrack(*id) : nullptr;
    if (!track) {
        setMessage(tr("There is no such track"));
        return m_message;
    }
    if (track->clips.empty()) {
        // closeGap with no clips means every gap of the sequence: never what "on this track" asks for.
        setMessage(tr("There is no gap to close on this track"));
        return m_message;
    }
    std::vector<ClipId> clips;
    clips.reserve(track->clips.size());
    for (const Clip& c : track->clips)
        clips.push_back(c.id);
    Json params = Json::object();
    params["clips"] = idArray(clips);
    return result(run("sequence.closeGap", std::move(params)));
}

// ---- Merge Clips, Interpret Footage, Timecode --------------------------------------------------------------------------

QString EditingBridge::mergeClips(const QStringList& keys, const QString& sync, bool removeCameraAudio, const QString& name) {
    const std::vector<MediaId> media = mediaOf(keys);
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!snap || media.size() < 2) {
        setMessage(tr("Select a video clip and the audio clips to merge with it"));
        return m_message;
    }
    // The first item with video is the camera; the rest are the recordings.
    MediaId video;
    Json audio = Json::array();
    for (const MediaId id : media) {
        const MediaItem* item = snap->findMedia(id);
        if (!video.isValid() && item && item->info.hasVideo())
            video = id;
        else
            audio.push_back(id.value);
    }
    Json params{{"audio", std::move(audio)}, {"sync", sync.toStdString()}, {"removeCameraAudio", removeCameraAudio}};
    if (video.isValid())
        params["video"] = video.value;
    if (!name.trimmed().isEmpty())
        params["name"] = name.trimmed().toStdString();
    command::CommandResult r;
    if (sync == QLatin1String("audio")) {
        // Syncing by the waveforms needs the audio analysis.
        command::EnginePolicy policy;
        policy.services = appAudioServices({.projectFile = m_controller->projectFile()});
        command::CommandEngine engine(m_controller->session(), command::builtinCommands(), policy);
        command::CommandRequest request;
        request.command = "media.mergeClips";
        request.params = std::move(params);
        r = engine.execute(request);
    } else {
        r = run("media.mergeClips", std::move(params));
    }
    if (r.ok())
        emit libraryRequested();
    return result(r);
}

QVariantMap EditingBridge::interpretation(const QStringList& keys) const {
    const std::vector<MediaId> media = mediaOf(keys);
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const MediaItem* item = snap && !media.empty() ? snap->findMedia(media.front()) : nullptr;
    if (!item)
        return {};
    const MediaInterpretation in = item->interpretation.value_or(MediaInterpretation{});
    const VideoStreamInfo* v = item->info.video.empty() ? nullptr : &item->info.video.front();
    const auto name = [](std::optional<AlphaMode> m) {
        return !m ? QString() : *m == AlphaMode::Premultiplied ? QStringLiteral("premultiplied") : QStringLiteral("straight");
    };
    return {{QStringLiteral("name"), QString::fromStdString(item->name)},
            {QStringLiteral("count"), static_cast<int>(media.size())},
            {QStringLiteral("frameRate"), in.frameRate ? in.frameRate->toDouble() : 0.0},
            {QStringLiteral("fileRate"), v && v->frameRate.isValid() ? v->frameRate.toDouble() : 0.0},
            {QStringLiteral("pixelAspect"), in.pixelAspect ? in.pixelAspect->toDouble() : 0.0},
            {QStringLiteral("filePixelAspect"), v && v->sampleAspect.isValid() ? v->sampleAspect.toDouble() : 1.0},
            {QStringLiteral("fieldOrder"), in.fieldOrder ? text(toString(*in.fieldOrder)) : QString()},
            {QStringLiteral("ignoreAlpha"), in.ignoreAlpha},
            {QStringLiteral("invertAlpha"), in.invertAlpha},
            {QStringLiteral("premultiplication"), name(in.premultiplication)},
            {QStringLiteral("hasAlpha"), v && v->alpha != AlphaMode::None},
            // 0.7 (VR / 360 video): the interpretation's projection and eye layout ("" = the file's) and what the file says.
            {QStringLiteral("vrProjection"), in.vr ? text(toString(in.vr->projection)) : QString()},
            {QStringLiteral("vrStereo"), in.vr ? text(toString(in.vr->stereo)) : QStringLiteral("mono")},
            {QStringLiteral("fileVrProjection"), v ? text(toString(v->vr.projection)) : QStringLiteral("none")},
            {QStringLiteral("fileVrStereo"), v ? text(toString(v->vr.stereo)) : QStringLiteral("mono")},
            {QStringLiteral("hasVideo"), v != nullptr}};
}

QString EditingBridge::applyInterpretation(const QStringList& keys, const QVariantMap& settings) {
    const std::vector<MediaId> media = mediaOf(keys);
    if (media.empty()) {
        setMessage(tr("Select clips in the Library to interpret"));
        return m_message;
    }
    // 0 or "" means "use the file's" (null for the command).
    Json params{{"media", mediaArray(media)}};
    const double rate = settings.value(QStringLiteral("frameRate")).toDouble();
    params["frameRate"] = rate > 0.0 ? Json(rate) : Json(nullptr);
    const double par = settings.value(QStringLiteral("pixelAspect")).toDouble();
    params["pixelAspect"] = par > 0.0 ? Json(par) : Json(nullptr);
    const QString fields = settings.value(QStringLiteral("fieldOrder")).toString();
    params["fieldOrder"] = fields.isEmpty() ? Json(nullptr) : Json(fields.toStdString());
    params["ignoreAlpha"] = settings.value(QStringLiteral("ignoreAlpha")).toBool();
    params["invertAlpha"] = settings.value(QStringLiteral("invertAlpha")).toBool();
    const QString premult = settings.value(QStringLiteral("premultiplication")).toString();
    params["premultiplication"] = premult.isEmpty() ? Json(nullptr) : Json(premult.toStdString());
    // 0.7: "" = what the file's spherical metadata says; "none" says the footage is flat.
    const QString projection = settings.value(QStringLiteral("vrProjection")).toString();
    if (projection.isEmpty()) {
        params["vr"] = Json(nullptr);
    } else {
        const QString stereo = settings.value(QStringLiteral("vrStereo")).toString();
        params["vr"] = Json{{"projection", projection.toStdString()},
                            {"stereo", projection == QLatin1String("none") || stereo.isEmpty() ? "mono" : stereo.toStdString()}};
    }
    return result(run("media.interpretFootage", std::move(params)));
}

QVariantMap EditingBridge::mediaTimecode(const QStringList& keys) const {
    const std::vector<MediaId> media = mediaOf(keys);
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const MediaItem* item = snap && !media.empty() ? snap->findMedia(media.front()) : nullptr;
    if (!item)
        return {};
    const Rate rate = command::sourceTimecodeRate(*snap, *item);
    const auto format = [&](std::optional<Ticks> t) {
        return t ? QString::fromStdString(ticksToTimecode(*t, rate, false)) : QString();
    };
    return {{QStringLiteral("name"), QString::fromStdString(item->name)},
            {QStringLiteral("count"), static_cast<int>(media.size())},
            {QStringLiteral("timecode"), format(item->timecode ? item->timecode->start : std::nullopt)},
            {QStringLiteral("fileTimecode"), format(item->info.startTimecode)},
            {QStringLiteral("reelName"), item->timecode ? QString::fromStdString(item->timecode->reelName) : QString()}};
}

QString EditingBridge::applyMediaTimecode(const QStringList& keys, const QString& timecode, const QString& reelName) {
    const std::vector<MediaId> media = mediaOf(keys);
    if (media.empty()) {
        setMessage(tr("Select clips in the Library"));
        return m_message;
    }
    Json params{{"media", mediaArray(media)}, {"reelName", reelName.trimmed().toStdString()}};
    params["timecode"] = timecode.trimmed().isEmpty() ? Json(nullptr) : Json(timecode.trimmed().toStdString());
    return result(run("media.setTimecode", std::move(params)));
}

void EditingBridge::openMergeClips(const QStringList& keys) {
    const QStringList chosen = keys.isEmpty() && m_library ? m_library->selection() : keys;
    if (mediaOf(chosen).size() < 2) {
        setMessage(tr("Select a video clip and the audio clips to merge with it"));
        return;
    }
    emit mergeClipsRequested(chosen);
}

void EditingBridge::openInterpretFootage(const QStringList& keys) {
    const QStringList chosen = keys.isEmpty() && m_library ? m_library->selection() : keys;
    if (mediaOf(chosen).empty()) {
        setMessage(tr("Select clips in the Library to interpret"));
        return;
    }
    emit interpretFootageRequested(chosen);
}

bool EditingBridge::isRawMedia(const QString& key) const {
    return !key.isEmpty() && !rawMediaOf({key}).empty();
}

QVariantMap EditingBridge::rawSourceSettings(const QStringList& keys) const {
    const std::vector<MediaId> media = rawMediaOf(keys);
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    const MediaItem* item = snap && !media.empty() ? snap->findMedia(media.front()) : nullptr;
    if (!item)
        return {};
    const RawSourceSettings s = rawSourceSettingsOf(item->locator.path);
    const RawWhiteBalance white = s.white.value_or(RawWhiteBalance{});
    const RawIso iso = s.iso.value_or(RawIso{});
    return {{QStringLiteral("name"), QString::fromStdString(item->name)},
            {QStringLiteral("count"), static_cast<int>(media.size())},
            {QStringLiteral("exposure"), s.exposure},
            {QStringLiteral("whiteBalance"), s.white ? QStringLiteral("custom") : QStringLiteral("asShot")},
            {QStringLiteral("temperature"), white.temperature},
            {QStringLiteral("tint"), white.tint},
            // 0.7 (card #108): "asShot" (the camera's ISO) or "custom": shot at isoShot, developed for isoDevelop.
            {QStringLiteral("iso"), s.iso ? QStringLiteral("custom") : QStringLiteral("asShot")},
            {QStringLiteral("isoShot"), iso.shot},
            {QStringLiteral("isoDevelop"), iso.develop}};
}

QString EditingBridge::applyRawSourceSettings(const QStringList& keys, const QVariantMap& settings) {
    const std::vector<MediaId> media = rawMediaOf(keys);
    if (media.empty()) {
        setMessage(tr("Source Settings develop camera RAW clips (ProRes RAW, CinemaDNG): select one in the Library"));
        return m_message;
    }
    Json params{{"media", mediaArray(media)}, {"exposure", settings.value(QStringLiteral("exposure")).toDouble()}};
    const bool custom = settings.value(QStringLiteral("whiteBalance")).toString() == QLatin1String("custom");
    params["whiteBalance"] = custom ? "custom" : "asShot";
    if (custom) {
        params["temperature"] = settings.value(QStringLiteral("temperature"), 5600).toInt();
        params["tint"] = settings.value(QStringLiteral("tint")).toInt();
    }
    // 0.7 (card #108): ISO. "asShot" puts the camera's back (equal ISOs); "custom" is shot at / developed for.
    const bool isoCustom = settings.value(QStringLiteral("iso")).toString() == QLatin1String("custom");
    const int shot = settings.value(QStringLiteral("isoShot"), 800).toInt();
    params["isoShot"] = shot;
    params["isoDevelop"] = isoCustom ? settings.value(QStringLiteral("isoDevelop"), shot).toInt() : shot;
    return result(run("media.setRawSourceSettings", std::move(params)));
}

void EditingBridge::openRawSourceSettings(const QStringList& keys) {
    const QStringList chosen = keys.isEmpty() && m_library ? m_library->selection() : keys;
    if (rawMediaOf(chosen).empty()) {
        setMessage(tr("Source Settings develop camera RAW clips (ProRes RAW, CinemaDNG): select one in the Library"));
        return;
    }
    emit rawSourceSettingsRequested(chosen);
}

void EditingBridge::openTimecode(const QStringList& keys) {
    const QStringList chosen = keys.isEmpty() && m_library ? m_library->selection() : keys;
    if (mediaOf(chosen).empty()) {
        setMessage(tr("Select clips in the Library"));
        return;
    }
    emit timecodeRequested(chosen);
}

// ---- History --------------------------------------------------------------------------------------------------------------

QVariantList EditingBridge::historySteps() const {
    QVariantList out;
    if (!m_controller)
        return out;
    EditSession& session = m_controller->session();
    const std::vector<std::string> done = session.undoLabels();
    const std::vector<std::string> undone = session.redoLabels();
    out.append(QVariantMap{{QStringLiteral("label"), tr("Open")},
                           {QStringLiteral("state"), done.empty() ? QStringLiteral("current") : QStringLiteral("done")}});
    for (std::size_t i = 0; i < done.size(); ++i) {
        out.append(QVariantMap{{QStringLiteral("label"), QString::fromStdString(done[i])},
                               {QStringLiteral("state"),
                                i + 1 == done.size() ? QStringLiteral("current") : QStringLiteral("done")}});
    }
    for (const std::string& label : undone) {
        out.append(QVariantMap{{QStringLiteral("label"), QString::fromStdString(label)},
                               {QStringLiteral("state"), QStringLiteral("undone")}});
    }
    return out;
}

bool EditingBridge::goToHistoryStep(int index) {
    if (!m_controller || index < 0)
        return false;
    EditSession& session = m_controller->session();
    const int current = static_cast<int>(session.undoCount());
    const int last = current + static_cast<int>(session.redoCount());
    if (index > last)
        return false;
    for (int i = current; i > index; --i) {
        if (!m_controller->undo())
            return false;
    }
    for (int i = current; i < index; ++i) {
        if (!m_controller->redo())
            return false;
    }
    emit historyChanged();
    return true;
}

bool EditingBridge::deleteHistoryStep(int index) {
    if (!m_controller || index < 1)
        return false;
    EditSession& session = m_controller->session();
    if (session.inGesture() || index > static_cast<int>(session.undoCount() + session.redoCount()))
        return false;
    if (!goToHistoryStep(index - 1) || !session.discardRedo())
        return false;
    emit m_controller->historyChanged(); // Undo / Redo, their menu rows and historySteps (connected) follow
    return true;
}

bool EditingBridge::clearHistory() {
    if (!m_controller || (!m_controller->canUndo() && !m_controller->canRedo()))
        return false;
    if (!m_controller->session().clearHistory())
        return false;
    emit m_controller->historyChanged();
    return true;
}

// ---- Reveal in Project / File Explorer (0.2 UX, stream T) ----------------------------------------------------------

QVariantMap EditingBridge::revealInfo(const QString& key) const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto id = key.isEmpty() && m_controller ? std::optional<ClipId>(m_controller->primaryClip()) : parseClipKey(key);
    const auto loc = seq && id && *id ? seq->findClip(*id) : std::nullopt;
    if (!loc)
        return {};
    const Clip& clip = loc->clip();
    const ProjectSnapshot snap = m_controller->snapshot();
    QVariantMap info{{QStringLiteral("item"), QString()},
                     {QStringLiteral("name"), QString()},
                     {QStringLiteral("inProject"), false},
                     {QStringLiteral("inExplorer"), false},
                     {QStringLiteral("offline"), false}};
    if (clip.sequence.isValid()) {
        // A nested or multi-camera clip: its sequence (Nesting.revealInLibrary did this for nested clips only).
        if (const Sequence* nested = snap->findSequence(clip.sequence)) {
            info[QStringLiteral("item")] = sequenceKey(nested->id);
            info[QStringLiteral("name")] = text(nested->name);
            info[QStringLiteral("inProject")] = true;
        }
        return info;
    }
    const MediaItem* media = snap->findMedia(clip.media);
    if (!media)
        return info;
    info[QStringLiteral("item")] = mediaKey(media->id);
    info[QStringLiteral("name")] = text(media->name);
    info[QStringLiteral("inProject")] = true;
    if (!media->isGenerator()) {
        const bool found = !media->locator.offline && resolveMediaPath(media->locator, m_controller->projectFile());
        info[QStringLiteral("inExplorer")] = found;
        info[QStringLiteral("offline")] = !found;
    }
    return info;
}

bool EditingBridge::revealInProject(const QString& key) {
    const QVariantMap info = revealInfo(key);
    if (!info.value(QStringLiteral("inProject")).toBool() || !m_library) {
        setMessage(info.isEmpty() ? tr("Select a clip first") : tr("This clip has no item in the project"));
        return false;
    }
    emit libraryRequested();
    if (!m_library->showItem(info.value(QStringLiteral("item")).toString())) {
        setMessage(tr("This clip has no item in the project"));
        return false;
    }
    setMessage({});
    return true;
}

bool EditingBridge::revealInExplorer(const QString& key) {
    const QVariantMap info = revealInfo(key);
    if (!info.value(QStringLiteral("inExplorer")).toBool() || !m_library) {
        setMessage(info.isEmpty()                                       ? tr("Select a clip first")
                   : info.value(QStringLiteral("offline")).toBool() ? tr("“%1” is offline: its file cannot be found.")
                                                                          .arg(info.value(QStringLiteral("name")).toString())
                                                                    : tr("This clip has no file to show"));
        return false;
    }
    if (!m_library->reveal(info.value(QStringLiteral("item")).toString())) {
        setMessage(tr("The file could not be shown"));
        return false;
    }
    setMessage({});
    return true;
}

} // namespace wabcut::app
