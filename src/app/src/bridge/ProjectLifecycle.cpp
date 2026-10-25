#include "ProjectLifecycle.h"

#include "BridgeIds.h"
#include "DemoProject.h"
#include "EditingBridge.h"
#include "MediaBridge.h"
#include "ProjectController.h"

#include <wabcut/editor/CameraCards.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/Production.h>
#include <wabcut/editor/Recovery.h>
#include <wabcut/editor/SourceEdit.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/export/AudioPostHost.h>
#include <wabcut/interchange/CaptionFiles.h>
#include <wabcut/interchange/ImportSummary.h>
#include <wabcut/model/AudioChannelOps.h>
#include <wabcut/model/ColorManagementOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequencePresets.h>
#include <wabcut/model/SequenceSettings.h>
#include <wabcut/model/Timecode.h>
#include <wabcut/model/Vfr.h>
#include <wabcut/platform/FileTime.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QPointer>
#include <QImage>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThreadPool>
#include <QDir>
#include <QLocale>
#include <QSettings>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QStandardPaths>

#include <algorithm>
#include <climits>
#include <chrono>
#include <cmath>
#include <filesystem>

#ifdef Q_OS_WIN
// 0.4 (fix round 1, import folders on network drives): kernel32, declared as the SDK declares it (no <windows.h> in
// this file: its macros).
extern "C" __declspec(dllimport) unsigned long __stdcall QueryDosDeviceW(const wchar_t* lpDeviceName, wchar_t* lpTargetPath,
                                                                        unsigned long ucchMax);
#endif

namespace wabcut::app {

namespace {

std::optional<MediaId> parseMediaKey(const QString& text) {
    return detail::parseId<MediaId>(QLatin1Char('m'), text);
}

std::filesystem::path toPath(const QUrl& url) {
    const QString local = url.isLocalFile() ? url.toLocalFile() : url.toString();
    return std::filesystem::path(local.toStdU16String());
}

QString toQString(const std::filesystem::path& path) {
    return QString::fromStdU16String(path.u16string());
}

QUrl toUrl(const std::filesystem::path& path) {
    return QUrl::fromLocalFile(toQString(path));
}

bool samePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a.empty() || b.empty()) {
        return false;
    }
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec)) {
        return true;
    }
#ifdef _WIN32
    return QString::compare(toQString(a.lexically_normal()), toQString(b.lexically_normal()), Qt::CaseInsensitive) == 0;
#else
    return a.lexically_normal() == b.lexically_normal();
#endif
}

/// Sequence settings for a new project: the first video's size and rate (Premiere's "New Sequence from Clip"),
/// else 1920x1080 at 30 fps.
edit::SequenceSettings settingsFor(const QList<QUrl>& media) {
    edit::SequenceSettings settings;
    for (const QUrl& url : media) {
        auto item = editor::describeMediaFile(toPath(url));
        if (!item || !item->info.hasVideo() || item->info.isStillImage) {
            continue;
        }
        const VideoStreamInfo& v = item->info.video.front();
        if (v.width > 0 && v.height > 0) {
            const bool sideways = v.rotationDegrees == 90 || v.rotationDegrees == 270;
            settings.width = sideways ? v.height : v.width;
            settings.height = sideways ? v.width : v.height;
        }
        if (v.frameRate.num > 0 && v.frameRate.den > 0) {
            // UX rework: a variable frame rate capture gets its nominal rate (the measured rate snapped to a standard
            // one), as Home's media start and the gaming start do; it used to get 30 fps.
            const Rate rate = v.variableFrameRate ? nominalRate(v.frameRate) : v.frameRate;
            if (rate.isExact()) {
                settings.frameRate = rate;
            }
        }
        break;
    }
    return settings;
}

ProjectState emptyProject(const edit::SequenceSettings& settings) {
    auto created = edit::addSequence(ProjectState{}, settings);
    return created ? std::move(created->state) : ProjectState{};
}

// ---- The New project page's settings (UX rework, Home) ----

constexpr int kMinFrameSize = 16;    // sequence.setSettings' bounds
constexpr int kMaxFrameSize = 16384;
constexpr int kMinAudioRate = 8000;
constexpr int kMaxAudioRate = 384000;
constexpr int kMaxTracks = 99;

/// A QML value as plain QVariant data (a JS object can arrive as a QJSValue).
QVariant plainValue(const QVariant& value) {
    if (value.metaType() == QMetaType::fromType<QJSValue>()) {
        return value.value<QJSValue>().toVariant();
    }
    return value;
}

/// `key` as a whole number in [lo, hi] into `out`; absent leaves `out`. False for anything else.
bool readWhole(const QVariantMap& values, const QString& key, int lo, int hi, std::int32_t& out) {
    if (!values.contains(key)) {
        return true;
    }
    bool ok = false;
    const double number = plainValue(values.value(key)).toDouble(&ok);
    if (!ok || !std::isfinite(number) || number != std::floor(number) || number < lo || number > hi) {
        return false;
    }
    out = static_cast<std::int32_t>(number);
    return true;
}

/// 29.97 and 59.94: the rates drop-frame timecode exists for.
bool dropFrameRate(Rate rate) {
    const Rate r = rate.normalized();
    return r.den == 1001 && (r.num == 30000 || r.num == 60000);
}

/// The untitled project New project makes from `values` (ProjectLifecycle::newProjectWith's keys): its one sequence,
/// then the Advanced settings applied to that state (no undo step). The message for the user when a value cannot be used.
std::expected<ProjectState, QString> newProjectState(const QVariantMap& values) {
    edit::SequenceSettings settings;
    if (const QString id = values.value(QStringLiteral("preset")).toString(); !id.isEmpty()) {
        const SequencePreset* preset = findSequencePreset(id.toStdString());
        if (!preset) {
            return std::unexpected(ProjectLifecycle::tr("There is no sequence preset “%1”").arg(id));
        }
        settings.width = preset->width;
        settings.height = preset->height;
        settings.frameRate = preset->frameRate;
        settings.dropFrame = preset->dropFrame;
    }
    if (const QString name = values.value(QStringLiteral("name")).toString().trimmed(); !name.isEmpty()) {
        settings.name = name.toStdString();
    }
    if (!readWhole(values, QStringLiteral("width"), kMinFrameSize, kMaxFrameSize, settings.width) ||
        !readWhole(values, QStringLiteral("height"), kMinFrameSize, kMaxFrameSize, settings.height)) {
        return std::unexpected(ProjectLifecycle::tr("The frame size must be %1 to %2 pixels each way")
                                   .arg(kMinFrameSize)
                                   .arg(kMaxFrameSize));
    }
    if (values.contains(QStringLiteral("rateNum")) || values.contains(QStringLiteral("rateDen"))) {
        settings.frameRate = Rate{values.value(QStringLiteral("rateNum"), settings.frameRate.num).toInt(),
                                  values.value(QStringLiteral("rateDen"), settings.frameRate.den).toInt()};
        if (settings.frameRate.isValid() && settings.frameRate.toDouble() > 1000.0) {
            return std::unexpected(ProjectLifecycle::tr("The frame rate must be at most 1000 fps"));
        }
    }
    std::int32_t audioRate = settings.audioRate.num / std::max(1, settings.audioRate.den);
    if (!readWhole(values, QStringLiteral("audioRate"), kMinAudioRate, kMaxAudioRate, audioRate)) {
        return std::unexpected(ProjectLifecycle::tr("The audio sample rate must be %1 to %2 Hz").arg(kMinAudioRate).arg(kMaxAudioRate));
    }
    settings.audioRate = Rate{audioRate, 1};
    if (values.contains(QStringLiteral("dropFrame"))) {
        settings.dropFrame = values.value(QStringLiteral("dropFrame")).toBool();
    }
    if (!dropFrameRate(settings.frameRate)) {
        settings.dropFrame = false; // only 29.97 and 59.94 have drop-frame timecode
    }
    if (!readWhole(values, QStringLiteral("videoTracks"), 1, kMaxTracks, settings.videoTracks)) {
        return std::unexpected(ProjectLifecycle::tr("A new sequence has 1 to %1 video tracks").arg(kMaxTracks));
    }
    if (!readWhole(values, QStringLiteral("audioTracks"), 1, kMaxTracks, settings.audioTracks)) {
        return std::unexpected(ProjectLifecycle::tr("A new sequence has 1 to %1 audio tracks").arg(kMaxTracks));
    }
    // The same checks as edit::addSequence (an exact rate, a frame), before anything changes.
    auto created = edit::addSequence(ProjectState{}, settings);
    if (!created) {
        return std::unexpected(
            ProjectLifecycle::tr("Cannot create the sequence: %1").arg(QString::fromStdString(created.error().message)));
    }
    ProjectState state = std::move(created->state);
    const SequenceId sequence = created->id;

    // Sequence Settings fields that addSequence does not take.
    edit::SequenceSettingsChange change;
    bool changed = false;
    if (values.contains(QStringLiteral("pixelAspect"))) {
        const QVariant value = plainValue(values.value(QStringLiteral("pixelAspect")));
        std::optional<Rate> aspect;
        if (value.typeId() == QMetaType::QVariantMap) {
            const QVariantMap map = value.toMap();
            aspect = Rate::make(map.value(QStringLiteral("num")).toInt(), map.value(QStringLiteral("den")).toInt());
        } else {
            bool ok = false;
            const double ratio = value.toDouble(&ok);
            if (ok && std::isfinite(ratio) && ratio > 0.0) {
                // A ratio such as 0.9091: thousandths are exact enough for every PAR Premiere lists (as sequence.setSettings).
                aspect = Rate::make(std::llround(ratio * 10000.0), 10000);
            }
        }
        if (!aspect || aspect->toDouble() < 0.1 || aspect->toDouble() > 10.0) {
            return std::unexpected(ProjectLifecycle::tr("The pixel aspect ratio must be 0.1 to 10"));
        }
        change.pixelAspect = *aspect;
        changed = true;
    }
    if (values.contains(QStringLiteral("fields"))) {
        const auto fields = fieldOrderFromString(values.value(QStringLiteral("fields")).toString().toStdString());
        if (!fields) {
            return std::unexpected(ProjectLifecycle::tr("Fields must be progressive, upperFirst or lowerFirst"));
        }
        change.fields = *fields;
        changed = true;
    }
    if (const QString start = values.value(QStringLiteral("startTimecode")).toString().trimmed(); !start.isEmpty()) {
        const auto ticks = parseTimecode(start.toStdString(), settings.frameRate, settings.dropFrame);
        if (!ticks) {
            return std::unexpected(ProjectLifecycle::tr("“%1” is not a start timecode").arg(start));
        }
        change.startTimecode = *ticks;
        changed = true;
    }
    if (values.contains(QStringLiteral("previewFormat"))) {
        const QString format = values.value(QStringLiteral("previewFormat")).toString();
        if (format == QLatin1String("prores") || format == QLatin1String("dnxhr") || format == QLatin1String("h264")) {
            change.previewFormat = format.toStdString();
            changed = true;
        } else if (!format.isEmpty() && format != QLatin1String("auto")) {
            return std::unexpected(ProjectLifecycle::tr("The preview file format must be Auto, ProRes, DNxHR or H.264"));
        }
    }
    if (changed) {
        auto edited = edit::setSequenceSettings(state, sequence, change);
        if (!edited) {
            return std::unexpected(ProjectLifecycle::tr("Cannot create the sequence: %1")
                                       .arg(QString::fromStdString(edited.error().message)));
        }
        state = std::move(edited->state);
    }

    // The audio tracks' channel format and the master's.
    if (values.contains(QStringLiteral("trackFormat"))) {
        const auto format = channelFormatFromString(values.value(QStringLiteral("trackFormat")).toString().toStdString());
        if (!format) {
            return std::unexpected(ProjectLifecycle::tr("The audio track format must be stereo, mono, 5.1 or adaptive"));
        }
        if (*format != ChannelFormat::Stereo) {
            std::vector<TrackId> tracks;
            for (const auto& track : state.findSequence(sequence)->audioTracks) {
                tracks.push_back(track->id);
            }
            edit::TrackStateChange trackChange;
            trackChange.format = *format;
            for (const TrackId track : tracks) {
                auto edited = edit::setTrackState(state, sequence, track, trackChange);
                if (!edited) {
                    return std::unexpected(ProjectLifecycle::tr("Cannot create the sequence: %1")
                                               .arg(QString::fromStdString(edited.error().message)));
                }
                state = std::move(edited->state);
            }
        }
    }
    if (values.contains(QStringLiteral("masterFormat"))) {
        const auto format = channelFormatFromString(values.value(QStringLiteral("masterFormat")).toString().toStdString());
        if (!format || (*format != ChannelFormat::Stereo && *format != ChannelFormat::Surround51)) {
            return std::unexpected(ProjectLifecycle::tr("The audio master must be stereo or 5.1"));
        }
        if (*format != ChannelFormat::Stereo) {
            auto edited = edit::setMasterFormat(state, sequence, *format);
            if (!edited) {
                return std::unexpected(ProjectLifecycle::tr("Cannot create the sequence: %1")
                                           .arg(QString::fromStdString(edited.error().message)));
            }
            state = std::move(edited->state);
        }
    }

    // The project's colour management (Color.setColorManagement's keys).
    if (values.contains(QStringLiteral("colorManagement"))) {
        const QVariant value = plainValue(values.value(QStringLiteral("colorManagement")));
        const QVariantMap map = value.typeId() == QMetaType::QVariantMap
                                    ? value.toMap()
                                    : QVariantMap{{QStringLiteral("mode"), value.toString()}};
        ColorManagement management;
        const QString modeId = map.value(QStringLiteral("mode"), QString::fromLatin1(toString(management.mode))).toString();
        const auto mode = colorManagementModeFromString(modeId.toStdString());
        if (!mode) {
            return std::unexpected(ProjectLifecycle::tr("There is no colour management “%1”").arg(modeId));
        }
        management.mode = *mode;
        if (map.contains(QStringLiteral("peakNits"))) {
            management.peakNits = map.value(QStringLiteral("peakNits")).toDouble();
        }
        if (map.contains(QStringLiteral("referenceWhiteNits"))) {
            management.referenceWhiteNits = map.value(QStringLiteral("referenceWhiteNits")).toDouble();
        }
        if (map.contains(QStringLiteral("autoDetectLog"))) {
            management.autoDetectLog = map.value(QStringLiteral("autoDetectLog")).toBool();
        }
        auto managed = edit::setColorManagement(state, management);
        if (!managed) {
            return std::unexpected(ProjectLifecycle::tr("Cannot set the colour management: %1")
                                       .arg(QString::fromStdString(managed.error().message)));
        }
        state = std::move(*managed);
    }
    return state;
}

