#include "ProjectController.h"

#include "PosterFrames.h"
#include "RedactedPictures.h"

#include "ClipAccessModel.h"
#include "DemoProject.h"
#include "EffectControls.h"
#include "MediaBridge.h"
#include "NestedPictures.h"
#include "ParameterUi.h"

#include <wabcut/command/ExecutionContext.h>

#include <wabcut/model/ClipGroups.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequenceSettings.h>
#include <wabcut/model/Timecode.h>

#include <QDir>
#include <QHash>
#include <QQmlEngine>
#include <QStandardPaths>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <set>
#include <span>
#include <unordered_set>
#include <string>
#include <utility>

namespace wabcut::app {

namespace {

// Edit labels are stored untranslated in the undo history and translated when shown (undoText/redoText).

// Header chip tints (indices into Theme.clipColors) by track position: V1 lime, V2 cyan, V3 pink, …
constexpr std::array<int, 8> kVideoTrackColors{0, 1, 3, 4, 5, 2, 6, 7};
constexpr std::array<int, 8> kAudioTrackColors{0, 7, 6, 2, 4, 1, 5, 3};

QString shortTrackName(TrackKind kind, int index) {
    return (kind == TrackKind::Video ? QStringLiteral("V") : QStringLiteral("A")) + QString::number(index + 1);
}

/// Every parameter in UI units, evaluated at curve time `curveTime` (Clip::curveTimeAt).
QVariantMap toUi(const ClipProperties& props, const PropertyFrame& f, Ticks curveTime = {}) {
    QVariantMap map;
    for (const UiParameter& p : uiParameters())
        map.insert(QLatin1String(p.key), p.range.toUi(props.value(p.id, curveTime), f));
    return map;
}

bool hasAnimatedParameters(const ClipProperties& props) {
    return std::ranges::any_of(props.values, [](const ParamValue& v) { return !v.isConstant(); });
}

bool isAudioProperty(const QString& key) {
    const UiParameter* p = uiParameter(key);
    return p && parameterInfo(p->id).role == PropertyRole::Audio;
}

} // namespace

// =====================================================================================================
// TrackListModel

TrackListModel::TrackListModel(QObject* parent) : QAbstractListModel(parent) {}

int TrackListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_data.size());
}

QHash<int, QByteArray> TrackListModel::roleNames() const {
    return {
        {TrackIdRole, "trackId"},     {ShortNameRole, "shortName"}, {NameRole, "name"},
        {KindRole, "kind"},           {ColorIndexRole, "colorIndex"}, {EnabledRole, "trackEnabled"},
        {LockedRole, "locked"},       {MutedRole, "muted"},         {SoloRole, "solo"},
        {TargetedRole, "targeted"},   {SyncLockedRole, "syncLocked"},
    };
}

QVariant TrackListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};
    const Row& r = m_data[static_cast<std::size_t>(index.row())];
    switch (role) {
    case TrackIdRole:
        return trackKey(r.id);
    case ShortNameRole:
        return shortTrackName(r.kind, r.index);
    case NameRole:
        return r.name;
    case KindRole:
        return r.kind == TrackKind::Video ? QStringLiteral("video") : QStringLiteral("audio");
    case ColorIndexRole: {
        const auto& table = r.kind == TrackKind::Video ? kVideoTrackColors : kAudioTrackColors;
        return table[static_cast<std::size_t>(r.index) % table.size()];
    }
    case EnabledRole:
        return r.enabled;
    case LockedRole:
        return r.locked;
    case MutedRole:
        return r.muted;
    case SoloRole:
        return r.solo;
    case TargetedRole:
        return r.targeted;
    case SyncLockedRole:
        return r.syncLocked;
    default:
        return {};
    }
}

TrackListModel::Row TrackListModel::makeRow(const Track& track, int index) {
    Row r;
    r.id = track.id;
    r.kind = track.kind;
    r.index = index;
    r.name = !track.name.empty() ? QString::fromStdString(track.name)
             : track.kind == TrackKind::Video
                 ? ProjectController::tr("Video %1").arg(index + 1)
                 : ProjectController::tr("Audio %1").arg(index + 1);
    r.enabled = track.enabled;
    r.locked = track.locked;
    r.muted = track.muted;
    r.solo = track.solo;
    r.targeted = track.targeted;
    r.syncLocked = track.syncLocked;
    return r;
}

void TrackListModel::update(const Sequence* sequence) {
    std::vector<Row> next;
    std::vector<const Track*> rows;
    if (sequence) {
        for (std::size_t i = sequence->videoTracks.size(); i-- > 0;) {
            next.push_back(makeRow(*sequence->videoTracks[i], static_cast<int>(i)));
            rows.push_back(sequence->videoTracks[i].get());
        }
        for (std::size_t i = 0; i < sequence->audioTracks.size(); ++i) {
            next.push_back(makeRow(*sequence->audioTracks[i], static_cast<int>(i)));
            rows.push_back(sequence->audioTracks[i].get());
        }
    }
    m_rows = std::move(rows);
    const bool sameShape = next.size() == m_data.size()
        && std::equal(next.begin(), next.end(), m_data.begin(),
                      [](const Row& a, const Row& b) { return a.id == b.id && a.kind == b.kind; });
    if (!sameShape) {
        beginResetModel();
        m_data = std::move(next);
        endResetModel();
        emit countChanged();
        return;
    }
    for (std::size_t i = 0; i < next.size(); ++i) {
        const Row& a = m_data[i];
        const Row& b = next[i];
        const bool same = a.name == b.name && a.enabled == b.enabled && a.locked == b.locked && a.muted == b.muted
            && a.solo == b.solo && a.targeted == b.targeted && a.syncLocked == b.syncLocked && a.index == b.index;
        m_data[i] = b;
        if (!same) {
            const QModelIndex idx = index(static_cast<int>(i));
            emit dataChanged(idx, idx);
        }
    }
}

// =====================================================================================================
// ProjectController

ProjectController::ProjectController(QObject* parent)
    : QObject(parent), m_session(std::make_unique<EditSession>()),
      m_clipboardServices(std::make_shared<command::ClipboardServices>()), m_pictures(std::make_shared<PictureProject>()),
      m_tracks(new TrackListModel(this)),
      m_clipList(new ClipAccessModel(this)), m_effectControls(new EffectControlsModel(this)) {
    connect(this, &ProjectController::selectionChanged, m_clipList, &ClipAccessModel::updateSelection);
    setMediaCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/media"));
    m_listener = m_session->addListener([this](const ChangeEvent& event) { onSessionChanged(event); });
    m_releaser = std::make_unique<SnapshotReleaser>(*m_session);
    DemoProject demo = buildDemoProject();
    resetProject(std::move(demo.state), demo.mainSequence, {demo.initialSelection});
    setPlayhead(demo.initialPlayhead);
}

ProjectController::~ProjectController() {
    emit sessionClosing();
    m_session->removeListener(m_listener);
}

ProjectController* ProjectController::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* controller = new ProjectController;
    controller->setSystemClipboardEnabled(true); // the app's clipboard is the system's (C.11)
    if (qmlEngine && !qmlEngine->imageProvider(QLatin1String(ThumbnailImageProvider::kProviderId))) {
        qmlEngine->addImageProvider(QLatin1String(PosterImageProvider::kProviderId), new PosterImageProvider);
        qmlEngine->addImageProvider(QLatin1String(ThumbnailImageProvider::kProviderId),
                                    new ThumbnailImageProvider(controller->m_mediaCacheDir.toStdU16String()));
    }
    if (qmlEngine && !qmlEngine->imageProvider(QLatin1String(SequenceThumbnailProvider::kProviderId))) {
        // Library thumbnails of sequences (I.9), rendered like an export from this controller's project.
        qmlEngine->addImageProvider(QLatin1String(SequenceThumbnailProvider::kProviderId),
                                    new SequenceThumbnailProvider(controller->m_pictures));
    }
    return controller;
}

std::shared_ptr<const PeakSource> ProjectController::peakSource() const {
    return m_peaks;
}

std::shared_ptr<const FilmstripSource> ProjectController::filmstripSource() const {
    return m_filmstrips;
}

void ProjectController::setMediaCacheDirectory(const QString& directory) {
    if (directory == m_mediaCacheDir && m_peaks)
        return;
    m_mediaCacheDir = directory;
    m_peaks = std::make_shared<MediaPeakSource>(std::filesystem::path(directory.toStdU16String()));
    connect(m_peaks.get(), &MediaPeakSource::peaksReady, this, &ProjectController::peaksChanged);
    m_filmstrips = std::make_shared<MediaFilmstripSource>(std::filesystem::path(directory.toStdU16String()));
    connect(m_filmstrips.get(), &MediaFilmstripSource::stripsReady, this, &ProjectController::filmstripsChanged);
    refreshMedia();
    emit peaksChanged();
    emit filmstripsChanged();
}

void ProjectController::refreshMedia() {
    const ProjectSnapshot snap = m_session->snapshot();
    // Thumbnails and filmstrips are redacted against this project (Codex wave-4 P2-9).
    redacted::publish(snap);
    std::unordered_map<std::uint64_t, MediaPeakSource::MediaFile> files;
    std::unordered_map<std::uint64_t, MediaFilmstripSource::MediaFile> filmFiles; // M5i: proxies preferred
    QVariantList items;
    if (snap) {
        for (const MediaPtr& media : snap->media) {
            const MediaInfo& info = media->info;
            std::string path;
            if (const auto resolved = media->locator.offline ? std::nullopt // not the original file
                                                             : resolveMediaPath(media->locator, m_projectFile)) {
                std::error_code ec;
                if (std::filesystem::is_regular_file(*resolved, ec))
                    path = pathToUtf8(*resolved);
            }
            if (info.hasVideo() && !media->isGenerator()) {
                std::string filmPath = path;
                std::error_code ec;
                if (media->locator.proxy) {
                    const std::filesystem::path proxy = pathFromUtf8(media->locator.proxy->path);
                    if (std::filesystem::is_regular_file(proxy, ec))
                        filmPath = media->locator.proxy->path;
                }
                if (!filmPath.empty())
                    filmFiles.emplace(media->id.value, MediaFilmstripSource::MediaFile{filmPath, info.duration.toSeconds(),
                                                                                       redacted::keyOf(*snap, media->id)});
            }
            if (!path.empty()) {
                MediaPeakSource::MediaFile file{path, {}};
                for (const AudioStreamInfo& a : info.audio)
                    file.audioStreams.push_back(a.index);
                files.emplace(media->id.value, std::move(file));
            }

            const bool video = info.hasVideo();
            // Titles are Text; stills sit with the media (M5j: they were listed as text).
            const QString kind = media->isTitle() ? QStringLiteral("text")
                               : video            ? QStringLiteral("video")
                               : info.hasAudio()  ? QStringLiteral("audio")
                                                  : QStringLiteral("text");
            QString meta;
            if (video) {
                const VideoStreamInfo& v = info.video.front();
                meta = QStringLiteral("%1×%2").arg(v.width).arg(v.height);
                if (!info.isStillImage && v.frameRate.num > 0)
                    meta += QStringLiteral(" · ") + QString::number(v.frameRate.toDouble(), 'g', 5);
            } else if (info.hasAudio()) {
                const AudioStreamInfo& a = info.audio.front();
                meta = tr("%1 kHz · %2 ch").arg(QString::number(a.sampleRate.toDouble() / 1000.0, 'g', 4)).arg(a.channels);
            }
            const bool offline = path.empty();
            const int hue = static_cast<int>(media->id.value % 8);
            items.append(QVariantMap{
                {QStringLiteral("id"), mediaKey(media->id)},
                {QStringLiteral("name"), QString::fromStdString(media->name)},
                {QStringLiteral("kind"), kind},
                {QStringLiteral("durationText"), formatTimecode(info.duration).mid(3)},
                {QStringLiteral("meta"), meta},
                {QStringLiteral("colors"), QVariantList{hue, (hue + 3) % 8}},
                {QStringLiteral("thumbnail"), !offline && video ? ThumbnailImageProvider::urlFor(path) : QString()},
                {QStringLiteral("offline"), offline},
                {QStringLiteral("path"), QString::fromStdString(media->locator.path)},
            });
        }
    }
    const bool hasFiles = !files.empty();
    if (m_peaks)
        m_peaks->setMediaFiles(std::move(files));
    if (m_filmstrips)
        m_filmstrips->setMediaFiles(std::move(filmFiles));
    publishPictures();
    m_mediaItems = std::move(items);
    m_hasMediaFiles = hasFiles;
    emit mediaChanged();
}

void ProjectController::publishPictures() {
    const ProjectSnapshot snap = m_session->snapshot();
    m_pictures->set(snap, m_projectFile);
    if (m_filmstrips)
        m_filmstrips->setProject(snap, m_projectFile);
}

SequencePtr ProjectController::currentSequence() const {
    const ProjectSnapshot snap = m_session->snapshot();
    return snap ? snap->sequencePtr(m_sequence) : nullptr;
}

void ProjectController::resetProject(ProjectState state, SequenceId sequence, std::vector<ClipId> selection) {
    m_sequence = sequence;
    m_selection = std::move(selection);
    m_primary = m_selection.empty() ? ClipId{} : m_selection.front();
    m_playhead = {};
    m_finePlayhead = {};
    m_session->reset(std::move(state)); // its Reset event resyncs everything
}

void ProjectController::onSessionChanged(const ChangeEvent& event) {
    // A reset replaces the project: treat it as "everything changed" whatever its ChangeSet says.
    syncAfterChange(event.kind == ChangeKind::Reset ? nullptr : &event.changes, event.kind, event.label);
}