QVariantMap reportToMap(const interchange::ImportResult& result, const std::filesystem::path& file) {
    using interchange::ReportOutcome;
    QVariantList entries;
    for (ReportOutcome outcome : {ReportOutcome::Dropped, ReportOutcome::Approximated}) {
        for (const interchange::ReportEntry* e : result.report.filter(outcome)) {
            QString location = QString::fromStdString(e->location.element);
            if (e->location.line > 0) {
                location = QStringLiteral("line %1 · %2").arg(e->location.line).arg(location);
            }
            entries.append(QVariantMap{
                {QStringLiteral("outcome"), QString::fromUtf8(interchange::toString(outcome).data())},
                {QStringLiteral("category"), QString::fromUtf8(interchange::toString(e->category).data())},
                {QStringLiteral("reason"), QString::fromStdString(e->reason)},
                {QStringLiteral("sequence"), QString::fromStdString(e->sequence)},
                {QStringLiteral("location"), location},
            });
        }
    }
    QVariantList unresolved;
    for (const interchange::UnresolvedMedia& m : result.unresolvedMedia) {
        unresolved.append(QVariantMap{{QStringLiteral("name"), QString::fromStdString(m.name)},
                                      {QStringLiteral("path"), QString::fromStdString(m.path)}});
    }
    interchange::SummaryOptions summary;
    summary.listClips = false;
    return {
        {QStringLiteral("file"), toQString(file.filename())},
        {QStringLiteral("format"), QString::fromUtf8(interchange::displayName(result.format).data())},
        {QStringLiteral("sequences"), static_cast<int>(result.sequences.size())},
        {QStringLiteral("media"), static_cast<int>(result.media.size())},
        {QStringLiteral("unresolved"), unresolved},
        {QStringLiteral("carried"), static_cast<int>(result.report.count(ReportOutcome::Carried))},
        {QStringLiteral("approximated"), static_cast<int>(result.report.count(ReportOutcome::Approximated))},
        {QStringLiteral("dropped"), static_cast<int>(result.report.count(ReportOutcome::Dropped))},
        {QStringLiteral("entries"), entries},
        {QStringLiteral("summary"), QString::fromStdString(interchange::formatSummary(result, summary))},
    };
}

} // namespace

ProjectLifecycle::ProjectLifecycle(ProjectController* controller, QObject* parent)
    : QObject(parent), m_controller(controller) {
    Q_ASSERT(controller);
    editor::ProjectDocumentOptions options;
    options.untitledAutosaveDir =
        toPath(QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                                   QStringLiteral("/Autosave")));
    m_untitledAutosaveDir = options.untitledAutosaveDir;
    m_documentOptions = options;
    m_document = std::make_unique<editor::ProjectDocument>(controller->session(), options);
    scanRecovery();
    m_autosaveText = tr("Not autosaved yet");
    m_autosaveOk = true;

    connect(controller, &ProjectController::revisionChanged, this, &ProjectLifecycle::onRevisionChanged);
    // 0.4: importFolder falls back to `folder`, which follows the document.
    connect(this, &ProjectLifecycle::documentChanged, this, &ProjectLifecycle::importFolderChanged);
    connect(controller, &ProjectController::sessionClosing, this, [this] {
        m_autosaveTimer.stop();
        cleanExit();
        m_document.reset();
        m_open.clear();
        m_open.emplace_back();
        m_active = 0;
        m_controller = nullptr;
    }, Qt::DirectConnection);

    m_autosaveTimer.setInterval(m_autosaveSeconds * 1000);
    connect(&m_autosaveTimer, &QTimer::timeout, this, [this] {
        if (!m_demo) {
            startBackgroundAutosave(); // the file work on a worker (P.1)
        }
    });
    m_autosaveTimer.start();
}

ProjectLifecycle::~ProjectLifecycle() {
    {
        std::lock_guard lock(m_scanGate->mutex);
        m_scanGate->owner = nullptr;
    }
    cleanExit();
}

void ProjectLifecycle::cleanExit() {
    waitForAutosave(); // an autosave still being written must not land after its files are removed
    // What is on disk as the project is now the truth (saved, or changes the user chose not to keep).
    if (m_document && !m_demo && !m_readOnly) {
        m_document->removeAutosaves();
    }
    if (!m_locked.empty()) {
        editor::releaseProjectLock(m_locked);
        m_locked.clear();
    }
    // The other open projects too (0.2, P.3): the quit question went through each one.
    for (std::size_t i = 0; i < m_open.size(); ++i) {
        if (i != m_active) {
            releaseSlot(m_open[i]);
        }
    }
}

// ---- Several projects open (0.2, P.3) ---------------------------------------------------------------------------------
//
// One EditSession and one ProjectDocument serve every open project (playback, the panels and the proxy manager hold
// them): the active project is in them, each other one is parked in its slot - its state and undo history
// (EditSession::Parked), its file and saved revisions (ProjectDocument::Identity), lock and view.

bool ProjectLifecycle::slotDirty(const OpenProject& slot) {
    return !slot.demo && editor::ProjectDocument::isDirty(slot.identity, slot.parked.snapshot());
}

void ProjectLifecycle::releaseSlot(OpenProject& slot) {
    if (m_document && !slot.demo && !slot.readOnly) {
        m_document->removeAutosavesOf(slot.identity);
    }
    if (!slot.locked.empty()) {
        editor::releaseProjectLock(slot.locked);
        slot.locked.clear();
    }
}

QVariantList ProjectLifecycle::openProjects() const {
    QVariantList out;
    if (!m_document) {
        return out;
    }
    for (std::size_t i = 0; i < m_open.size(); ++i) {
        const bool active = i == m_active;
        const OpenProject& slot = m_open[i];
        const std::filesystem::path& file = active ? m_document->file() : slot.identity.file;
        const bool demo = active ? m_demo : slot.demo;
        out.append(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(i)},
            {QStringLiteral("name"), demo ? tr("Demo project") : file.empty() ? tr("Untitled") : toQString(file.stem())},
            {QStringLiteral("path"), file.empty() ? QString() : QDir::toNativeSeparators(toQString(file))},
            {QStringLiteral("url"), file.empty() ? QUrl() : toUrl(file)},
            {QStringLiteral("dirty"), active ? dirty() : slotDirty(slot)},
            {QStringLiteral("active"), active},
            {QStringLiteral("readOnly"), active ? m_readOnly : slot.readOnly},
            {QStringLiteral("demo"), demo},
        });
    }
    return out;
}