void ProjectController::ensureValidSequence() {
    const ProjectSnapshot snap = m_session->snapshot();
    if (!snap || snap->sequences.empty()) {
        m_sequence = {};
        return;
    }
    if (!snap->findSequence(m_sequence))
        m_sequence = snap->sequences.front()->id;
}

// ---- TimelineChange -------------------------------------------------------------------------------------------------

const SequenceChange* TimelineChange::sequenceChange() const {
    return changes ? changes->sequence(sequence) : nullptr;
}

bool TimelineChange::touches(ClipId clip) const {
    const ClipId one[] = {clip};
    return touchesAny(one);
}

bool TimelineChange::touchesAny(std::span<const ClipId> clips, std::span<const MediaId> media) const {
    if (structural || !changes)
        return true;
    if (!media.empty()) {
        for (MediaId m : media) {
            if (std::ranges::find(changes->modifiedMedia, m) != changes->modifiedMedia.end()
                || std::ranges::find(changes->removedMedia, m) != changes->removedMedia.end())
                return true;
        }
    }
    const SequenceChange* s = sequenceChange();
    if (!s || clips.empty())
        return false;
    for (const std::vector<ClipId>* list : {&s->modifiedClips, &s->removedClips, &s->addedClips}) {
        for (ClipId id : *list) {
            if (std::ranges::find(clips, id) != clips.end())
                return true;
        }
    }
    return false;
}

bool TimelineChange::projectWide() const {
    return structural || !changes || !changes->modifiedMedia.empty() || !changes->addedMedia.empty()
        || !changes->removedMedia.empty() || changes->redactionsChanged || changes->assetsChanged;
}

bool ProjectController::lastChangeAtPlayhead() const {
    if (m_lastChange.structural || !m_lastChange.changes)
        return true;
    const SequenceChange* s = m_lastChange.sequenceChange();
    const SequencePtr seq = currentSequence();
    if (!s || !seq)
        return false;
    for (const TrackChange& t : s->tracks) {
        const auto pos = seq->trackPosition(t.track);
        if (pos && pos->kind != TrackKind::Video)
            continue;
        if (t.headerChanged || t.added || t.removed)
            return true;
        if (std::ranges::any_of(t.ranges, [&](const TimeRange& r) { return r.contains(m_playhead); }))
            return true;
    }
    return false;
}

void ProjectController::pruneSelection(const ChangeSet* changes, bool sequenceSwitched) {
    const SequencePtr seq = currentSequence();
    if (!seq) {
        m_selection.clear();
    } else if (changes && !sequenceSwitched && m_selection == m_checkedSelection) {
        // Every selected clip existed after the last change: only the ones this change removed go.
        if (const SequenceChange* s = changes->sequence(m_sequence); s && !s->removedClips.empty()) {
            const std::unordered_set<std::uint64_t> removed = [&] {
                std::unordered_set<std::uint64_t> out;
                for (ClipId id : s->removedClips)
                    out.insert(id.value);
                return out;
            }();
            std::erase_if(m_selection, [&](ClipId id) { return removed.contains(id.value); });
        }
    } else if (m_selection.size() <= 32) {
        // A new selection (or a reset, another sequence): check each id (a few lookups, not a set of every clip).
        std::erase_if(m_selection, [&](ClipId id) { return !seq->findClip(id).has_value(); });
    } else {
        std::unordered_set<std::uint64_t> present;
        seq->forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips)
                present.insert(c.id.value);
        });
        std::erase_if(m_selection, [&](ClipId id) { return !present.contains(id.value); });
    }
    m_checkedSelection = m_selection;
    if (std::find(m_selection.begin(), m_selection.end(), m_primary) == m_selection.end())
        m_primary = m_selection.empty() ? ClipId{} : m_selection.front();
}

void ProjectController::syncAfterChange(const ChangeSet* changes, ChangeKind kind, std::string_view label) {
    const ProjectSnapshot now = m_session->snapshot();
    const SequenceId previousSequence = m_sequence;
    ensureValidSequence();
    const std::vector<ClipId> previousSelection = m_selection;
    pruneSelection(changes, previousSequence != m_sequence);
    const bool transitionGone = m_selectedTransition && !findTransition(m_selectedTransition);
    if (transitionGone)
        m_selectedTransition = {};
    const Sequence* newSeq = now ? now->findSequence(m_sequence) : nullptr;

    // What changed in the current sequence, from the model's ChangeSet (edits, undo and redo all carry one).
    const SequenceChange* seqChange = changes ? changes->sequence(m_sequence) : nullptr;
    TimelineChange change;
    change.structural = !changes || previousSequence != m_sequence || !newSeq
                     || (seqChange && seqChange->trackListChanged);
    if (change.structural) {
        change.markersChanged = true;
        change.marksChanged = true;
    } else if (seqChange) {
        change.tracks = seqChange->tracks;
        change.markersChanged = seqChange->markersChanged;
        change.marksChanged = seqChange->marksChanged;
    }
    QStringList changedTracks;
    if (change.structural && newSeq) {
        newSeq->forEachTrack([&](const Track& t) { changedTracks << trackKey(t.id); });
    } else {
        for (const TrackChange& t : change.tracks)
            changedTracks << trackKey(t.track);
    }

    const bool headerChanged =
        change.structural || (seqChange && (seqChange->settingsChanged || seqChange->extentChanged));
    bool sequencesDiffer = !changes || !changes->addedSequences.empty() || !changes->removedSequences.empty();
    if (changes && !sequencesDiffer)
        sequencesDiffer = std::ranges::any_of(changes->sequences, [](const SequenceChange& c) { return c.settingsChanged; });

    change.changes = changes ? std::make_shared<const ChangeSet>(*changes) : nullptr;
    change.sequence = m_sequence;
    change.before = std::exchange(m_seenSnapshot, now);
    m_lastChange = std::move(change);
    ++m_revision;
    if (!changes || !changes->addedMedia.empty() || !changes->removedMedia.empty() || !changes->modifiedMedia.empty() ||
        changes->redactionsChanged)
        refreshMedia();
    else
        publishPictures(); // an edit inside a nested sequence changes the strips of the clips showing it
    if (m_lastChange.structural || !m_lastChange.tracks.empty())
        m_tracks->update(newSeq);
    if (m_lastChange.structural || !newSeq) {
        m_clipList->reset(newSeq);
    } else if (!m_lastChange.tracks.empty()) {
        std::vector<TrackId> changedIds;
        for (const TrackChange& t : m_lastChange.tracks)
            changedIds.push_back(t.track);
        m_clipList->updateTracks(*newSeq, changedIds);
    }
    if (m_playhead > contentDuration())
        setPlayhead(contentDuration());

    if (sequencesDiffer)
        emit sequencesChanged();
    if (headerChanged)
        emit sequenceChanged();
    if (m_lastChange.markersChanged)
        emit markersChanged();
    if (m_lastChange.marksChanged)
        emit marksChanged();
    emit historyChanged();
    if (previousSelection != m_selection)
        emit selectionChanged();
    if (transitionGone)
        emit selectedTransitionChanged();
    emit selectedClipChanged(); // the clip's parameters or name may have changed
    if (headerChanged || m_lastChange.markersChanged)
        emit playheadChanged(); // timecode format / marker index may differ
    emit revisionChanged();

    static constexpr std::array<const char*, 5> kKinds{"edit", "undo", "redo", "reset", "cancel"};
    emit changed({
        {QStringLiteral("kind"), QString::fromLatin1(kKinds[static_cast<std::size_t>(kind)])},
        {QStringLiteral("label"), QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size()))},
        {QStringLiteral("sequence"), sequenceKey(m_sequence)},
        {QStringLiteral("tracks"), changedTracks},
        {QStringLiteral("structural"), m_lastChange.structural},
        {QStringLiteral("markers"), m_lastChange.markersChanged},
        {QStringLiteral("marks"), m_lastChange.marksChanged},
    });
}

void ProjectController::setError(const QString& message) {
    if (m_lastError == message)
        return;
    m_lastError = message;
    emit lastErrorChanged();
}

template <class Fn>
bool ProjectController::applyEdit(const char* label, Fn&& fn, std::string_view mergeKey) {
    auto outcome = m_session->apply(label, std::forward<Fn>(fn), mergeKey);
    if (!outcome) {
        setError(QString::fromStdString(outcome.error().message));
        return false;
    }
    setError({});
    return true;
}

bool ProjectController::applyEditFn(const char* label, const EditFn& fn, std::string_view mergeKey) {
    return applyEdit(label, fn, mergeKey);
}

bool ProjectController::applyResult(const char* label, const EditFn& fn, bool followPlayhead) {
    std::optional<Ticks> playhead;
    const bool ok = applyEdit(label, [&](const ProjectState& state) -> edit::EditOutcome {
        auto result = fn(state);
        if (result)
            playhead = result->playhead;
        return result;
    });
    if (ok && followPlayhead && playhead)
        setPlayhead(*playhead);
    return ok;
}

// ---- Sequence ---------------------------------------------------------------------------------------

QStringList ProjectController::sequenceNames() const {
    QStringList names;
    if (const ProjectSnapshot snap = m_session->snapshot()) {
        for (const SequencePtr& s : snap->sequences)
            names << QString::fromStdString(s->name);
    }
    return names;
}

int ProjectController::currentSequenceIndex() const {
    if (const ProjectSnapshot snap = m_session->snapshot()) {
        for (std::size_t i = 0; i < snap->sequences.size(); ++i) {
            if (snap->sequences[i]->id == m_sequence)
                return static_cast<int>(i);
        }
    }
    return -1;
}

void ProjectController::setCurrentSequenceIndex(int index) {
    const ProjectSnapshot snap = m_session->snapshot();
    if (!snap || index < 0 || index >= static_cast<int>(snap->sequences.size()))
        return;
    const SequenceId next = snap->sequences[static_cast<std::size_t>(index)]->id;
    if (next == m_sequence)
        return;
    const bool hadSelection = !m_selection.empty();
    m_sequence = next;
    m_selection.clear();
    m_primary = {};
    m_playhead = {};
    m_finePlayhead = {};
    // Switching sequences is not an edit; resync everything as a structural change.
    syncAfterChange(nullptr, ChangeKind::Reset, {});
    if (hadSelection)
        emit selectionChanged();
    emit playheadChanged();
}

QString ProjectController::sequenceName() const {
    const SequencePtr seq = currentSequence();
    return seq ? QString::fromStdString(seq->name) : QString();
}

QString ProjectController::frameRateText() const {
    const SequencePtr seq = currentSequence();
    if (!seq)
        return {};
    const Rate r = seq->frameRate;
    QString number = r.den == 1 ? QString::number(r.num) : QString::number(r.toDouble(), 'f', 3);
    if (number.contains(QLatin1Char('.'))) {
        while (number.endsWith(QLatin1Char('0')))
            number.chop(1);
        if (number.endsWith(QLatin1Char('.')))
            number.chop(1);
    }
    return tr("%1 fps").arg(number);
}

QString ProjectController::formatText() const {
    const SequencePtr seq = currentSequence();
    if (!seq)
        return {};
    QString rate = frameRateText();
    rate = rate.left(rate.indexOf(QLatin1Char(' ')));
    //: Sequence format badge: frame height + "p" + frame rate, e.g. 1080p23.976
    return tr("%1p%2").arg(std::min(seq->width, seq->height)).arg(rate);
}

int ProjectController::frameWidth() const {
    const SequencePtr seq = currentSequence();
    return seq ? seq->width : 0;
}

int ProjectController::frameHeight() const {
    const SequencePtr seq = currentSequence();
    return seq ? seq->height : 0;
}

QString ProjectController::durationText() const {
    const SequencePtr seq = currentSequence();
    return seq ? formatTimecode(seq->duration) : QString();
}

int ProjectController::clipCount() const {
    const SequencePtr seq = currentSequence();
    int count = 0;
    if (seq)
        seq->forEachTrack([&](const Track& t) { count += static_cast<int>(t.clips.size()); });
    return count;
}

QVariantList ProjectController::markers() const {
    QVariantList list;
    if (const SequencePtr seq = currentSequence()) {
        for (const Marker& m : seq->markers) {
            list << QVariantMap{
                {QStringLiteral("name"), QString::fromStdString(m.name)},
                {QStringLiteral("comment"), QString::fromStdString(m.comment)},
                {QStringLiteral("timecode"), formatTimecode(m.start)},
                {QStringLiteral("colorIndex"), m.colorIndex},
            };
        }
    }
    return list;
}

QVariantList ProjectController::audioTracks() const {
    QVariantList list;
    const auto& rows = m_tracks->rows();
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        if (rows[static_cast<std::size_t>(i)]->kind != TrackKind::Audio)
            continue;
        const QModelIndex index = m_tracks->index(i);
        list << QVariantMap{
            {QStringLiteral("trackId"), m_tracks->data(index, TrackListModel::TrackIdRole)},
            {QStringLiteral("shortName"), m_tracks->data(index, TrackListModel::ShortNameRole)},
            {QStringLiteral("name"), m_tracks->data(index, TrackListModel::NameRole)},
            {QStringLiteral("colorIndex"), m_tracks->data(index, TrackListModel::ColorIndexRole)},
        };
    }
    return list;
}

QString ProjectController::formatTimecode(Ticks t) const {
    const SequencePtr seq = currentSequence();
    if (seq) // P.2: the sequence's Display Format (timecode, frames, feet + frames)
        return QString::fromStdString(edit::formatSequenceTime(*seq, t));
    return QString::fromStdString(ticksToTimecode(t, kRate30, false));
}