int ProjectLifecycle::indexOfOpen(const std::filesystem::path& file) const {
    for (std::size_t i = 0; m_document && i < m_open.size(); ++i) {
        if (samePath(i == m_active ? m_document->file() : m_open[i].identity.file, file)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int ProjectLifecycle::indexOfProject(const QUrl& file) const { return indexOfOpen(toPath(file)); }

ProjectSnapshot ProjectLifecycle::openProjectState(const std::filesystem::path& file) const {
    const int at = indexOfOpen(file);
    if (at < 0 || !m_controller) {
        return nullptr;
    }
    if (static_cast<std::size_t>(at) == m_active) {
        return m_demo ? nullptr : m_controller->snapshot();
    }
    return m_open[static_cast<std::size_t>(at)].parked.snapshot();
}

int ProjectLifecycle::firstDirtyProject() const {
    if (dirty()) {
        return static_cast<int>(m_active);
    }
    for (std::size_t i = 0; i < m_open.size(); ++i) {
        if (i != m_active && slotDirty(m_open[i])) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void ProjectLifecycle::stashActive() {
    OpenProject& slot = m_open[m_active];
    slot.demo = m_demo;
    slot.readOnly = m_readOnly;
    slot.lockOwner = m_lockOwner;
    slot.autosaveText = m_autosaveText;
    slot.autosaveOk = m_autosaveOk;
    slot.locked = std::exchange(m_locked, {});
    slot.sequenceIndex = m_controller ? m_controller->currentSequenceIndex() : 0;
    slot.playhead = m_controller ? m_controller->playhead() : Ticks{};
}

void ProjectLifecycle::takeActive(std::size_t index) {
    OpenProject& slot = m_open[index];
    m_demo = slot.demo;
    m_readOnly = slot.readOnly;
    m_lockOwner = slot.lockOwner;
    m_autosaveText = slot.autosaveText;
    m_autosaveOk = slot.autosaveOk;
    m_locked = std::exchange(slot.locked, {});
    m_active = index;
}

void ProjectLifecycle::showActive() {
    const OpenProject& slot = m_open[m_active];
    m_controller->setProjectFile(m_document->file());
    m_controller->setCurrentSequenceIndex(std::max(0, slot.sequenceIndex));
    m_controller->setPlayhead(slot.playhead);
    rememberProject();
    emit autosaveChanged();
    adoptDocument();
}

bool ProjectLifecycle::switchTo(int index) {
    if (!m_document || !m_controller || index < 0 || index >= static_cast<int>(m_open.size())) {
        return false;
    }
    const auto target = static_cast<std::size_t>(index);
    if (target == m_active) {
        return true;
    }
    if (m_controller->session().inGesture()) {
        setError(tr("Finish the edit in progress first"));
        return false;
    }
    waitForAutosave(); // P.1: an autosave being written lands before the project is parked
    if (!m_demo && !m_readOnly && dirty()) {
        autosaveNow(); // the project left keeps a recovery copy of its unsaved changes
    }
    m_controller->setSelection({});
    const std::size_t previous = m_active;
    stashActive();
    // The document first: listeners of the session's Reset see the new project's file with its state.
    m_open[previous].identity = m_document->exchangeIdentity(std::move(m_open[target].identity));
    m_open[previous].parked = m_controller->session().exchange(std::move(m_open[target].parked));
    takeActive(target);
    setError({});
    showActive();
    return true;
}

bool ProjectLifecycle::openAlongside(const QUrl& file) {
    if (!m_document || !m_controller) {
        return false;
    }
    const std::filesystem::path path = toPath(file);
    if (const int at = indexOfOpen(path); at >= 0) {
        return switchTo(at);
    }
    if (m_demo) {
        return open(file); // the demo is not a project: it makes way
    }
    if (m_controller->session().inGesture()) {
        setError(tr("Finish the edit in progress first"));
        return false;
    }
    waitForAutosave();
    if (!m_readOnly && dirty()) {
        autosaveNow();
    }
    m_controller->setSelection({});
    const std::size_t previous = m_active;
    stashActive();
    m_open[previous].identity = m_document->exchangeIdentity({});
    m_open[previous].parked = m_controller->session().park();
    m_open.emplace_back();
    m_active = m_open.size() - 1;
    m_readOnly = false;
    m_lockOwner.clear();
    if (open(file)) {
        return true;
    }
    // It did not open: back to the project left, as it was.
    const QString error = m_lastError;
    m_open.pop_back();
    m_document->exchangeIdentity(std::move(m_open[previous].identity));
    m_controller->session().restore(std::move(m_open[previous].parked));
    takeActive(previous);
    showActive();
    setError(error);
    return false;
}

bool ProjectLifecycle::closeProject(int index) {
    if (!m_document || !m_controller || index < 0 || index >= static_cast<int>(m_open.size()) || m_open.size() < 2) {
        return false;
    }
    const auto at = static_cast<std::size_t>(index);
    if (at != m_active) {
        releaseSlot(m_open[at]);
        m_open.erase(m_open.begin() + index);
        if (m_active > at) {
            --m_active;
        }
        emit documentChanged();
        return true;
    }
    if (m_controller->session().inGesture()) {
        setError(tr("Finish the edit in progress first"));
        return false;
    }
    waitForAutosave();
    // What is on disk is the truth now: the user saved, or chose not to keep the changes.
    if (!m_demo && !m_readOnly) {
        m_document->removeAutosaves();
    }
    if (!m_locked.empty()) {
        editor::releaseProjectLock(m_locked);
        m_locked.clear();
    }
    m_controller->setSelection({});
    const std::size_t next = at + 1 < m_open.size() ? at + 1 : at - 1;
    m_document->exchangeIdentity(std::move(m_open[next].identity));
    m_controller->session().restore(std::move(m_open[next].parked));
    takeActive(next);
    m_open.erase(m_open.begin() + index);
    if (m_active > at) {
        --m_active;
    }
    setError({});
    showActive();
    return true;
}

void ProjectLifecycle::applyScratchDisks() {
    if (!m_controller) {
        return;
    }
    QString wanted;
    QString key = m_document && !m_demo ? toQString(m_document->file()) : QString();
    if (m_document && !m_document->isUntitled() && !m_demo) {
        if (const auto production = editor::productionOf(m_document->file())) {
            if (const std::filesystem::path cache = production->resolve(production->settings().mediaCache); !cache.empty()) {
                wanted = toQString(cache);
            }
            key += QLatin1Char('\n') + QString::fromStdString(production->settings().previews) + QLatin1Char('\n') + wanted;
        }
    }
    if (!wanted.isEmpty()) {
        if (!m_scratchCache) {
            m_appMediaCache = m_controller->mediaCacheDirectory();
            m_scratchCache = true;
        }
        if (m_controller->mediaCacheDirectory() != wanted) {
            m_controller->setMediaCacheDirectory(wanted);
        }
    } else if (m_scratchCache) {
        m_scratchCache = false;
        m_controller->setMediaCacheDirectory(m_appMediaCache);
    }
    if (key != m_scratchKey) {
        m_scratchKey = key;
        emit scratchDisksChanged();
    }
}

// ---- Project locking (0.2, P.3) --------------------------------------------------------------------------------------

bool ProjectLifecycle::lockProjects() const { return QSettings().value(QStringLiteral("project/lockProjects"), false).toBool(); }

void ProjectLifecycle::setLockProjects(bool on) {
    if (on == lockProjects()) {
        return;
    }
    QSettings().setValue(QStringLiteral("project/lockProjects"), on);
    emit lockProjectsChanged();
    if (m_document && !m_document->isUntitled()) {
        updateLock(m_document->file());
        emit documentChanged();
    }
}

QString ProjectLifecycle::productionFolder() const {
    if (!m_document || m_document->isUntitled()) {
        return {};
    }
    const auto folder = editor::Production::folderOf(m_document->file());
    return folder ? QDir::toNativeSeparators(toQString(*folder)) : QString();
}

void ProjectLifecycle::updateLock(const std::filesystem::path& project) {
    if (!m_locked.empty() && m_locked != project) {
        editor::releaseProjectLock(m_locked);
        m_locked.clear();
    }
    m_readOnly = false;
    m_lockOwner.clear();
    if (project.empty() || (!lockProjects() && !editor::Production::folderOf(project))) {
        return;
    }
    auto held = editor::acquireProjectLock(project);
    if (!held) {
        return; // a folder that cannot take the lock file: the project stays editable, unlocked
    }
    if (*held) {
        m_readOnly = true;
        m_lockOwner = QString::fromStdString((*held)->owner());
        return;
    }
    m_locked = project;
}

void ProjectLifecycle::rememberProject() {
    if (m_document && !m_document->isUntitled()) {
        QSettings().setValue(QStringLiteral("project/lastFile"), toQString(m_document->file()));
    }
}

namespace {

QString whenText(std::filesystem::file_time_type t) {
    const auto when = QDateTime::fromStdTimePoint(
        std::chrono::time_point_cast<std::chrono::milliseconds>(platform::toSystemClock(t)));
    return QLocale().toString(when, QLocale::ShortFormat);
}

QString durationText(Ticks t) {
    const auto s = static_cast<qint64>(t.toSeconds() + 0.5);
    return QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

/// A recovery version as QML shows it; `current` is the version saved now (for the comparison), if any.
QVariantMap versionMap(const editor::RecoveryEntry& e, const editor::ProjectSummary* current) {
    using Kind = editor::RecoveryEntry::Kind;
    const editor::ProjectSummary& s = e.summary;
    const QString kind = e.kind == Kind::Current    ? QStringLiteral("current")
                         : e.kind == Kind::Autosave ? QStringLiteral("autosave")
                         : e.kind == Kind::Backup   ? QStringLiteral("backup")
                                                    : QStringLiteral("untitled");
    const QString title = e.kind == Kind::Current    ? QObject::tr("Saved version")
                          : e.kind == Kind::Autosave ? (e.index == 1 ? QObject::tr("Autosave") : QObject::tr("Autosave %1").arg(e.index))
                          : e.kind == Kind::Backup   ? QObject::tr("Backup %1").arg(e.index)
                                                     : QObject::tr("Untitled project (autosave)");
    QVariantMap m{
        {QStringLiteral("url"), QUrl::fromLocalFile(QString::fromStdU16String(e.file.u16string()))},
        {QStringLiteral("file"), QString::fromStdU16String(e.file.filename().u16string())},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("title"), title},
        {QStringLiteral("when"), whenText(s.modified)},
        {QStringLiteral("loadable"), s.loadable},
        {QStringLiteral("error"), QString::fromStdString(s.error)},
        {QStringLiteral("sequences"), s.sequences},
        {QStringLiteral("clips"), s.clips},
        {QStringLiteral("media"), s.media},
        {QStringLiteral("cues"), s.captionCues},
        {QStringLiteral("duration"), durationText(s.duration)},
        {QStringLiteral("sequence"), QString::fromStdString(s.mainSequence)},
        {QStringLiteral("newer"), e.newerThanProject},
        {QStringLiteral("poster"), QString()},
    };
    if (current && e.kind != Kind::Current && e.kind != Kind::UntitledAutosave && s.loadable) {
        QStringList diff;
        const auto delta = [&](int a, int b, const QString& what) {
            if (a != b) {
                diff << QStringLiteral("%1%2 %3").arg(a > b ? QStringLiteral("+") : QString()).arg(a - b).arg(what);
            }
        };
        delta(s.sequences, current->sequences, QObject::tr("sequences"));
        delta(s.clips, current->clips, QObject::tr("clips"));
        delta(s.media, current->media, QObject::tr("media"));
        if (s.duration != current->duration) {
            const double d = (s.duration - current->duration).toSeconds();
            diff << QObject::tr("%1%2 s long").arg(d > 0 ? QStringLiteral("+") : QString()).arg(d, 0, 'f', 1);
        }
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(s.modified - current->modified).count();
        const auto magnitude = static_cast<int>(std::min<long long>(std::llabs(seconds), INT_MAX));
        const QString age = magnitude < 60 ? QObject::tr("%n s", "", magnitude) : QObject::tr("%n min", "", magnitude / 60);
        diff << (seconds >= 0 ? QObject::tr("%1 newer than the saved version").arg(age)
                              : QObject::tr("%1 older than the saved version").arg(age));
        m.insert(QStringLiteral("comparison"), diff.isEmpty() ? QObject::tr("Same as the saved version") : diff.join(QStringLiteral(" · ")));
    } else {
        m.insert(QStringLiteral("comparison"), QString());
    }
    return m;
}

} // namespace

void ProjectLifecycle::scanRecoveryVersions() {
    std::filesystem::path project = m_document ? m_document->file() : std::filesystem::path{};
    if (project.empty()) {
        const QString last = QSettings().value(QStringLiteral("project/lastFile")).toString();
        if (!last.isEmpty()) {
            project = toPath(QUrl::fromLocalFile(last));
        }
    }
    const std::filesystem::path untitledDir = m_untitledAutosaveDir;
    const std::filesystem::path posters =
        toPath(QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))) / "recovery-posters";
    const std::uint64_t scan = ++m_versionsScan;
    m_versionsScanning = true;
    emit recoveryVersionsChanged();
    {
        std::lock_guard lock(m_scanGate->mutex);
        m_scanGate->owner = this;
    }
    const auto post = [gate = m_scanGate, scan](QVariantList list, bool finished) {
        std::lock_guard lock(gate->mutex);
        if (ProjectLifecycle* owner = gate->owner) {
            QMetaObject::invokeMethod(
                owner,
                [owner, scan, list = std::move(list), finished] {
                    if (scan != owner->m_versionsScan) {
                        return; // a newer scan runs
                    }
                    owner->m_versions = list;
                    owner->m_versionsScanning = !finished;
                    emit owner->recoveryVersionsChanged();
                },
                Qt::QueuedConnection);
        }
    };
    QThreadPool::globalInstance()->start([project, untitledDir, posters, post] {
        const std::vector<editor::RecoveryEntry> entries = editor::recoveryEntries(project, untitledDir);
        const editor::ProjectSummary* current = nullptr;
        for (const editor::RecoveryEntry& e : entries) {
            if (e.kind == editor::RecoveryEntry::Kind::Current) {
                current = &e.summary;
            }
        }
        QVariantList list;
        for (const editor::RecoveryEntry& e : entries) {
            list.append(versionMap(e, current));
        }
        post(list, false);
        // Posters: the picture a third of the way into each version's first sequence (export renderer, offscreen).
        std::error_code ec;
        std::filesystem::create_directories(posters, ec);
        for (qsizetype i = 0; i < list.size(); ++i) {
            const editor::RecoveryEntry& e = entries[static_cast<std::size_t>(i)];
            if (!e.summary.loadable) {
                continue;
            }
            auto state = io::loadProject(e.file);
            if (!state) {
                continue;
            }
            auto poster = editor::renderPoster(*state, e.project, 320);
            if (!poster) {
                continue;
            }
            const QImage image(poster->rgba.data(), poster->width, poster->height, poster->width * 4,
                               QImage::Format_RGBA8888);
            const std::filesystem::path file =
                posters / (std::to_string(std::hash<std::string>{}(pathToUtf8(e.file))) + "-" +
                           // long long: libc++'s file clock counts in an __int128, which to_string does not take.
                           std::to_string(static_cast<long long>(e.summary.modified.time_since_epoch().count())) +
                           ".png");
            const QString path = QString::fromStdU16String(file.u16string());
            if (image.save(path)) {
                QVariantMap m = list[i].toMap();
                m.insert(QStringLiteral("poster"), QUrl::fromLocalFile(path));
                list[i] = m;
                post(list, false);
            }
        }
        post(list, true);
    });
}

bool ProjectLifecycle::openRecoveryCopy(const QUrl& file) {
    if (!m_document) {
        return false;
    }
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    m_controller->setSelection({});
    // Untitled: never overwrites the project. Media resolve relative to the project the copy belongs to (P2-11).
    if (auto opened = m_document->recover(toPath(file), {}); !opened) {
        setError(tr("Cannot open %1: %2").arg(file.fileName(), QString::fromStdString(opened.error().message)));
        return false;
    }
    setError({});
    updateLock({});
    m_controller->setProjectFile(m_document->file());
    m_controller->setCurrentSequenceIndex(0);
    m_controller->setPlayhead({});
    m_demo = false;
    m_autosaveText = tr("Copy of %1").arg(file.fileName());
    m_autosaveOk = true;
    emit autosaveChanged();
    adoptDocument();
    if (const int offline = offlineCount(); offline > 0) {
        emit offlineMediaFound(offline);
    }
    return true;
}

void ProjectLifecycle::scanRecovery() {
    // What to scan is read here (settings, the document's options); the files are read on a worker (P.1).
    const QString last = m_document ? QSettings().value(QStringLiteral("project/lastFile")).toString() : QString();
    const std::filesystem::path projectFile = last.isEmpty() ? std::filesystem::path{} : toPath(QUrl::fromLocalFile(last));
    const std::filesystem::path untitledDir = m_untitledAutosaveDir;
    const int copies = m_document ? m_document->autosaveCopies() : 1;
    const std::uint64_t scan = ++m_recoveryScanSerial;
    {
        std::lock_guard lock(m_scanGate->mutex);
        m_scanGate->owner = this;
    }
    m_recoveryScan = std::async(std::launch::async, [gate = m_scanGate, scan, last, projectFile, untitledDir, copies] {
        RecoveryScan out;
        out.lastProject = last;
        out.untitled = editor::ProjectDocument::untitledRecoveryFiles(untitledDir);
        if (!projectFile.empty()) {
            out.last = editor::ProjectDocument::newestUnsavedAutosave(projectFile, copies); // the newest one, as before
        }
        // Announce it, unless something collected it first or a newer scan replaced it.
        std::lock_guard lock(gate->mutex);
        if (ProjectLifecycle* owner = gate->owner) {
            QMetaObject::invokeMethod(
                owner,
                [owner, scan] {
                    if (scan == owner->m_recoveryScanSerial) {
                        owner->collectRecovery(false);
                    }
                },
                Qt::QueuedConnection);
        }
        return out;
    });
}

void ProjectLifecycle::collectRecovery(bool queuedNotify) const {
    if (!m_recoveryScan.valid()) {
        return;
    }
    const RecoveryScan scan = m_recoveryScan.get(); // waits only while the worker still runs
    QVariantList found;
    const auto describe = [](const editor::RecoveryFile& r, const QString& project) {
        const auto when = QDateTime::fromStdTimePoint(
            std::chrono::time_point_cast<std::chrono::milliseconds>(platform::toSystemClock(r.modified)));
        return QVariantMap{
            {QStringLiteral("url"), toUrl(r.file)},
            {QStringLiteral("name"), project.isEmpty() ? tr("Untitled project") : QFileInfo(project).completeBaseName()},
            {QStringLiteral("when"), QLocale().toString(when, QLocale::ShortFormat)},
            {QStringLiteral("project"), project.isEmpty() ? QUrl() : QUrl::fromLocalFile(project)},
        };
    };
    for (const editor::RecoveryFile& r : scan.untitled) {
        found.append(describe(r, {}));
    }
    if (scan.last) {
        found.append(describe(*scan.last, scan.lastProject));
    }
    if (found == m_recovery) {
        return; // nothing to announce (the scan at construction used to emit before anything listened)
    }
    m_recovery = std::move(found);
    auto* self = const_cast<ProjectLifecycle*>(this);
    if (queuedNotify) { // from the property's getter: never emit inside a binding's evaluation
        QMetaObject::invokeMethod(self, [self] { emit self->recoveryChanged(); }, Qt::QueuedConnection);
    } else {
        emit self->recoveryChanged();
    }
}

bool ProjectLifecycle::recover(const QUrl& file) {
    if (!m_document) {
        return false;
    }
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    collectRecovery(false);
    QVariantMap entry;
    for (const QVariant& v : std::as_const(m_recovery)) {
        if (v.toMap().value(QStringLiteral("url")).toUrl() == file) {
            entry = v.toMap();
        }
    }
    const QUrl project = entry.value(QStringLiteral("project")).toUrl();
    const std::filesystem::path recoveryFile = toPath(file);
    m_controller->setSelection({});
    if (auto recovered = m_document->recover(recoveryFile, project.isEmpty() ? std::filesystem::path{} : toPath(project));
        !recovered) {
        setError(tr("Cannot recover: %1").arg(QString::fromStdString(recovered.error().message)));
        return false;
    }
    setError({});
    updateLock(m_document->isUntitled() ? std::filesystem::path{} : m_document->file());
    m_controller->setProjectFile(m_document->file());
    m_controller->setCurrentSequenceIndex(0);
    m_controller->setPlayhead({});
    m_demo = false;
    if (!project.isEmpty()) {
        rememberProject();
    }
    // The recovered revision is written again as this document's autosave right away. Nothing is deleted here: an
    // untitled source goes only once that write succeeded (the document retires it), and a failed write (disk full,
    // no permission) keeps it for the next start.
    autosaveNow();
    dismissRecovery(file);
    m_autosaveText = tr("Recovered");
    m_autosaveOk = true;
    emit autosaveChanged();
    adoptDocument();
    if (const int offline = offlineCount(); offline > 0) {
        emit offlineMediaFound(offline);
    }
    return true;
}

void ProjectLifecycle::dismissRecovery(const QUrl& file) {
    collectRecovery(false);
    QVariantList kept;
    for (const QVariant& v : std::as_const(m_recovery)) {
        if (v.toMap().value(QStringLiteral("url")).toUrl() != file) {
            kept.append(v);
        }
    }
    m_recovery = std::move(kept);
    emit recoveryChanged();
}

void ProjectLifecycle::discardRecovery(const QUrl& file) {
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    collectRecovery(false);
    QVariantList kept;
    for (const QVariant& v : std::as_const(m_recovery)) {
        if (v.toMap().value(QStringLiteral("url")).toUrl() == file) {
            std::error_code ec;
            std::filesystem::remove(toPath(file), ec);
        } else {
            kept.append(v);
        }
    }
    m_recovery = std::move(kept);
    emit recoveryChanged();
}

void ProjectLifecycle::discardAllRecovery() {
    collectRecovery(false);
    const QVariantList all = m_recovery;
    for (const QVariant& v : all) {
        discardRecovery(v.toMap().value(QStringLiteral("url")).toUrl());
    }
}

ProjectLifecycle* ProjectLifecycle::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* controller = qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    // ProjectController is default-constructible, so QML builds that singleton with its constructor and never
    // calls ProjectController::create(), which is where the library's thumbnail provider was meant to be
    // registered (Qt prefers the constructor over a static create()). Register it here, before any card asks.
    if (!qmlEngine->imageProvider(QLatin1String(ThumbnailImageProvider::kProviderId))) {
        qmlEngine->addImageProvider(QLatin1String(ThumbnailImageProvider::kProviderId),
                                    new ThumbnailImageProvider(controller->mediaCacheDirectory().toStdU16String()));
    }
    return new ProjectLifecycle(controller);
}

// ---- Properties -------------------------------------------------------------------------------------

QString ProjectLifecycle::name() const {
    if (m_demo) {
        return tr("Demo project");
    }
    return m_document ? QString::fromStdString(m_document->displayName()) : QString();
}

QString ProjectLifecycle::windowTitle() const {
    //: Window title: project name, then the app name
    QString title = tr("%1 — Wabcut").arg(name());
    if (m_readOnly) {
        //: Window title of a project another editor has locked: project name, then the app name
        title = tr("%1 (read-only) — Wabcut").arg(name());
    }
    return dirty() ? QStringLiteral("● ") + title : title;
}

bool ProjectLifecycle::dirty() const {
    return m_document && !m_demo && m_document->isDirty();
}

bool ProjectLifecycle::untitled() const {
    return !m_document || m_document->isUntitled();
}

QString ProjectLifecycle::filePath() const {
    return m_document ? QDir::toNativeSeparators(toQString(m_document->file())) : QString();
}

QUrl ProjectLifecycle::folder() const {
    if (m_document && !m_document->isUntitled()) {
        return toUrl(m_document->file().parent_path());
    }
    return QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
}

// ---- 0.4: where import dialogs open ----

namespace {

constexpr int kRecentImportFolders = 5;

/// Two folder paths name the same folder (case-insensitive on Windows).
bool sameFolderText(const QString& a, const QString& b) {
#ifdef Q_OS_WIN
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
#else
    return QDir::cleanPath(a) == QDir::cleanPath(b);
#endif
}

/// Fix round 1: a folder on a network share (\\server\share, or a drive letter mapped to one). Whether it is there is
/// not asked on the GUI thread: a share that does not answer holds the caller for the redirector's timeout (15-60 s),
/// and importFolder is read by four dialogs while the window loads. Told from the path and the drive letter's DOS
/// device alone (no file system access); what is not a local disk, CD or floppy counts as a share.
bool onNetworkShare(const QString& path, int depth = 0) {
    const QString p = QDir::fromNativeSeparators(path);
    if (p.startsWith(QLatin1String("//")))
        return true;
#ifdef Q_OS_WIN
    if (p.size() >= 2 && p.at(1) == QLatin1Char(':') && p.at(0).isLetter()) {
        const wchar_t drive[3] = {static_cast<wchar_t>(p.at(0).toUpper().unicode()), L':', L'\0'};
        wchar_t target[4096] = {};
        if (QueryDosDeviceW(drive, target, 4096) == 0)
            return false; // no such drive: asking about it fails at once
        const QString device = QString::fromWCharArray(target);
        if (device.startsWith(QLatin1String("\\??\\UNC\\"), Qt::CaseInsensitive))
            return true; // SUBST of a share
        if (device.startsWith(QLatin1String("\\??\\")))
            return depth > 4 || onNetworkShare(device.mid(4), depth + 1); // SUBST of a folder: where it points
        return !device.startsWith(QLatin1String("\\Device\\Harddisk"), Qt::CaseInsensitive) &&
               !device.startsWith(QLatin1String("\\Device\\CdRom"), Qt::CaseInsensitive) &&
               !device.startsWith(QLatin1String("\\Device\\Floppy"), Qt::CaseInsensitive);
    }
#else
    (void)depth;
#endif
    return false;
}

/// A remembered folder still to offer: a local one while it is there, one on a share as it is (onNetworkShare; a
/// dialog opened at a share that is gone falls back by itself).
bool usableFolder(const QString& path) {
    return !path.isEmpty() && (onNetworkShare(path) || QFileInfo(path).isDir());
}

} // namespace

QUrl ProjectLifecycle::importFolder() const {
    const QString last = QSettings().value(QStringLiteral("import/lastFolder")).toString();
    if (usableFolder(last))
        return QUrl::fromLocalFile(QDir::fromNativeSeparators(last));
    return folder();
}

void ProjectLifecycle::rememberImportFolder(const QList<QUrl>& chosen) {
    QStringList folders; // each file's folder, each folder itself; in the order chosen
    QString last;        // the first one's folder (a folder: its parent, so a sibling is one click away)
    for (const QUrl& url : chosen) {
        const QString path = url.isLocalFile() ? url.toLocalFile() : QString();
        if (path.isEmpty())
            continue;
        const QFileInfo info(path);
        const QString folderPath = QDir::cleanPath(info.isDir() ? info.absoluteFilePath() : info.absolutePath());
        if (last.isEmpty())
            last = info.isDir() ? QDir::cleanPath(QFileInfo(folderPath).absolutePath()) : folderPath;
        if (std::ranges::none_of(folders, [&](const QString& f) { return sameFolderText(f, folderPath); }))
            folders << folderPath;
    }
    if (last.isEmpty())
        return;
    QSettings settings;
    settings.setValue(QStringLiteral("import/lastFolder"), QDir::toNativeSeparators(last));
    // Newest first: this choice's folders, then the ones remembered before (deduplicated), at most five.
    QStringList recent;
    for (const QString& f : folders)
        recent << QDir::toNativeSeparators(f);
    for (const QString& f : settings.value(QStringLiteral("import/recentFolders")).toStringList()) {
        if (std::ranges::none_of(recent, [&](const QString& r) { return sameFolderText(r, f); }))
            recent << f;
    }
    while (recent.size() > kRecentImportFolders)
        recent.removeLast();
    settings.setValue(QStringLiteral("import/recentFolders"), recent);
    emit importFolderChanged();
}

QStringList ProjectLifecycle::recentImportFolders() {
    QStringList out;
    for (const QString& f : QSettings().value(QStringLiteral("import/recentFolders")).toStringList()) {
        if (usableFolder(f))
            out << QDir::toNativeSeparators(f);
    }
    return out;
}

void ProjectLifecycle::setAutosaveIntervalSeconds(int seconds) {
    seconds = std::max(5, seconds);
    if (seconds == m_autosaveSeconds) {
        return;
    }
    m_autosaveSeconds = seconds;
    m_autosaveTimer.setInterval(seconds * 1000);
    emit autosaveChanged();
}

QVariantList ProjectLifecycle::offlineMedia() const {
    QVariantList list;
    if (!m_document) {
        return list;
    }
    const ProjectSnapshot snap = m_document->session().snapshot();
    for (const MediaPtr& media : snap->media) {
        if (media->locator.offline) {
            list.append(QVariantMap{{QStringLiteral("id"), mediaKey(media->id)},
                                    {QStringLiteral("name"), QString::fromStdString(media->name)},
                                    {QStringLiteral("path"), QString::fromStdString(media->locator.path)}});
        }
    }
    // LUT assets (I.5) are found and relinked with the media; their ids are their references ("asset:<id>").
    for (const editor::AssetStatus& asset : m_document->assetStatus()) {
        if (asset.offline) {
            list.append(QVariantMap{{QStringLiteral("id"), QString::fromStdString(assetReference(asset.asset))},
                                    {QStringLiteral("name"), QString::fromStdString(asset.name)},
                                    {QStringLiteral("path"), QString::fromStdString(asset.path)},
                                    {QStringLiteral("kind"), QString::fromStdString(asset.kind)}});
        }
    }
    return list;
}

int ProjectLifecycle::offlineCount() const {
    if (!m_document || m_demo) {
        return 0;
    }
    const auto assets = m_document->assetStatus();
    return static_cast<int>(m_document->offlineMedia().size()) +
           static_cast<int>(std::ranges::count_if(assets, [](const editor::AssetStatus& a) { return a.offline; }));
}

QStringList ProjectLifecycle::importNameFilters() const {
    return {
        tr("Media, timelines and captions (*.mp4 *.mov *.mkv *.mxf *.avi *.m4v *.webm *.wav *.mp3 *.aac *.m4a *.flac "
           "*.png *.jpg *.jpeg *.tif *.tiff *.xml *.fcpxml *.otio *.edl *.prproj *.srt *.vtt *.scc *.mcc *.stl *.ttml "
           "*.dfxp *.itt)"),
        tr("Timelines (*.xml *.fcpxml *.otio *.edl *.prproj)"),
        // P.2: a caption file becomes a caption track of the current sequence.
        tr("Caption files (*.srt *.vtt *.scc *.mcc *.stl *.ttml *.dfxp *.itt *.xml)"),
        tr("All files (*)"),
    };
}

// ---- State ------------------------------------------------------------------------------------------

void ProjectLifecycle::setError(const QString& message) {
    if (message == m_lastError) {
        return;
    }
    m_lastError = message;
    emit lastErrorChanged();
}

void ProjectLifecycle::adoptDocument() {
    m_lastDirty = dirty();
    m_lastOfflineCount = offlineCount();
    applyScratchDisks(); // 0.2 (P.3): the production's media cache, if it has one
    emit documentChanged();
    emit mediaStatusChanged();
}

void ProjectLifecycle::onRevisionChanged() {
    if (!m_document) {
        return;
    }
    const bool nowDirty = dirty();
    if (nowDirty != m_lastDirty) {
        m_lastDirty = nowDirty;
        emit documentChanged();
    }
    const int offline = offlineCount();
    if (offline != m_lastOfflineCount) {
        m_lastOfflineCount = offline;
        emit mediaStatusChanged();
    }
}

// ---- File -------------------------------------------------------------------------------------------

void ProjectLifecycle::startNewProject(const QList<QUrl>& firstMedia) {
    startNewProjectWith(settingsFor(firstMedia));
}

void ProjectLifecycle::startNewProjectWith(const edit::SequenceSettings& settings) {
    startNewProjectFrom(emptyProject(settings));
}

void ProjectLifecycle::startNewProjectFrom(ProjectState initial) {
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    m_controller->setSelection({});
    m_document->newProject(std::move(initial));
    updateLock({});
    m_controller->setProjectFile({});
    m_controller->setCurrentSequenceIndex(0);
    m_controller->setPlayhead({});
    m_demo = false;
    m_autosaveText = tr("Not autosaved yet");
    m_autosaveOk = true;
    emit autosaveChanged();
    adoptDocument();
}

void ProjectLifecycle::newProject() {
    if (!m_document) {
        return;
    }
    setError({});
    startNewProject({});
}

bool ProjectLifecycle::openDemo() {
    if (!m_document || !m_controller) {
        return false;
    }
    if (m_controller->session().inGesture()) {
        setError(tr("Finish the edit in progress first"));
        return false;
    }
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    // The project shown makes way (QML asked about its unsaved changes first): what is on disk is the truth now, as
    // closeProject does. Other open projects stay open.
    if (!m_demo && !m_readOnly) {
        m_document->removeAutosaves();
    }
    m_controller->setSelection({});
    DemoProject demo = buildDemoProject();
    m_document->newProject(std::move(demo.state)); // untitled, not dirty, no history
    updateLock({});
    m_controller->setProjectFile({});
    int sequenceIndex = 0;
    if (const ProjectSnapshot snapshot = m_controller->snapshot()) {
        const auto& sequences = snapshot->sequences;
        const auto it = std::ranges::find_if(sequences, [&](const SequencePtr& s) { return s->id == demo.mainSequence; });
        if (it != sequences.end()) {
            sequenceIndex = static_cast<int>(it - sequences.begin());
        }
    }
    m_controller->setCurrentSequenceIndex(sequenceIndex);
    // As at launch: City B-roll selected, the playhead at 00:00:12:08.
    if (demo.initialSelection.isValid()) {
        m_controller->setSelection({demo.initialSelection}, demo.initialSelection);
    }
    m_controller->setPlayhead(demo.initialPlayhead);
    m_demo = true;
    m_autosaveText = tr("Not autosaved yet");
    m_autosaveOk = true;
    setError({});
    emit autosaveChanged();
    adoptDocument();
    return true;
}

QUrl ProjectLifecycle::launchProject() const {
    return launchProjectIn(QCoreApplication::arguments());
}

QUrl ProjectLifecycle::launchProjectIn(const QStringList& arguments) {
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString& argument = arguments[i];
        if (argument == QLatin1String("--rhi")) {
            ++i; // its value
            continue;
        }
        if (argument.startsWith(QLatin1Char('-')) || !argument.endsWith(QLatin1String(".wabcut"), Qt::CaseInsensitive)) {
            continue;
        }
        const QFileInfo info(argument);
        if (info.isFile()) {
            return QUrl::fromLocalFile(info.absoluteFilePath());
        }
    }
    return {};
}

bool ProjectLifecycle::open(const QUrl& file) {
    if (!m_document) {
        return false;
    }
    const std::filesystem::path path = toPath(file);
    // 0.2 (P.3): open next to this one already: it becomes the active project, with its unsaved changes. The active
    // project itself (File > Open, an import or a drop of its own file): nothing to do. Reading it again would drop its
    // undo history; File > Revert is the way to read the saved file again.
    if (const int at = indexOfOpen(path); at >= 0) {
        if (static_cast<std::size_t>(at) != m_active) {
            return switchTo(at);
        }
        setError({});
        return true;
    }
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    m_controller->setSelection({});
    if (auto opened = m_document->open(path); !opened) {
        setError(tr("Cannot open %1: %2").arg(toQString(path.filename()), QString::fromStdString(opened.error().message)));
        return false;
    }
    setError({});
    updateLock(m_document->file());
    m_controller->setProjectFile(m_document->file());
    m_controller->setCurrentSequenceIndex(0);
    m_controller->setPlayhead({});
    m_demo = false;
    m_autosaveText = m_readOnly ? tr("Read-only: %1 has it open").arg(m_lockOwner)
                                : tr("Opened %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
    m_autosaveOk = true;
    emit autosaveChanged();
    adoptDocument();
    if (const int offline = offlineCount(); offline > 0) {
        emit offlineMediaFound(offline);
    }
    // A newer autosave of this project means an unclean exit lost work since the last save: offer it.
    const QString project = toQString(m_document->file());
    QSettings().setValue(QStringLiteral("project/lastFile"), project);
    scanRecovery();
    collectRecovery(false); // this project's answer is needed now (no parse at all after a clean exit)
    const bool newer = std::ranges::any_of(m_recovery, [&](const QVariant& v) {
        return v.toMap().value(QStringLiteral("project")).toUrl() == QUrl::fromLocalFile(project);
    });
    if (newer) {
        emit recoveryAvailable();
    }
    return true;
}

bool ProjectLifecycle::save() {
    if (!m_document) {
        return false;
    }
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    if (m_document->isUntitled()) {
        setError(tr("The project has no file yet"));
        return false;
    }
    if (m_readOnly) {
        setError(tr("%1 has this project open, so it is read-only here. Save As to keep your changes in another file.")
                     .arg(m_lockOwner));
        return false;
    }
    if (auto saved = m_document->save(); !saved) {
        setError(tr("Cannot save: %1").arg(QString::fromStdString(saved.error().message)));
        return false;
    }
    setError({});
    m_autosaveText = tr("Saved %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
    m_autosaveOk = true;
    emit autosaveChanged();
    adoptDocument();
    return true;
}

bool ProjectLifecycle::saveAs(const QUrl& file) {
    if (!m_document) {
        return false;
    }
    waitForAutosave(); // P.1: an autosave being written lands before the files change
    std::filesystem::path path = toPath(file);
    if (path.extension() != io::kProjectExtension) {
        path += std::string(io::kProjectExtension);
    }
    // 0.2 (P.3): never over a project open next to this one (two open projects on one file).
    if (const int at = indexOfOpen(path); at >= 0 && static_cast<std::size_t>(at) != m_active) {
        setError(tr("%1 is open next to this project: close it first").arg(toQString(path.filename())));
        return false;
    }
    if (auto saved = m_document->saveAs(path); !saved) {
        setError(tr("Cannot save %1: %2").arg(toQString(path.filename()), QString::fromStdString(saved.error().message)));
        return false;
    }
    setError({});
    updateLock(m_document->file());
    m_controller->setProjectFile(m_document->file());
    m_demo = false;
    rememberProject();
    m_autosaveText = tr("Saved %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
    m_autosaveOk = true;
    emit autosaveChanged();
    adoptDocument();
    return true;
}

// UX rework (stream S): File > Save a Copy... and File > Revert.
bool ProjectLifecycle::saveCopy(const QUrl& file) {
    if (!m_document) {
        return false;
    }
    std::filesystem::path path = toPath(file);
    if (path.empty()) {
        setError(tr("Choose a file for the copy"));
        return false;
    }
    if (path.extension() != io::kProjectExtension) {
        path += std::string(io::kProjectExtension);
    }
    // Not over this project's own file (that is Save) nor over a project open next to it.
    if (indexOfOpen(path) >= 0) {
        setError(tr("%1 is open: choose another file for the copy").arg(toQString(path.filename())));
        return false;
    }
    if (auto written = m_document->saveCopy(path); !written) {
        setError(tr("Cannot save a copy as %1: %2").arg(toQString(path.filename()), QString::fromStdString(written.error().message)));
        return false;
    }
    setError({});
    return true;
}

bool ProjectLifecycle::revert() {
    if (!m_document || !m_controller) {
        return false;
    }
    if (m_demo || m_document->isUntitled()) {
        setError(tr("Only a saved project can be reverted"));
        return false;
    }
    if (m_controller->session().inGesture()) {
        setError(tr("Finish the edit in progress first"));
        return false;
    }
    waitForAutosave(); // an autosave being written lands before its files are removed
    const std::filesystem::path file = m_document->file();
    const int sequence = m_controller->currentSequenceIndex();
    const Ticks playhead = m_controller->playhead();
    m_controller->setSelection({});
    // Read first: a file that cannot be read changes nothing (the unsaved work and its autosaves stay).
    if (auto opened = m_document->open(file); !opened) {
        setError(tr("Cannot revert to %1: %2").arg(toQString(file.filename()), QString::fromStdString(opened.error().message)));
        return false;
    }
    // The user threw the changes away: their autosaves go too (no recovery offer for them), and what is on disk is the
    // autosaved state again. Another session's lock: its autosaves are not this session's to remove.
    if (!m_readOnly) {
        m_document->removeAutosaves();
    }
    m_document->markClean();
    setError({});
    m_controller->setProjectFile(m_document->file());
    if (sequence >= 0 && sequence < static_cast<int>(m_controller->sequenceNames().size())) {
        m_controller->setCurrentSequenceIndex(sequence);
    }
    m_controller->setPlayhead(playhead);
    m_autosaveText = tr("Reverted %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
    m_autosaveOk = true;
    emit autosaveChanged();
    adoptDocument();
    if (const int offline = offlineCount(); offline > 0) {
        emit offlineMediaFound(offline);
    }
    return true;
}

void ProjectLifecycle::startBackgroundAutosave() {
    if (!m_document || m_demo || m_readOnly) {
        return;
    }
    if (m_autosaveWrite.valid()) {
        if (m_autosaveWrite.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return; // the previous one is still writing: the next tick writes the newer revision
        }
        collectAutosave();
    }
    std::optional<editor::ProjectDocument::AutosaveJob> job = m_document->prepareAutosave();
    if (!job) {
        return;
    }
    m_autosaveJob = std::move(job);
    const std::uint64_t serial = ++m_autosaveSerial;
    {
        std::lock_guard lock(m_scanGate->mutex);
        m_scanGate->owner = this;
    }
    m_autosaveWrite = std::async(std::launch::async, [job = &*m_autosaveJob, gate = m_scanGate, serial] {
        std::expected<void, Error> written = job->write();
        std::lock_guard lock(gate->mutex);
        if (ProjectLifecycle* owner = gate->owner) {
            QMetaObject::invokeMethod(
                owner,
                [owner, serial] {
                    if (serial == owner->m_autosaveSerial && owner->m_autosaveWrite.valid()) {
                        owner->collectAutosave();
                    }
                },
                Qt::QueuedConnection);
        }
        return written;
    });
}

void ProjectLifecycle::collectAutosave() {
    if (!m_autosaveWrite.valid()) {
        return;
    }
    const std::expected<void, Error> written = m_autosaveWrite.get();
    std::optional<editor::ProjectDocument::AutosaveJob> job = std::exchange(m_autosaveJob, std::nullopt);
    if (!written) {
        autosaveFailedWith(QString::fromStdString(written.error().message));
        return;
    }
    if (m_document && job) {
        m_document->finishAutosave(*job);
    }
    m_autosaveText = tr("Autosaved %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
    m_autosaveOk = true;
    emit autosaveChanged();
}

void ProjectLifecycle::waitForAutosave() {
    if (m_autosaveWrite.valid()) {
        m_autosaveWrite.wait();
        collectAutosave();
    }
}

bool ProjectLifecycle::autosaveNow() {
    if (!m_document || m_demo || m_readOnly) {
        return false;
    }
    waitForAutosave();
    auto written = m_document->autosave();
    if (!written) {
        autosaveFailedWith(QString::fromStdString(written.error().message));
        return false;
    }
    if (*written) {
        m_autosaveText = tr("Autosaved %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")));
        m_autosaveOk = true;
        emit autosaveChanged();
    }
    return *written;
}

// 0.4: an autosave could not be written (drive gone, disk full, no permission, a folder in the way). The revision stays
// pending, so every tick tries again; every failure is signalled (QML toasts once per failure streak: until autosaveOk
// turns true again with an autosave, save, open, revert or recovery).
void ProjectLifecycle::autosaveFailedWith(const QString& reason) {
    setError(tr("Autosave failed: %1").arg(reason));
    m_autosaveOk = false;
    m_autosaveText = tr("Autosave failed %1: %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm")), reason);
    emit autosaveChanged();
    emit autosaveFailed(reason);
}

// ---- Import -----------------------------------------------------------------------------------------

QVariantMap ProjectLifecycle::importFiles(const QList<QUrl>& files, const QVariantMap& options) {
    QVariantMap summary;
    if (!m_document) {
        return summary;
    }
    // Home screen options (M5f); see the header.
    const bool freshProject = options.value(QStringLiteral("newProject")).toBool();
    const bool openImported = options.value(QStringLiteral("openImported")).toBool();
    const bool experimentalPremiere = options.value(QStringLiteral("experimentalPremiereProject")).toBool();
    QList<QUrl> media;
    std::vector<std::vector<std::string>> mediaBins; // 0.4: per entry of `media`, its bins under the target bin
    QList<QUrl> timelines;
    QList<QUrl> captions;
    QVariantList projects;
    // 0.4 (Import Folder): what the folders given held and left out.
    QStringList folderNames;
    QVariantList skipped;
    QStringList unreadableFolders;
    QStringList cardsLeftOut; // "AVCHD card in DCIM was left out: ..."
    int notMedia = 0;
    bool tooDeep = false;
    bool truncated = false;
    m_importedMedia.clear();
    for (const QUrl& url : files) {
        switch (editor::classifyImportFile(toPath(url))) {
        case editor::ImportFileKind::Media:
            media.append(url);
            mediaBins.emplace_back();
            break;
        case editor::ImportFileKind::Captions:
            captions.append(url);
            break;
        case editor::ImportFileKind::Interchange:
            timelines.append(url);
            break;
        case editor::ImportFileKind::Project:
            projects.append(url);
            break;
        case editor::ImportFileKind::Folder: {
            const std::filesystem::path folderPath = toPath(url);
            const editor::FolderImportPlan plan = editor::collectImportFolder(folderPath);
            const QFileInfo folderInfo(toQString(folderPath));
            folderNames << (folderInfo.fileName().isEmpty() ? QDir::toNativeSeparators(folderInfo.absoluteFilePath())
                                                            : folderInfo.fileName());
            if (plan.files.empty() && plan.skipped.empty() && !QDir(folderInfo.absoluteFilePath()).isReadable())
                unreadableFolders << folderNames.back();
            for (const editor::FolderImportFile& file : plan.files) {
                media.append(toUrl(file.file));
                mediaBins.push_back(file.bins);
            }
            for (const editor::FolderImportSkipped& left : plan.skipped) {
                QString reason;
                switch (left.reason) {
                case editor::FolderSkip::NotMedia:
                    reason = QStringLiteral("notMedia");
                    ++notMedia;
                    break;
                case editor::FolderSkip::Hidden:
                    reason = QStringLiteral("hidden");
                    break;
                case editor::FolderSkip::Link:
                    reason = QStringLiteral("link");
                    break;
                case editor::FolderSkip::TimelineBundle:
                    reason = QStringLiteral("bundle");
                    break;
                case editor::FolderSkip::CameraCard: {
                    reason = QStringLiteral("cameraCard");
                    const auto card = editor::cameraCardAt(left.path);
                    const std::string_view kind = card ? editor::displayName(card->first) : std::string_view("Camera");
                    cardsLeftOut << tr("%1 card in %2 was left out: import it from the Media Browser.")
                                        .arg(QString::fromUtf8(kind.data(), static_cast<qsizetype>(kind.size())),
                                             toQString(left.path.filename()));
                    break;
                }
                case editor::FolderSkip::TooDeep:
                    reason = QStringLiteral("tooDeep");
                    tooDeep = true;
                    break;
                }
                skipped.append(QVariantMap{{QStringLiteral("name"), toQString(left.path.filename())},
                                           {QStringLiteral("reason"), reason}});
            }
            truncated = truncated || plan.truncated;
            break;
        }
        }
    }
    // The empty first sequence of a project started for timelines only: the first import replaces it.
    std::optional<SequenceId> placeholder;
    // 0.4: the bin the media goes in (the Library's open bin; a search bin: its parent; unknown: the root). A project
    // started for this import gets it all at its root.
    BinId targetBin;
    if (const auto bin = detail::parseId<BinId>(QLatin1Char('b'), options.value(QStringLiteral("bin")).toString())) {
        if (const Bin* found = m_document->session().snapshot()->findBin(*bin)) {
            targetBin = found->isSearch() ? found->parent : found->id;
        }
    }
    if ((m_demo || freshProject) && (!media.isEmpty() || !timelines.isEmpty() || !captions.isEmpty())) {
        startNewProject(media); // the demo is not a project: importing starts a real one
        targetBin = BinId{};
        if (freshProject && media.isEmpty() && !m_controller->snapshot()->sequences.empty()) {
            placeholder = m_controller->snapshot()->sequences.front()->id;
        }
    }
    std::optional<SequenceId> firstImported;

    QVariantList failed;
    int added = 0;
    int existing = 0;
    int unreadable = 0;
    int binsMade = 0;
    if (!media.isEmpty()) {
        std::vector<std::filesystem::path> paths;
        for (const QUrl& url : media) {
            paths.push_back(toPath(url));
        }
        const editor::MediaImportResult result =
            editor::importMediaFiles(m_document->session(), paths,
                                     {.projectFile = m_document->file(),
                                      .bin = targetBin,
                                      .scaling = EditingBridge::defaultMediaScalingSetting(),
                                      .stillDuration = EditingBridge::stillDurationSetting(), // P.2
                                      .binPaths = mediaBins,
                                      .undoLabel = folderNames.isEmpty() ? std::string() : std::string("Import Folder")});
        m_importedMedia.clear();
        for (std::size_t i = 0; i < result.byFile.size(); ++i) {
            // Fix round 1: the files given themselves; a folder's files go into its bins (a folder dropped on the
            // timeline is not edited in file by file).
            const bool fromFolder = i < mediaBins.size() && !mediaBins[i].empty();
            if (result.byFile[i] && !fromFolder) {
                m_importedMedia.push_back(result.byFile[i]);
            }
        }
        added = static_cast<int>(result.added.size());
        existing = static_cast<int>(result.alreadyInProject.size());
        unreadable = static_cast<int>(result.failed.size());
        binsMade = static_cast<int>(result.createdBins.size());
        for (const editor::FailedImport& f : result.failed) {
            failed.append(QVariantMap{{QStringLiteral("name"), toQString(f.file.filename())},
                                      {QStringLiteral("error"), QString::fromStdString(f.error.message)}});
        }
    }
    int imported = 0;
    for (const QUrl& url : timelines) {
        const std::filesystem::path path = toPath(url);
        editor::TimelineImportOptions timelineOptions;
        timelineOptions.projectFile = m_document->file();
        timelineOptions.mediaSearchRoots = {path.parent_path()};
        timelineOptions.experimentalPremiereProject = experimentalPremiere;
        if (placeholder && imported == 0) {
            timelineOptions.dropSequences = {*placeholder};
        }
        auto result = editor::importTimelineFile(m_document->session(), path, timelineOptions);
        if (!result) {
            failed.append(QVariantMap{{QStringLiteral("name"), toQString(path.filename())},
                                      {QStringLiteral("error"), QString::fromStdString(result.error().message)}});
            continue;
        }
        ++imported;
        if (!firstImported && !result->sequences.empty()) {
            firstImported = result->sequences.front();
        }
        emit importReport(reportToMap(*result, path));
    }
    // P.2: a caption file becomes a new caption track of the current sequence (captions.import, undoable), and its
    // report (styles, positions, 608 modes approximated) shows like a timeline's.
    int captionTracks = 0;
    for (const QUrl& url : captions) {
        const std::filesystem::path path = toPath(url);
        const ProjectSnapshot snap = m_document->session().snapshot();
        const Sequence* sequence = m_controller ? snap->findSequence(m_controller->currentSequenceId()) : nullptr;
        if (!sequence && !snap->sequences.empty()) {
            sequence = snap->sequences.front().get();
        }
        if (!sequence) {
            failed.append(QVariantMap{{QStringLiteral("name"), toQString(path.filename())},
                                      {QStringLiteral("error"), tr("Captions go into a sequence: create one first")}});
            continue;
        }
        command::CommandRequest request;
        request.command = "captions.import";
        request.params = command::Json{{"sequence", sequence->id.value}, {"path", pathToUtf8(path)}};
        command::CommandEngine engine(m_document->session(), ai::edit::editingCommands(), command::EnginePolicy{});
        const command::CommandResult result = engine.execute(request);
        if (!result.ok()) {
            failed.append(QVariantMap{{QStringLiteral("name"), toQString(path.filename())},
                                      {QStringLiteral("error"), QString::fromStdString(result.error->message)}});
            continue;
        }
        ++captionTracks;
        const command::Json& data = result.data;
        const command::Json counts = data.value("report", command::Json::object());
        QVariantList entries;
        for (const command::Json& e : counts.value("entries", command::Json::array())) {
            entries.append(QVariantMap{
                {QStringLiteral("outcome"), QString::fromStdString(e.value("outcome", std::string()))},
                {QStringLiteral("category"), QString::fromStdString(e.value("category", std::string()))},
                {QStringLiteral("reason"), QString::fromStdString(e.value("reason", std::string()))},
                {QStringLiteral("location"), QString::fromStdString(e.value("where", std::string()))},
            });
        }
        const auto format = interchange::captionFormatFromName(data.value("format", std::string()));
        const int cues = data.value("cues", 0);
        emit importReport(QVariantMap{
            {QStringLiteral("file"), toQString(path.filename())},
            {QStringLiteral("format"), format ? QString::fromUtf8(interchange::displayName(*format).data()) : QString()},
            {QStringLiteral("sequences"), 0},
            {QStringLiteral("media"), 0},
            {QStringLiteral("unresolved"), QVariantList{}},
            {QStringLiteral("carried"), counts.value("carried", 0)},
            {QStringLiteral("approximated"), counts.value("approximated", 0)},
            {QStringLiteral("dropped"), counts.value("dropped", 0)},
            {QStringLiteral("entries"), entries},
            //: e.g. "214 captions on a new caption track of Main Edit"
            {QStringLiteral("summary"), tr("%n caption(s) on a new caption track of %1", "", cues)
                                            .arg(QString::fromStdString(sequence->name))},
        });
    }
    if (openImported) {
        int firstIndex = -1;
        if (firstImported) {
            const auto& sequences = m_controller->snapshot()->sequences;
            const auto it = std::ranges::find_if(sequences, [&](const SequencePtr& s) { return s->id == *firstImported; });
            if (it != sequences.end()) {
                firstIndex = static_cast<int>(it - sequences.begin());
                m_controller->setCurrentSequenceIndex(firstIndex);
                m_controller->setPlayhead({});
            }
        }
        summary.insert(QStringLiteral("firstSequence"), firstIndex);
    }
    if (failed.isEmpty()) {
        setError({});
    } else {
        setError(tr("%n file(s) could not be imported", "", static_cast<int>(failed.size())));
    }
    adoptDocument();
    summary.insert(QStringLiteral("added"), added);
    summary.insert(QStringLiteral("existing"), existing);
    summary.insert(QStringLiteral("failed"), failed);
    summary.insert(QStringLiteral("timelines"), imported);
    summary.insert(QStringLiteral("captionTracks"), captionTracks);
    summary.insert(QStringLiteral("projects"), projects);
    // 0.4 (Import Folder): what the folders gave, and the toast that says so.
    summary.insert(QStringLiteral("folders"), static_cast<int>(folderNames.size()));
    summary.insert(QStringLiteral("bins"), binsMade);
    summary.insert(QStringLiteral("skipped"), skipped);
    summary.insert(QStringLiteral("truncated"), truncated);
    QString folderSummary;
    if (!folderNames.isEmpty()) {
        // Whole English words (no "file(s)"): the toast is read at a glance.
        const auto filesText = [](int n) { return n == 1 ? tr("1 file") : tr("%1 files").arg(n); };
        const auto binsText = [](int n) { return n == 1 ? tr("1 bin") : tr("%1 bins").arg(n); };
        const QString source = folderNames.size() == 1 ? folderNames.front() : tr("%1 folders").arg(folderNames.size());
        const QString leftOut = notMedia == 1 ? tr("1 file was left out (not media).")
                                              : tr("%1 files were left out (not media).").arg(notMedia);
        QStringList sentences;
        if (added > 0) {
            //: "Imported 12 files from Session1 into 3 bins."
            sentences << (binsMade > 0 ? tr("Imported %1 from %2 into %3.").arg(filesText(added), source, binsText(binsMade))
                                       : tr("Imported %1 from %2.").arg(filesText(added), source));
            if (notMedia > 0)
                sentences << leftOut;
        } else if (unreadableFolders.size() == folderNames.size() && media.isEmpty()) {
            sentences << (unreadableFolders.size() == 1 ? tr("This folder cannot be read: %1.").arg(unreadableFolders.front())
                                                        : tr("These folders cannot be read: %1.").arg(unreadableFolders.join(QStringLiteral(", "))));
        } else if (notMedia > 0) {
            //: "No media in Session1: 4 files were left out (not media)."
            sentences << tr("No media in %1: %2").arg(source, leftOut);
        } else if (existing > 0 && unreadable == 0) {
            sentences << tr("Nothing new in %1.").arg(source);
        } else {
            sentences << tr("No media in %1.").arg(source);
        }
        if (unreadable > 0)
            sentences << (unreadable == 1 ? tr("1 file could not be read.") : tr("%1 files could not be read.").arg(unreadable));
        if (existing > 0)
            sentences << (existing == 1 ? tr("1 file was already in the project.")
                                        : tr("%1 files were already in the project.").arg(existing));
        sentences << cardsLeftOut;
        if (tooDeep)
            sentences << tr("Folders more than %1 levels deep were left out.").arg(editor::FolderImportLimits{}.maxDepth);
        if (truncated)
            sentences << tr("Only the first %1 files were looked at.").arg(editor::FolderImportLimits{}.maxFiles);
        folderSummary = sentences.join(QLatin1Char(' '));
    }
    summary.insert(QStringLiteral("folderSummary"), folderSummary);
    return summary;
}

// ---- Home screen (M5f) ------------------------------------------------------------------------------

bool ProjectLifecycle::newProjectWith(const QVariantMap& sequence) {
    if (!m_document) {
        return false;
    }
    // The sequence and the Advanced settings, checked and applied before anything changes (UX rework).
    auto state = newProjectState(sequence);
    if (!state) {
        setError(state.error());
        return false;
    }
    setError({});
    startNewProjectFrom(std::move(*state));
    return true;
}

QString ProjectLifecycle::checkNewProjectSettings(const QVariantMap& sequence) {
    const auto state = newProjectState(sequence);
    return state ? QString() : state.error();
}

// ---- Export Timeline (0.1.1) ----------------------------------------------------------------------------------------

namespace {

QString exportFormatName(const QString& format) {
    if (format == QLatin1String("fcp7-xml"))
        return QStringLiteral("Final Cut Pro 7 XML");
    if (format == QLatin1String("fcpxml"))
        return QStringLiteral("FCPXML");
    if (format == QLatin1String("otio"))
        return QStringLiteral("OpenTimelineIO");
    if (format == QLatin1String("edl"))
        return QStringLiteral("CMX 3600 EDL");
    if (format == QLatin1String("aaf"))
        return QStringLiteral("AAF");
    if (format == QLatin1String("omf"))
        return QStringLiteral("OMF");
    return format;
}

/// The file suffix of an Export Timeline format.
QString exportFormatSuffix(const QString& format) {
    if (format == QLatin1String("fcp7-xml"))
        return QStringLiteral("xml");
    return format; // fcpxml, otio, edl, aaf, omf
}

/// A sequence name as a file name: Windows' reserved characters replaced, never empty.
QString fileNameFor(const QString& name) {
    static const QString kReserved = QStringLiteral("<>:\"/\\|?*");
    QString out = name;
    for (QChar& c : out) {
        if (kReserved.contains(c) || c.unicode() < 32)
            c = QLatin1Char('_');
    }
    out = out.trimmed();
    while (out.endsWith(QLatin1Char('.')))
        out.chop(1);
    return out.isEmpty() ? QStringLiteral("Sequence") : out;
}

} // namespace

bool ProjectLifecycle::exportTimeline(const QString& format, int sequenceIndex, const QUrl& file,
                                      const QVariantMap& audioPost) {
    if (!m_controller || m_exportingTimeline)
        return false;
    const ProjectSnapshot base = m_controller->snapshot();
    if (sequenceIndex < 0 || sequenceIndex >= static_cast<int>(base->sequences.size())) {
        setError(tr("Choose a sequence to export"));
        return false;
    }
    const QString local = file.toLocalFile();
    if (local.isEmpty()) {
        setError(tr("Choose where to save the timeline"));
        return false;
    }
    command::CommandRequest request;
    request.command = "interchange.export";
    request.params = command::Json{{"sequence", base->sequences[static_cast<std::size_t>(sequenceIndex)]->id.value},
                                   {"format", format.toStdString()},
                                   {"path", QDir::fromNativeSeparators(local).toStdString()}};
    const bool forAudioPost = format == QLatin1String("aaf") || format == QLatin1String("omf");
    if (forAudioPost && !audioPost.isEmpty()) {
        request.params["audioPost"] = command::Json::parse(
            QJsonDocument(QJsonObject::fromVariantMap(audioPost)).toJson(QJsonDocument::Compact).toStdString());
    }
    // P.2: AAF / OMF render the clips' audio (and Mixdown Video) through this host's playback and export paths.
    const std::filesystem::path projectFile = toPath(QUrl::fromLocalFile(filePath()));
    setError({});
    m_exportingTimeline = true;
    QPointer<ProjectLifecycle> self(this);
    const QString fileName = QFileInfo(local).fileName();
    // The command runs on a worker, on a scratch session over the snapshot: the GUI thread only gets the report.
    QThreadPool::globalInstance()->start([self, base, request, fileName, format, local, forAudioPost, projectFile] {
        command::CommandResult result;
        try {
            EditSession scratch(*base);
            command::EnginePolicy policy;
            if (forAudioPost) {
                policy.services = exporter::interchangeServices(projectFile);
            }
            command::CommandEngine engine(scratch, ai::edit::editingCommands(), policy);
            result = engine.execute(request);
        } catch (const std::exception& e) {
            result.error = command::CommandError{command::ErrorCode::Internal, e.what(), {}};
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, result = std::move(result), fileName, format, local, forAudioPost] {
                if (!self)
                    return;
                self->m_exportingTimeline = false;
                QVariantMap report{{QStringLiteral("exported"), true},
                                   {QStringLiteral("ok"), result.ok()},
                                   {QStringLiteral("file"), fileName},
                                   {QStringLiteral("path"), local},
                                   {QStringLiteral("format"), exportFormatName(format)}};
                if (!result.ok()) {
                    const QString error = tr("The timeline could not be exported: %1")
                                              .arg(QString::fromStdString(result.error->message));
                    report.insert(QStringLiteral("error"), error);
                    self->setError(error);
                    emit self->timelineExported(report);
                    return;
                }
                const command::Json& data = result.data;
                const command::Json counts = data.value("report", command::Json::object());
                QVariantList entries;
                for (const command::Json& e : counts.value("entries", command::Json::array())) {
                    entries.append(QVariantMap{
                        {QStringLiteral("outcome"), QString::fromStdString(e.value("outcome", std::string()))},
                        {QStringLiteral("category"), QString::fromStdString(e.value("category", std::string()))},
                        {QStringLiteral("reason"), QString::fromStdString(e.value("reason", std::string()))},
                        {QStringLiteral("location"), QString::fromStdString(e.value("where", std::string()))},
                    });
                }
                const int sequences = static_cast<int>(data.value("sequences", command::Json::array()).size());
                report.insert(QStringLiteral("sequences"), forAudioPost ? 1 : sequences);
                report.insert(QStringLiteral("carried"), counts.value("carried", 0));
                report.insert(QStringLiteral("approximated"), counts.value("approximated", 0));
                report.insert(QStringLiteral("dropped"), counts.value("dropped", 0));
                report.insert(QStringLiteral("entries"), entries);
                if (forAudioPost) {
                    const int tracks = data.value("tracks", 0);
                    const int clips = data.value("clips", 0);
                    const int files = static_cast<int>(data.value("audioFiles", command::Json::array()).size());
                    //: e.g. "AAF · 4 audio tracks · 12 clips · C:\Projects\mix.aaf"
                    QString summary = tr("%1 · %n audio track(s)", "", tracks).arg(exportFormatName(format)) + QStringLiteral(" · ") +
                                      tr("%n clip(s)", "", clips) + QStringLiteral(" · ") + QDir::toNativeSeparators(local);
                    if (files > 0) {
                        summary += QStringLiteral(" · ") + tr("%n audio file(s) next to it", "", files);
                    }
                    report.insert(QStringLiteral("summary"), summary);
                } else {
                    //: e.g. "Final Cut Pro 7 XML · 2 sequences · C:\Projects\cut.xml"
                    report.insert(QStringLiteral("summary"), tr("%1 · %n sequence(s) · %2", "", sequences)
                                                                 .arg(exportFormatName(format), QDir::toNativeSeparators(local)));
                }
                emit self->timelineExported(report);
            },
            Qt::QueuedConnection);
    });
    return true;
}

bool ProjectLifecycle::exportTimelines(const QString& format, const QVariantList& sequenceIndices, const QUrl& folder,
                                       const QVariantMap& audioPost) {
    if (!m_controller || m_exportingTimeline)
        return false;
    const ProjectSnapshot base = m_controller->snapshot();
    std::vector<int> indices;
    for (const QVariant& v : sequenceIndices) {
        const int i = v.toInt();
        if (i < 0 || i >= static_cast<int>(base->sequences.size())) {
            setError(tr("Choose the sequences to export"));
            return false;
        }
        if (std::ranges::find(indices, i) == indices.end())
            indices.push_back(i);
    }
    if (indices.empty()) {
        setError(tr("Choose the sequences to export"));
        return false;
    }
    const QString dir = folder.toLocalFile();
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
        setError(tr("Choose a folder for the timeline files"));
        return false;
    }
    const bool forAudioPost = format == QLatin1String("aaf") || format == QLatin1String("omf");
    command::Json audioPostJson;
    if (forAudioPost && !audioPost.isEmpty()) {
        audioPostJson = command::Json::parse(
            QJsonDocument(QJsonObject::fromVariantMap(audioPost)).toJson(QJsonDocument::Compact).toStdString());
    }
    // One file per sequence, named after it ("Name.xml", "Name (2).xml" when two sequences share a name).
    struct Job {
        command::CommandRequest request;
        QString sequence;
        QString file;
        QString path;
    };
    std::vector<Job> jobs;
    QStringList taken;
    for (const int i : indices) {
        const Sequence& seq = *base->sequences[static_cast<std::size_t>(i)];
        const QString name = QString::fromStdString(seq.name);
        const QString stem = fileNameFor(name);
        const QString suffix = QLatin1Char('.') + exportFormatSuffix(format);
        QString file = stem + suffix;
        for (int n = 2; taken.contains(file, Qt::CaseInsensitive); ++n)
            file = QStringLiteral("%1 (%2)%3").arg(stem).arg(n).arg(suffix);
        taken << file;
        Job job;
        job.sequence = name;
        job.file = file;
        job.path = QDir(dir).filePath(file);
        job.request.command = "interchange.export";
        job.request.params = command::Json{{"sequence", seq.id.value},
                                           {"format", format.toStdString()},
                                           {"path", QDir::fromNativeSeparators(job.path).toStdString()}};
        if (!audioPostJson.is_null())
            job.request.params["audioPost"] = audioPostJson;
        jobs.push_back(std::move(job));
    }
    const std::filesystem::path projectFile = toPath(QUrl::fromLocalFile(filePath()));
    setError({});
    m_exportingTimeline = true;
    QPointer<ProjectLifecycle> self(this);
    // One worker writes the files one after another, on a scratch session over the snapshot; one combined report.
    QThreadPool::globalInstance()->start([self, base, jobs, format, dir, forAudioPost, projectFile] {
        std::vector<command::CommandResult> results;
        for (const Job& job : jobs) {
            command::CommandResult result;
            try {
                EditSession scratch(*base);
                command::EnginePolicy policy;
                if (forAudioPost) {
                    policy.services = exporter::interchangeServices(projectFile);
                }
                command::CommandEngine engine(scratch, ai::edit::editingCommands(), policy);
                result = engine.execute(job.request);
            } catch (const std::exception& e) {
                result.error = command::CommandError{command::ErrorCode::Internal, e.what(), {}};
            }
            results.push_back(std::move(result));
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, results = std::move(results), jobs, format, dir] {
                if (!self)
                    return;
                self->m_exportingTimeline = false;
                QVariantList files;
                QVariantList entries;
                QStringList failures;
                int carried = 0;
                int approximated = 0;
                int dropped = 0;
                int written = 0;
                for (std::size_t i = 0; i < jobs.size(); ++i) {
                    const Job& job = jobs[i];
                    const command::CommandResult& result = results[i];
                    QVariantMap file{{QStringLiteral("sequence"), job.sequence},
                                     {QStringLiteral("file"), job.file},
                                     {QStringLiteral("path"), job.path},
                                     {QStringLiteral("ok"), result.ok()}};
                    if (!result.ok()) {
                        const QString why = QString::fromStdString(result.error->message);
                        file.insert(QStringLiteral("error"), why);
                        //: Export Timeline, one sequence that failed: "Interview: the file cannot be written"
                        failures << tr("%1: %2").arg(job.sequence, why);
                        files.append(file);
                        continue;
                    }
                    ++written;
                    files.append(file);
                    const command::Json counts = result.data.value("report", command::Json::object());
                    carried += counts.value("carried", 0);
                    approximated += counts.value("approximated", 0);
                    dropped += counts.value("dropped", 0);
                    for (const command::Json& e : counts.value("entries", command::Json::array())) {
                        const QString where = QString::fromStdString(e.value("where", std::string()));
                        entries.append(QVariantMap{
                            {QStringLiteral("outcome"), QString::fromStdString(e.value("outcome", std::string()))},
                            {QStringLiteral("category"), QString::fromStdString(e.value("category", std::string()))},
                            {QStringLiteral("reason"), QString::fromStdString(e.value("reason", std::string()))},
                            //: Export Timeline report row of several sequences: "Interview · V1, 00:00:04:12"
                            {QStringLiteral("location"),
                             where.isEmpty() ? job.sequence : tr("%1 · %2").arg(job.sequence, where)},
                        });
                    }
                }
                const bool ok = failures.isEmpty();
                QVariantMap report{{QStringLiteral("exported"), true},
                                   {QStringLiteral("ok"), ok},
                                   //: The export report's title after Export Timeline of several sequences: "Exported 3 timelines"
                                   {QStringLiteral("file"), tr("%n timeline(s)", "", static_cast<int>(jobs.size()))},
                                   {QStringLiteral("path"), dir},
                                   {QStringLiteral("format"), exportFormatName(format)},
                                   {QStringLiteral("files"), files},
                                   {QStringLiteral("sequences"), written},
                                   {QStringLiteral("carried"), carried},
                                   {QStringLiteral("approximated"), approximated},
                                   {QStringLiteral("dropped"), dropped},
                                   {QStringLiteral("entries"), entries}};
                //: e.g. "Final Cut Pro 7 XML · 3 timelines · C:\Projects\Exports"
                report.insert(QStringLiteral("summary"), tr("%1 · %n timeline(s) · %2", "", written)
                                                             .arg(exportFormatName(format), QDir::toNativeSeparators(dir)));
                if (!ok) {
                    const QString error = tr("%n timeline(s) could not be exported: %1", "", static_cast<int>(failures.size()))
                                              .arg(failures.join(QStringLiteral("; ")));
                    report.insert(QStringLiteral("error"), error);
                    self->setError(error);
                }
                emit self->timelineExported(report);
            },
            Qt::QueuedConnection);
    });
    return true;
}