Ticks ProjectController::contentDuration() const {
    const SequencePtr seq = currentSequence();
    const Ticks duration = seq ? seq->duration : Ticks{};
    return std::max(duration + Ticks{duration.value / 25}, Ticks::fromSeconds(30));
}

void ProjectController::newSequence() {
    const SequencePtr current = currentSequence();
    edit::SequenceSettings settings;
    if (current) {
        settings.frameRate = current->frameRate;
        settings.width = current->width;
        settings.height = current->height;
        settings.audioRate = current->audioRate;
        settings.dropFrame = current->dropFrame;
    }
    const QStringList names = sequenceNames();
    int n = static_cast<int>(names.size()) + 1;
    QString name;
    do {
        name = tr("Sequence %1").arg(n++, 2, 10, QLatin1Char('0'));
    } while (names.contains(name));
    settings.name = name.toStdString();

    SequenceId created;
    const bool ok = applyEdit(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "New Sequence"), [&](const ProjectState& state) {
        auto result = edit::addSequence(state, settings);
        if (result)
            created = result->id;
        return result;
    });
    if (ok && created) {
        const ProjectSnapshot snap = m_session->snapshot();
        for (std::size_t i = 0; i < snap->sequences.size(); ++i) {
            if (snap->sequences[i]->id == created)
                setCurrentSequenceIndex(static_cast<int>(i));
        }
    }
}

void ProjectController::setProjectFile(std::filesystem::path file) {
    m_projectFile = std::move(file);
    refreshMedia();
}

bool ProjectController::loadProject(const QUrl& file) {
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    auto loaded = io::loadProject(std::filesystem::path(path.toStdU16String()));
    if (!loaded) {
        setError(QString::fromStdString(loaded.error().message));
        return false;
    }
    setError({});
    m_projectFile = std::filesystem::path(path.toStdU16String()); // media locators resolve relative to it
    resetProject(std::move(*loaded));
    return true;
}

// ---- Playhead ---------------------------------------------------------------------------------------

void ProjectController::setPlayhead(Ticks t) {
    const SequencePtr seq = currentSequence();
    const Rate rate = seq ? seq->frameRate : kRate30;
    const Ticks bounded = std::clamp(t, Ticks{}, contentDuration());
    // 0.2, Show Audio Time Units: the playhead stops on audio samples; the frame holding the sample is playhead().
    const bool samples = m_audioTimeUnits && seq && seq->audioRate.isValid();
    const Ticks fine = samples ? snapToFrame(bounded, seq->audioRate) : snapToFrame(bounded, rate);
    const Ticks clamped = samples ? snapToFrame(fine, rate) : fine;
    if (clamped == m_playhead && fine == m_finePlayhead)
        return;
    const bool frameChanged = clamped != m_playhead;
    m_playhead = clamped;
    m_finePlayhead = fine;
    emit playheadChanged();
    if (!frameChanged)
        return; // the same frame: nothing under the playhead changed
    followPlayheadSelection(); // 0.2: Selection Follows Playhead (off by default)
    // The clip editor shows parameter values at the playhead: refresh it when they can differ over time.
    if (seq) {
        for (StreamKind kind : {StreamKind::Video, StreamKind::Audio}) {
            const auto target = propertyTarget(kind);
            if (target && hasAnimatedParameters(seq->findClip(*target)->clip().properties())) {
                emit selectedClipChanged();
                break;
            }
        }
    }
}

int ProjectController::playheadMarkerIndex() const {
    if (const SequencePtr seq = currentSequence()) {
        for (std::size_t i = 0; i < seq->markers.size(); ++i) {
            if (snapToFrame(seq->markers[i].start, seq->frameRate) == m_playhead)
                return static_cast<int>(i);
        }
    }
    return -1;
}

void ProjectController::stepFrames(int frames) {
    const SequencePtr seq = currentSequence();
    if (!seq)
        return;
    if (m_audioTimeUnits && seq->audioRate.isValid()) {
        // Show Audio Time Units (Premiere): the arrow keys step by audio samples.
        setPlayhead(framesToTicks(ticksToFrames(m_finePlayhead, seq->audioRate) + frames, seq->audioRate));
        return;
    }
    setPlayhead(framesToTicks(ticksToFrames(m_playhead, seq->frameRate) + frames, seq->frameRate));
}

void ProjectController::setAudioTimeUnits(bool on) {
    if (on == m_audioTimeUnits)
        return;
    m_audioTimeUnits = on;
    emit audioTimeUnitsChanged();
    if (!on && m_finePlayhead != m_playhead) {
        m_finePlayhead = m_playhead; // back on the frame
        emit playheadChanged();
    } else {
        emit playheadChanged(); // playheadText changes its units
    }
}

QString ProjectController::playheadText() const {
    return m_audioTimeUnits ? formatAudioTime(m_finePlayhead) : formatTimecode(m_playhead);
}

QString ProjectController::formatAudioTime(Ticks t) const {
    const SequencePtr seq = currentSequence();
    if (!seq)
        return formatTimecode(t);
    return QString::fromStdString(edit::formatSequenceAudioTime(*seq, t));
}

void ProjectController::goToStart() {
    setPlayhead({});
}

void ProjectController::goToEnd() {
    const SequencePtr seq = currentSequence();
    setPlayhead(seq ? seq->contentEnd() : Ticks{});
}

std::vector<Ticks> ProjectController::editPoints(bool anyTrack) const {
    std::vector<Ticks> points{Ticks{}};
    if (const SequencePtr seq = currentSequence()) {
        // Premiere: Up/Down use the targeted tracks (all tracks when none is targeted); Shift+Up/Down any track.
        bool anyTargeted = false;
        seq->forEachTrack([&](const Track& t) { anyTargeted = anyTargeted || t.targeted; });
        seq->forEachTrack([&](const Track& t) {
            if (!anyTrack && anyTargeted && !t.targeted)
                return;
            for (const Clip& c : t.clips) {
                points.push_back(c.start());
                points.push_back(c.end());
            }
        });
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
}

void ProjectController::goToPreviousEdit(bool anyTrack) {
    const std::vector<Ticks> points = editPoints(anyTrack);
    const auto it = std::lower_bound(points.begin(), points.end(), m_playhead);
    setPlayhead(it == points.begin() ? Ticks{} : *(it - 1));
}

void ProjectController::goToNextEdit(bool anyTrack) {
    const std::vector<Ticks> points = editPoints(anyTrack);
    const auto it = std::upper_bound(points.begin(), points.end(), m_playhead);
    if (it != points.end())
        setPlayhead(*it);
}

void ProjectController::goToSelectedClipStart() {
    const SequencePtr seq = currentSequence();
    if (!seq || !m_primary)
        return;
    if (const auto loc = seq->findClip(m_primary))
        setPlayhead(loc->clip().start());
}

void ProjectController::goToSelectedClipEnd() {
    const SequencePtr seq = currentSequence();
    if (!seq || !m_primary)
        return;
    if (const auto loc = seq->findClip(m_primary))
        setPlayhead(loc->clip().end());
}

// ---- Sequence marks ---------------------------------------------------------------------------------

std::optional<Ticks> ProjectController::markIn() const {
    const SequencePtr seq = currentSequence();
    return seq ? seq->markIn : std::nullopt;
}

std::optional<Ticks> ProjectController::markOut() const {
    const SequencePtr seq = currentSequence();
    return seq ? seq->markOut : std::nullopt;
}

QString ProjectController::markInText() const {
    const auto in = markIn();
    return in ? formatTimecode(*in) : QString();
}

// The Out mark is exclusive (the end of the last marked frame); the UI names that last frame, as Premiere does.
Ticks ProjectController::lastMarkedFrame() const {
    const SequencePtr seq = currentSequence();
    if (!seq || !seq->markOut)
        return {};
    const std::int64_t frame = ticksToFrames(*seq->markOut, seq->frameRate);
    return framesToTicks(std::max<std::int64_t>(0, frame - 1), seq->frameRate);
}

QString ProjectController::markOutText() const {
    return markOut() ? formatTimecode(lastMarkedFrame()) : QString();
}

void ProjectController::markInAtPlayhead() {
    const SequenceId seq = m_sequence;
    const Ticks at = m_playhead;
    applyEdit(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Mark In"),
              [&](const ProjectState& state) { return edit::markIn(state, seq, at); });
}

void ProjectController::markOutAtPlayhead() {
    const SequenceId seq = m_sequence;
    const Ticks at = m_playhead;
    applyEdit(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Mark Out"),
              [&](const ProjectState& state) { return edit::markOut(state, seq, at); });
}

void ProjectController::markClipAtPlayhead() {
    const SequenceId seq = m_sequence;
    const Ticks at = m_playhead;
    applyEdit(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Mark Clip"),
              [&](const ProjectState& state) { return edit::markClip(state, seq, at); });
}

void ProjectController::markSelection() {
    if (m_selection.empty())
        return;
    const SequenceId seq = m_sequence;
    const std::vector<ClipId> clips = m_selection;
    applyEdit(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Mark Selection"),
              [&](const ProjectState& state) { return edit::markSelection(state, seq, clips); });
}

void ProjectController::clearMarks(bool clearIn, bool clearOut) {
    if ((!clearIn || !markIn()) && (!clearOut || !markOut()))
        return; // nothing to clear: no empty undo step
    const SequenceId seq = m_sequence;
    const char* label = clearIn && clearOut ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Clear In and Out")
                      : clearIn             ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Clear In")
                                            : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Clear Out");
    applyEdit(label, [&](const ProjectState& state) { return edit::clearMarks(state, seq, clearIn, clearOut); });
}

void ProjectController::goToMarkIn() {
    if (const auto in = markIn())
        setPlayhead(*in);
}

void ProjectController::goToMarkOut() {
    if (markOut())
        setPlayhead(lastMarkedFrame()); // Premiere parks on the last marked frame
}

// ---- Keymap commands --------------------------------------------------------------------------------

namespace {
using CommandAction = void (*)(ProjectController&);
const QHash<QString, CommandAction>& commandActions();
} // namespace

bool ProjectController::handlesCommand(const QString& id) {
    return commandActions().contains(id);
}

bool ProjectController::runCommand(const QString& id) {
    const auto& actions = commandActions();
    const auto it = actions.constFind(id);
    if (it == actions.constEnd())
        return false;
    (*it)(*this);
    return true;
}

namespace {
const QHash<QString, CommandAction>& commandActions() {
    using Action = CommandAction;
    static const QHash<QString, Action> kActions{
        {QStringLiteral("edit.undo"), [](ProjectController& c) { c.undo(); }},
        {QStringLiteral("edit.redo"), [](ProjectController& c) { c.redo(); }},
        {QStringLiteral("edit.selectAll"), [](ProjectController& c) { c.selectAll(); }},
        {QStringLiteral("edit.deselectAll"), [](ProjectController& c) { c.clearSelection(); }},
        {QStringLiteral("file.newSequence"), [](ProjectController& c) { c.newSequence(); }},
        {QStringLiteral("transport.stepBackward"), [](ProjectController& c) { c.stepFrames(-1); }},
        {QStringLiteral("transport.stepForward"), [](ProjectController& c) { c.stepFrames(1); }},
        {QStringLiteral("transport.stepBackwardFive"), [](ProjectController& c) { c.stepFrames(-5); }},
        {QStringLiteral("transport.stepForwardFive"), [](ProjectController& c) { c.stepFrames(5); }},
        {QStringLiteral("transport.goToPreviousEdit"), [](ProjectController& c) { c.goToPreviousEdit(false); }},
        {QStringLiteral("transport.goToNextEdit"), [](ProjectController& c) { c.goToNextEdit(false); }},
        {QStringLiteral("transport.goToPreviousEditAnyTrack"), [](ProjectController& c) { c.goToPreviousEdit(true); }},
        {QStringLiteral("transport.goToNextEditAnyTrack"), [](ProjectController& c) { c.goToNextEdit(true); }},
        {QStringLiteral("transport.goToStart"), [](ProjectController& c) { c.goToStart(); }},
        {QStringLiteral("transport.goToEnd"), [](ProjectController& c) { c.goToEnd(); }},
        {QStringLiteral("transport.goToSelectedClipStart"), [](ProjectController& c) { c.goToSelectedClipStart(); }},
        {QStringLiteral("transport.goToSelectedClipEnd"), [](ProjectController& c) { c.goToSelectedClipEnd(); }},
        {QStringLiteral("marker.markIn"), [](ProjectController& c) { c.markInAtPlayhead(); }},
        {QStringLiteral("marker.markOut"), [](ProjectController& c) { c.markOutAtPlayhead(); }},
        {QStringLiteral("marker.markClip"), [](ProjectController& c) { c.markClipAtPlayhead(); }},
        {QStringLiteral("marker.markSelection"), [](ProjectController& c) { c.markSelection(); }},
        {QStringLiteral("marker.clearIn"), [](ProjectController& c) { c.clearMarks(true, false); }},
        {QStringLiteral("marker.clearOut"), [](ProjectController& c) { c.clearMarks(false, true); }},
        {QStringLiteral("marker.clearInAndOut"), [](ProjectController& c) { c.clearMarks(true, true); }},
        {QStringLiteral("marker.goToIn"), [](ProjectController& c) { c.goToMarkIn(); }},
        {QStringLiteral("marker.goToOut"), [](ProjectController& c) { c.goToMarkOut(); }},
        // Editing (M5b)
        // Delete with a transition selected removes the transition (Premiere).
        {QStringLiteral("edit.clear"), [](ProjectController& c) { if (!c.deleteSelectedTransition()) c.deleteSelection(); }},
        {QStringLiteral("timeline.clearSelection"), [](ProjectController& c) { if (!c.deleteSelectedTransition()) c.deleteSelection(); }},
        {QStringLiteral("edit.rippleDelete"), [](ProjectController& c) { if (!c.deleteSelectedTransition()) c.rippleDeleteSelection(); }},
        {QStringLiteral("timeline.rippleDelete"), [](ProjectController& c) { if (!c.deleteSelectedTransition()) c.rippleDeleteSelection(); }},
        {QStringLiteral("sequence.addEdit"), [](ProjectController& c) { c.razorAtPlayhead(false); }},
        {QStringLiteral("sequence.addEditToAllTracks"), [](ProjectController& c) { c.razorAtPlayhead(true); }},
        {QStringLiteral("sequence.rippleTrimPreviousToPlayhead"), [](ProjectController& c) { c.rippleTrimPreviousToPlayhead(); }},
        {QStringLiteral("sequence.rippleTrimNextToPlayhead"), [](ProjectController& c) { c.rippleTrimNextToPlayhead(); }},
        {QStringLiteral("sequence.extendEditToPlayhead"), [](ProjectController& c) { c.extendEditToPlayhead(); }},
        {QStringLiteral("sequence.selectClipAtPlayhead"), [](ProjectController& c) { c.selectClipAtPlayhead(); }},
        {QStringLiteral("clip.link"), [](ProjectController& c) { c.toggleLinkSelection(); }},
        {QStringLiteral("clip.enable"), [](ProjectController& c) { c.toggleEnabledSelection(); }},
        // 0.2 timeline parity (ProjectControllerTimeline.cpp)
        {QStringLiteral("clip.group"), [](ProjectController& c) { c.groupSelection(); }},
        {QStringLiteral("clip.ungroup"), [](ProjectController& c) { c.ungroupSelection(); }},
        {QStringLiteral("sequence.selectionFollowsPlayhead"),
         [](ProjectController& c) { c.setSelectionFollowsPlayhead(!c.selectionFollowsPlayhead()); }},
        {QStringLiteral("sequence.nextInSequence"), [](ProjectController& c) { c.goToSequenceGap(true); }},
        {QStringLiteral("sequence.previousInSequence"), [](ProjectController& c) { c.goToSequenceGap(false); }},
        {QStringLiteral("timeline.selectNextClip"), [](ProjectController& c) { c.selectNextClip(1); }},
        {QStringLiteral("timeline.selectPreviousClip"), [](ProjectController& c) { c.selectNextClip(-1); }},
        {QStringLiteral("edit.copy"), [](ProjectController& c) { c.copySelection(); }},
        {QStringLiteral("edit.cut"), [](ProjectController& c) { c.cutSelection(); }},
        {QStringLiteral("edit.paste"), [](ProjectController& c) { c.paste(); }},
        {QStringLiteral("edit.pasteInsert"), [](ProjectController& c) { c.pasteInsert(); }},
        {QStringLiteral("timeline.nudgeClipLeft"), [](ProjectController& c) { c.nudgeSelection(-1); }},
        {QStringLiteral("timeline.nudgeClipRight"), [](ProjectController& c) { c.nudgeSelection(1); }},
        {QStringLiteral("timeline.nudgeClipLeftFive"), [](ProjectController& c) { c.nudgeSelection(-5); }},
        {QStringLiteral("timeline.nudgeClipRightFive"), [](ProjectController& c) { c.nudgeSelection(5); }},
        {QStringLiteral("timeline.slideClipLeft"), [](ProjectController& c) { c.slideSelection(-1); }},
        {QStringLiteral("timeline.slideClipRight"), [](ProjectController& c) { c.slideSelection(1); }},
        {QStringLiteral("timeline.slideClipLeftFive"), [](ProjectController& c) { c.slideSelection(-5); }},
        {QStringLiteral("timeline.slideClipRightFive"), [](ProjectController& c) { c.slideSelection(5); }},
        {QStringLiteral("timeline.slipClipLeft"), [](ProjectController& c) { c.slipSelection(-1); }},
        {QStringLiteral("timeline.slipClipRight"), [](ProjectController& c) { c.slipSelection(1); }},
        {QStringLiteral("timeline.slipClipLeftFive"), [](ProjectController& c) { c.slipSelection(-5); }},
        {QStringLiteral("timeline.slipClipRightFive"), [](ProjectController& c) { c.slipSelection(5); }},
        // Finishing (M5d)
        {QStringLiteral("sequence.applyVideoTransition"), [](ProjectController& c) { c.applyDefaultTransition(false); }},
        {QStringLiteral("sequence.applyAudioTransition"), [](ProjectController& c) { c.applyDefaultTransition(true); }},
        {QStringLiteral("sequence.applyDefaultTransitions"), [](ProjectController& c) { c.applyDefaultTransitionsToSelection(); }},
    };
    return kActions;
}
} // namespace

void ProjectController::seekToMarker(int index) {
    const SequencePtr seq = currentSequence();
    if (seq && index >= 0 && index < static_cast<int>(seq->markers.size()))
        setPlayhead(seq->markers[static_cast<std::size_t>(index)].start);
}

// ---- Selection --------------------------------------------------------------------------------------

bool ProjectController::isSelected(ClipId id) const {
    return std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end();
}

void ProjectController::setSelection(std::vector<ClipId> clips, ClipId primary) {
    std::vector<ClipId> unique;
    std::unordered_set<std::uint64_t> seen;
    unique.reserve(clips.size());
    for (ClipId id : clips) {
        if (id && seen.insert(id.value).second)
            unique.push_back(id);
    }
    const ClipId nextPrimary = primary && std::find(unique.begin(), unique.end(), primary) != unique.end()
                                   ? primary
                                   : (unique.empty() ? ClipId{} : unique.front());
    if (unique == m_selection && nextPrimary == m_primary)
        return;
    m_selection = std::move(unique);
    if (nextPrimary != m_primary)
        setSelectedEdge({}); // a new clip starts without a chosen edge
    m_primary = nextPrimary;
    if (!m_selection.empty() && m_selectedTransition) {
        m_selectedTransition = {}; // clips and transitions are selected one or the other (Premiere)
        emit selectedTransitionChanged();
    }
    emit selectionChanged();
    emit selectedClipChanged();
}

void ProjectController::selectClipById(ClipId clip, SelectMode mode, bool followLinks) {
    const SequencePtr seq = currentSequence();
    if (!seq || !seq->findClip(clip))
        return;
    std::vector<ClipId> group{clip};
    if (followLinks) {
        const ClipId ids[] = {clip};
        group = edit::withGroupedClips(*seq, ids); // its links, and its group's clips (0.2)
    }
    std::vector<ClipId> next = mode == Replace ? std::vector<ClipId>{} : m_selection;
    if (mode == Toggle && isSelected(clip)) {
        std::erase_if(next, [&](ClipId id) { return std::find(group.begin(), group.end(), id) != group.end(); });
        setSelection(std::move(next), m_primary == clip ? ClipId{} : m_primary);
        return;
    }
    for (ClipId id : group) {
        if (std::find(next.begin(), next.end(), id) == next.end())
            next.push_back(id);
    }
    setSelection(std::move(next), clip);
}

QStringList ProjectController::selectedClips() const {
    QStringList keys;
    for (ClipId id : m_selection)
        keys << clipKey(id);
    return keys;
}

void ProjectController::selectClip(const QString& clipId, int mode, bool followLinks) {
    if (const auto id = parseClipKey(clipId))
        selectClipById(*id, static_cast<SelectMode>(std::clamp(mode, 0, 2)), followLinks);
}

void ProjectController::selectAll() {
    const SequencePtr seq = currentSequence();
    if (!seq)
        return;
    std::vector<ClipId> all;
    seq->forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips)
            all.push_back(c.id);
    });
    setSelection(std::move(all), m_primary);
}