QVariantMap ProjectLifecycle::detectImport(const QUrl& file) const {
    const std::filesystem::path path = toPath(file);
    QVariantMap out{{QStringLiteral("format"), QString()},     {QStringLiteral("formatName"), QString()},
                    {QStringLiteral("supported"), true},       {QStringLiteral("experimental"), false},
                    {QStringLiteral("guidance"), QString()}};
    switch (editor::classifyImportFile(path)) {
    case editor::ImportFileKind::Media:
        out.insert(QStringLiteral("kind"), QStringLiteral("media"));
        return out;
    case editor::ImportFileKind::Project:
        out.insert(QStringLiteral("kind"), QStringLiteral("project"));
        return out;
    case editor::ImportFileKind::Folder: // 0.4: importFiles expands it into bins
        out.insert(QStringLiteral("kind"), QStringLiteral("folder"));
        return out;
    case editor::ImportFileKind::Captions: {
        out.insert(QStringLiteral("kind"), QStringLiteral("captions"));
        std::string extension = pathToUtf8(path.extension());
        if (const auto format = extension.size() > 1 ? interchange::captionFormatFromName(extension.substr(1)) : std::nullopt) {
            out.insert(QStringLiteral("format"), QString::fromUtf8(interchange::toString(*format).data()));
            out.insert(QStringLiteral("formatName"), QString::fromUtf8(interchange::displayName(*format).data()));
        }
        return out;
    }
    case editor::ImportFileKind::Interchange:
        break;
    }
    const interchange::Detection detection = interchange::ImportRouter::detect(path);
    const bool experimental = detection.format == interchange::Format::PremiereProject;
    out.insert(QStringLiteral("kind"), QStringLiteral("timeline"));
    out.insert(QStringLiteral("format"), QString::fromUtf8(interchange::toString(detection.format).data()));
    out.insert(QStringLiteral("formatName"), QString::fromUtf8(interchange::displayName(detection.format).data()));
    out.insert(QStringLiteral("supported"), detection.supported);
    out.insert(QStringLiteral("experimental"), experimental);
    out.insert(QStringLiteral("guidance"), QString::fromStdString(detection.guidance));
    return out;
}