void ProjectController::clearSelection() {
    setSelection({});
}

QString ProjectController::clipName(const QString& clipId) const {
    const auto id = parseClipKey(clipId);
    const SequencePtr seq = currentSequence();
    if (!id || !seq)
        return {};
    const auto loc = seq->findClip(*id);
    return loc ? QString::fromStdString(loc->clip().name()) : QString();
}

std::optional<ClipId> ProjectController::propertyTarget(StreamKind kind) const {
    const SequencePtr seq = currentSequence();
    if (!seq || !m_primary)
        return std::nullopt;
    const auto loc = seq->findClip(m_primary);
    if (!loc)
        return std::nullopt;
    if (loc->clip().stream.kind == kind)
        return m_primary;
    const ClipId ids[] = {m_primary};
    for (ClipId partner : edit::withLinkedClips(*seq, ids)) {
        const auto p = seq->findClip(partner);
        if (p && p->clip().stream.kind == kind)
            return partner;
    }
    return std::nullopt;
}

QVariantMap ProjectController::selectedClip() const {
    const SequencePtr seq = currentSequence();
    if (!seq || !m_primary)
        return {};
    const auto loc = seq->findClip(m_primary);
    if (!loc)
        return {};
    const Clip& clip = loc->clip();

    QString trackName;
    if (const auto pos = seq->trackPosition(loc->track->id))
        trackName = shortTrackName(pos->kind, static_cast<int>(pos->index));

    const auto videoTarget = propertyTarget(StreamKind::Video);
    const auto audioTarget = propertyTarget(StreamKind::Audio);
    const PropertyFrame frame = propertyFrame(videoTarget ? seq->findClip(*videoTarget)->clip() : clip);
    // Video parameters come from the video clip, Volume/Pan from its linked audio clip, both evaluated at the
    // playhead (held inside each clip) so animated parameters show their current value.
    QVariantMap properties = toUi(ClipProperties{}, frame);
    const auto merge = [&](ClipId target, bool audio) {
        const Clip& c = seq->findClip(target)->clip();
        const QVariantMap values = toUi(c.properties(), frame, curveTimeAtPlayhead(c));
        for (const UiParameter& p : uiParameters()) {
            if ((parameterInfo(p.id).role == PropertyRole::Audio) == audio)
                properties[QLatin1String(p.key)] = values[QLatin1String(p.key)];
        }
    };
    if (videoTarget)
        merge(*videoTarget, false);
    if (audioTarget)
        merge(*audioTarget, true);

    return {
        {QStringLiteral("id"), clipKey(clip.id)},
        {QStringLiteral("name"), QString::fromStdString(clip.name())},
        {QStringLiteral("trackName"), trackName},
        {QStringLiteral("rangeText"), formatTimecode(clip.start()) + QStringLiteral(" → ") + formatTimecode(clip.end())},
        {QStringLiteral("colorIndex"), clip.colorIndex},
        {QStringLiteral("hasVideo"), videoTarget.has_value()},
        {QStringLiteral("hasAudio"), audioTarget.has_value()},
        {QStringLiteral("properties"), properties},
        {QStringLiteral("defaults"), toUi(ClipProperties{}, frame)},
    };
}