// ---- Relink -----------------------------------------------------------------------------------------

QVariantList ProjectLifecycle::relinkCandidates(const QString& mediaId, const QList<QUrl>& folders) const {
    QVariantList list;
    const auto id = parseMediaKey(mediaId);
    if (!m_document || !id) {
        return list;
    }
    std::vector<std::filesystem::path> roots;
    for (const QUrl& url : folders) {
        roots.push_back(toPath(url));
    }
    for (const editor::RelinkCandidate& c : m_document->findRelinkCandidates(*id, roots)) {
        list.append(QVariantMap{{QStringLiteral("url"), toUrl(c.file)},
                                {QStringLiteral("path"), QDir::toNativeSeparators(toQString(c.file))},
                                {QStringLiteral("matches"), c.fingerprintMatches}});
    }
    return list;
}

bool ProjectLifecycle::relink(const QString& mediaId, const QUrl& file, bool force) {
    if (const AssetId asset = assetOfReference(mediaId.toStdString()); asset.isValid()) { // a LUT asset (I.5)
        if (!m_document) {
            return false;
        }
        if (auto relinked = m_document->relinkAsset(asset, toPath(file), force); !relinked) {
            setError(QString::fromStdString(relinked.error().message));
            return false;
        }
        setError({});
        adoptDocument();
        return true;
    }
    const auto id = parseMediaKey(mediaId);
    if (!m_document || !id) {
        return false;
    }
    if (auto relinked = m_document->relink(*id, toPath(file), force); !relinked) {
        setError(QString::fromStdString(relinked.error().message));
        return false;
    }
    setError({});
    adoptDocument();
    return true;
}