// ---- Edits ------------------------------------------------------------------------------------------

QString ProjectController::undoText() const {
    const auto label = m_session->undoLabel();
    return label ? tr("Undo %1").arg(tr(label->c_str())) : tr("Undo");
}

QString ProjectController::redoText() const {
    const auto label = m_session->redoLabel();
    return label ? tr("Redo %1").arg(tr(label->c_str())) : tr("Redo");
}

bool ProjectController::undo() {
    return m_session->undo().has_value();
}

bool ProjectController::redo() {
    return m_session->redo().has_value();
}

void ProjectController::setTrackFlag(const QString& trackId, const QString& flag, bool value) {
    const auto track = parseTrackKey(trackId);
    if (!track || !m_sequence)
        return;
    edit::TrackStateChange change;
    const char* label = nullptr;
    if (flag == QLatin1String("enabled")) {
        change.enabled = value;
        label = QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Toggle Track Output");
    } else if (flag == QLatin1String("locked")) {
        change.locked = value;
        label = value ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Lock Track") : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Unlock Track");
    } else if (flag == QLatin1String("muted")) {
        change.muted = value;
        label = QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Mute Track");
    } else if (flag == QLatin1String("solo")) {
        change.solo = value;
        label = QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Solo Track");
    } else if (flag == QLatin1String("targeted")) {
        change.targeted = value;
        label = QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Target Track");
    } else if (flag == QLatin1String("syncLocked")) {
        change.syncLocked = value;
        label = QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Toggle Sync Lock");
    } else {
        return;
    }
    const SequenceId seq = m_sequence;
    applyEdit(label, [&](const ProjectState& state) { return edit::setTrackState(state, seq, *track, change); });
}

void ProjectController::setClipProperty(const QString& key, double value) {
    const UiParameter* p = uiParameter(key);
    if (!p || !std::isfinite(value))
        return;
    const SequencePtr seq = currentSequence();
    const auto target = propertyTarget(isAudioProperty(key) ? StreamKind::Audio : StreamKind::Video);
    if (!seq || !target)
        return;
    const Clip& clip = seq->findClip(*target)->clip();
    const ParameterInfo& info = parameterInfo(p->id);
    const double model = std::clamp(p->range.toModel(value, propertyFrame(clip)), info.minValue, info.maxValue);
    const std::string mergeKey = "clip-property:" + std::to_string(target->value) + ":" + key.toStdString();
    setParameterAtPlayhead(*target, edit::ParamRef::of(p->id), model, p->undoLabel, mergeKey);
}

void ProjectController::endPropertyGesture() {
    m_session->endMerge();
}

} // namespace wabcut::app