int ProjectLifecycle::relinkAll(const QList<QUrl>& folders) {
    if (!m_document) {
        return 0;
    }
    std::vector<std::filesystem::path> roots;
    for (const QUrl& url : folders) {
        roots.push_back(toPath(url));
    }
    auto relinked = m_document->relinkAll(roots);
    if (!relinked) {
        setError(QString::fromStdString(relinked.error().message));
        return 0;
    }
    setError({});
    adoptDocument();
    return static_cast<int>(relinked->size());
}

// ---- Library -> timeline ----------------------------------------------------------------------------

bool ProjectLifecycle::dropMedia(const QString& mediaId, const QString& timecode, const QString& trackId,
                                 bool overwrite) {
    const auto media = parseMediaKey(mediaId);
    // A sequence from the Project panel: nested where it is dropped (C.8).
    const auto nested = media ? std::nullopt : parseSequenceKey(mediaId);
    const SequencePtr sequence = m_controller ? m_controller->currentSequence() : nullptr;
    if (!m_document || (!media && !nested) || !sequence) {
        return false;
    }
    const auto at = parseTimecode(timecode.toStdString(), sequence->frameRate, sequence->dropFrame);
    if (!at) {
        setError(QString::fromStdString(at.error().message));
        return false;
    }
    editor::MediaDrop drop;
    drop.media = media.value_or(MediaId{});
    drop.sourceSequence = nested.value_or(SequenceId{});
    drop.sequence = sequence->id;
    drop.at = *at;
    drop.track = parseTrackKey(trackId).value_or(TrackId{});
    drop.mode = overwrite ? editor::DropMode::Overwrite : editor::DropMode::Insert;
    drop.useSourceMarks = true; // 0.2 UX (stream T): the item's Source Monitor In and Out, as Premiere's drag does
    auto edited = editor::dropMedia(m_document->session(), drop);
    if (!edited) {
        setError(QString::fromStdString(edited.error().message));
        return false;
    }
    setError({});
    m_controller->setSelection(edited->placed, edited->placed.empty() ? ClipId{} : edited->placed.front());
    return true;
}

QVariantMap ProjectLifecycle::dropFiles(const QList<QUrl>& files, const QString& timecode, const QString& trackId,
                                        bool overwrite) {
    QVariantMap summary = importFiles(files);
    const std::vector<MediaId> media = m_importedMedia;
    const SequencePtr sequence = m_controller ? m_controller->currentSequence() : nullptr;
    int placed = 0;
    if (m_document && sequence && !media.empty()) {
        auto at = parseTimecode(timecode.toStdString(), sequence->frameRate, sequence->dropFrame);
        if (!at) {
            at = Ticks{};
        }
        // The track the files were dropped on, if the sequence (a new one, after the demo) still has it.
        TrackId track = parseTrackKey(trackId).value_or(TrackId{});
        if (track && !sequence->findTrack(track)) {
            track = TrackId{};
        }
        std::vector<ClipId> clips;
        QString error;
        for (const MediaId id : media) {
            editor::MediaDrop drop;
            drop.media = id;
            drop.sequence = sequence->id;
            drop.at = *at;
            drop.track = track;
            drop.mode = overwrite ? editor::DropMode::Overwrite : editor::DropMode::Insert;
            auto edited = editor::dropMedia(m_document->session(), drop);
            if (!edited) {
                error = QString::fromStdString(edited.error().message);
                continue;
            }
            ++placed;
            // The next file follows this one.
            const SequencePtr after = m_controller->snapshot()->sequencePtr(sequence->id);
            for (const ClipId clip : edited->placed) {
                clips.push_back(clip);
                if (!after) {
                    continue;
                }
                if (const auto found = after->findClip(clip)) {
                    *at = std::max(*at, found->clip().end());
                }
            }
        }
        if (!error.isEmpty()) {
            setError(error);
        }
        if (!clips.empty()) {
            m_controller->setSelection(clips, clips.front());
        }
    }
    summary.insert(QStringLiteral("placed"), placed);
    // Fix round 1: a folder dropped here came in as bins (importFiles, its one step) and stays off the timeline; the
    // toast says where it went when it brought media.
    const int fromFolders = summary.value(QStringLiteral("added")).toInt() + summary.value(QStringLiteral("existing")).toInt() -
                            static_cast<int>(media.size());
    if (summary.value(QStringLiteral("folders")).toInt() > 0 && fromFolders > 0) {
        const QString said = summary.value(QStringLiteral("folderSummary")).toString();
        const QString where = tr("Folders go into the Library as bins: drag their clips onto the timeline from there.");
        summary.insert(QStringLiteral("folderSummary"), said.isEmpty() ? where : said + QLatin1Char(' ') + where);
    }
    return summary;
}

bool ProjectLifecycle::insertModifierHeld() const {
    return QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ControlModifier);
}

} // namespace wabcut::app
