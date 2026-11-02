#include "AuditionsBridge.h"

#include "BridgeIds.h"
#include "NestedPictures.h"
#include "PlaybackBridge.h"
#include "PosterFrames.h"
#include "ProjectController.h"
#include "SourceMonitorBridge.h"

#include <wabcut/model/Auditions.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/RedactionMedia.h>

#include <QQmlEngine>
#include <QSettings>

#include <algorithm>
#include <filesystem>

namespace wabcut::app {

namespace {

constexpr const char* kOverlayKey = "auditions/programOverlay";

const char* const kCommands[] = {
    "audition.create",       "audition.addSelected", "audition.addSource",  "audition.duplicateTake",
    "audition.nextTake",     "audition.previousTake", "audition.flatten",   "audition.toggleOverlay",
    "audition.showTakeSelector",
};

/// The clip of `seq` with `id`, or null.
const Clip* clipIn(const Sequence& seq, ClipId id) {
    const auto loc = id.isValid() ? seq.findClip(id) : std::nullopt;
    return loc ? &loc->clip() : nullptr;
}

} // namespace

AuditionsBridge::AuditionsBridge(QObject* parent) : QObject(parent) {
    m_overlay = QSettings().value(QLatin1String(kOverlayKey), false).toBool();
}

AuditionsBridge::~AuditionsBridge() = default;

AuditionsBridge* AuditionsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new AuditionsBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setPlayback(qmlEngine->singletonInstance<PlaybackBridge*>("Wabcut", "Playback"));
        bridge->setSourceMonitor(qmlEngine->singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor"));
    }
    return bridge;
}

void AuditionsBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::revisionChanged, this, [this] {
            // An edit, undo or another project took the preview back (EditSession ends it before its own work): the
            // take is not playing in the audition's place any more.
            if (m_preview && m_preview->shown && !m_preview->kept && m_controller && !m_controller->session().inPreview()) {
                m_preview.reset();
                emit previewChanged();
            }
            refresh(true);
        });
        connect(controller, &ProjectController::sequenceChanged, this, [this] { refresh(true); });
        connect(controller, &ProjectController::selectionChanged, this, [this] { refresh(true); });
        connect(controller, &ProjectController::playheadChanged, this, [this] { refresh(false); });
    }
    refresh(true);
}

void AuditionsBridge::setPlayback(PlaybackBridge* playback) {
    if (m_playback == playback)
        return;
    if (m_playback)
        disconnect(m_playback, nullptr, this, nullptr);
    m_playback = playback;
    if (playback)
        connect(playback, &PlaybackBridge::transportChanged, this, &AuditionsBridge::onTransportChanged);
}

void AuditionsBridge::setSourceMonitor(SourceMonitorBridge* source) {
    if (m_source == source)
        return;
    if (m_source)
        disconnect(m_source, nullptr, this, nullptr);
    m_source = source;
    if (source)
        connect(source, &SourceMonitorBridge::sourceChanged, this, &AuditionsBridge::changed);
}

ClipId AuditionsBridge::resolveClip() const {
    if (!m_controller)
        return {};
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return {};
    if (m_preview) // a take being auditioned stays shown
        return clipIn(*seq, m_preview->clip) ? m_preview->clip : ClipId{};
    const auto isAudition = [&](ClipId id) {
        const Clip* c = clipIn(*seq, id);
        return c && c->isAudition();
    };
    if (isAudition(m_controller->primaryClip()))
        return m_controller->primaryClip();
    for (const ClipId id : m_controller->selection()) {
        if (isAudition(id))
            return id;
    }
    // Under the playhead: the topmost video track holding an audition, then the audio tracks from A1.
    const Ticks at = m_controller->playhead();
    const auto atPlayhead = [&](const TrackPtr& t) -> ClipId {
        if (const auto i = t->clipIndexAt(at)) {
            const Clip& c = t->clips[*i];
            if (c.isAudition())
                return c.id;
        }
        return {};
    };
    for (auto it = seq->videoTracks.rbegin(); it != seq->videoTracks.rend(); ++it) {
        if (const ClipId id = atPlayhead(*it); id.isValid())
            return id;
    }
    for (const TrackPtr& t : seq->audioTracks) {
        if (const ClipId id = atPlayhead(t); id.isValid())
            return id;
    }
    return {};
}

void AuditionsBridge::refresh(bool force) {
    const ClipId next = resolveClip();
    if (!force && next == m_clip)
        return;
    m_clip = next;
    emit changed();
}

QString AuditionsBridge::clip() const { return m_clip.isValid() ? clipKey(m_clip) : QString(); }

namespace {

/// The shown audition clip in the current snapshot (kept alive by `seq`), or null.
const Clip* shownClip(const ProjectController* controller, ClipId id, SequencePtr& seq) {
    if (!controller || !id.isValid())
        return nullptr;
    seq = controller->currentSequence();
    return seq ? clipIn(*seq, id) : nullptr;
}

} // namespace

QString AuditionsBridge::name() const {
    SequencePtr seq;
    const Clip* c = shownClip(m_controller, m_clip, seq);
    return c ? QString::fromStdString(c->name()) : QString();
}

QString AuditionsBridge::badge() const {
    SequencePtr seq;
    const Clip* c = shownClip(m_controller, m_clip, seq);
    if (!c || !c->isAudition())
        return {};
    return tr("Take %1 of %2").arg(c->audition()->active + 1).arg(c->audition()->size());
}

int AuditionsBridge::count() const {
    SequencePtr seq;
    const Clip* c = shownClip(m_controller, m_clip, seq);
    return c ? static_cast<int>(takeCount(*c)) : 0;
}

int AuditionsBridge::activeIndex() const {
    SequencePtr seq;
    const Clip* c = shownClip(m_controller, m_clip, seq);
    return c && c->isAudition() ? static_cast<int>(c->audition()->active) : 0;
}

QString AuditionsBridge::kindText() const {
    SequencePtr seq;
    const Clip* c = shownClip(m_controller, m_clip, seq);
    if (!c)
        return {};
    bool video = false;
    bool audio = false;
    for (const ClipId id : auditionPartners(*seq, c->id)) {
        if (const Clip* p = clipIn(*seq, id))
            (p->stream.kind == StreamKind::Video ? video : audio) = true;
    }
    return video && audio ? tr("Picture and sound") : video ? tr("Picture") : tr("Sound");
}

QString AuditionsBridge::rangeText() const {
    SequencePtr seq;
    const Clip* c = shownClip(m_controller, m_clip, seq);
    if (!c)
        return {};
    const TimeRange whole = auditionRange(*c);
    return tr("%1 - %2 (%3)")
        .arg(m_controller->formatTimecode(whole.start), m_controller->formatTimecode(whole.end()),
             m_controller->formatTimecode(whole.duration));
}

QVariantList AuditionsBridge::takes() const {
    QVariantList out;
    SequencePtr seq;
    const Clip* c = shownClip(m_controller, m_clip, seq);
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    if (!c || !snap)
        return out;
    const TimeRange whole = auditionRange(*c);
    const std::size_t active = c->isAudition() ? c->audition()->active : 0;
    for (std::size_t i = 0; i < takeCount(*c); ++i) {
        const Clip take = auditionTakeClip(*c, i);
        const TakeFit fit = takeFit(*snap, *c, i, seq->frameRate);
        const Ticks covered = std::max(Ticks{}, whole.duration - fit.head - fit.tail);
        QString kind = take.stream.kind == StreamKind::Audio ? QStringLiteral("audio") : QStringLiteral("video");
        QString source;
        QString thumbnail;
        if (take.isNested()) {
            if (const Sequence* nested = snap->findSequence(take.sequence)) {
                source = QString::fromStdString(nested->name);
                if (nested->multicam) {
                    kind = QStringLiteral("multicam");
                    const std::size_t angle = take.angles.first();
                    if (angle < nested->multicam->angles.size())
                        source += QStringLiteral(" · ") + QString::fromStdString(nested->multicam->angles[angle].name);
                } else {
                    kind = QStringLiteral("nested");
                }
                if (take.stream.kind == StreamKind::Video)
                    thumbnail = SequenceThumbnailProvider::urlFor(*snap, take.sequence);
            }
        } else if (const MediaItem* media = snap->findMedia(take.media)) {
            source = QString::fromStdString(media->name);
            if (media->isGraphic() || media->isTitle())
                kind = QStringLiteral("graphic");
            else if (take.stream.kind == StreamKind::Video && media->info.isStillImage)
                kind = QStringLiteral("still");
            // The take's first frame (in the audition), from the file: hidden when a source redaction covers it.
            const auto stream = static_cast<std::size_t>(std::max(0, take.stream.index));
            if (take.stream.kind == StreamKind::Video && !media->isGenerator() && stream < media->info.video.size()) {
                const Ticks first = take.sourceTimeAt(std::min(whole.start + fit.head, whole.end() - Ticks{1}));
                bool redacted = false;
                for (const Redaction* r : redactionsCovering(*snap, media->id))
                    redacted = redacted || (r->enabled && r->source.contains(first));
                const auto path = media->locator.offline ? std::nullopt
                                                         : resolveMediaPath(media->locator, m_controller->projectFile());
                if (path && !redacted)
                    thumbnail = PosterImageProvider::urlFor(QString::fromStdString(pathToUtf8(*path)),
                                                            std::max(Ticks{}, first), media->info.video[stream].index);
            }
        }
        out.append(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(i)},
            {QStringLiteral("name"), QString::fromStdString(take.name())},
            {QStringLiteral("active"), i == active},
            {QStringLiteral("source"), source},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("durationText"), m_controller->formatTimecode(covered)},
            {QStringLiteral("isShort"), !fit.fills()},
            {QStringLiteral("shortText"),
             fit.fills() ? QString() : tr("%1 short").arg(m_controller->formatTimecode(fit.head + fit.tail))},
            {QStringLiteral("effects"), static_cast<int>(take.effects().size())},
            {QStringLiteral("thumbnail"), thumbnail},
        });
    }
    return out;
}

bool AuditionsBridge::canCreate() const { return m_controller && !m_controller->selection().empty(); }

bool AuditionsBridge::canAddSelected() const {
    if (!m_controller)
        return false;
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq || !m_clip.isValid())
        return false;
    const std::vector<ClipId> own = auditionPartners(*seq, m_clip);
    return std::any_of(m_controller->selection().begin(), m_controller->selection().end(),
                       [&](ClipId id) { return std::find(own.begin(), own.end(), id) == own.end(); });
}

bool AuditionsBridge::canAddSource() const {
    return m_clip.isValid() && m_source && m_source->mediaId().isValid();
}

void AuditionsBridge::setOverlayShown(bool shown) {
    if (m_overlay == shown)
        return;
    m_overlay = shown;
    QSettings().setValue(QLatin1String(kOverlayKey), shown);
    emit overlayShownChanged();
}

void AuditionsBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged(); // every time: the same problem twice still shows its toast
}

bool AuditionsBridge::apply(const char* label, const std::function<edit::EditOutcome(const ProjectState&)>& fn) {
    if (!m_controller)
        return false;
    const bool ok = m_controller->applyEditFn(label, fn);
    if (!ok)
        setMessage(m_controller->lastError());
    refresh(true);
    return ok;
}

bool AuditionsBridge::createFromSelection() {
    if (!m_controller || m_controller->selection().empty()) {
        setMessage(tr("Select the clips to make an audition of"));
        return false;
    }
    endPreview();
    const SequenceId seq = m_controller->currentSequenceId();
    const std::vector<ClipId> clips = m_controller->selection();
    const bool ok = apply(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "New Audition"),
                          [&](const ProjectState& s) { return edit::createAudition(s, seq, clips); });
    if (ok) {
        // The audition is the clip that stayed: show and select it.
        if (const SequencePtr now = m_controller->currentSequence()) {
            for (const ClipId id : clips) {
                if (const Clip* c = clipIn(*now, id); c && c->isAudition()) {
                    m_controller->setSelection(edit::withLinkedClips(*now, std::vector<ClipId>{id}), id);
                    break;
                }
            }
        }
        refresh(true);
        setMessage(count() == 1 ? tr("New audition with 1 take") : tr("New audition with %1 takes").arg(count()));
    }
    return ok;
}

bool AuditionsBridge::addSelected() {
    if (!m_controller)
        return false;
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq || !m_clip.isValid()) {
        setMessage(tr("Select an audition and the clips to add to it"));
        return false;
    }
    endPreview();
    const ClipId audition = m_clip;
    const std::vector<ClipId> own = auditionPartners(*seq, audition);
    std::vector<ClipId> clips;
    for (const ClipId id : m_controller->selection()) {
        if (std::find(own.begin(), own.end(), id) == own.end())
            clips.push_back(id);
    }
    if (clips.empty()) {
        setMessage(tr("Select the clips to add to the audition"));
        return false;
    }
    const SequenceId seqId = seq->id;
    const int before = count();
    const bool ok = apply(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Add to Audition"),
                          [&](const ProjectState& s) { return edit::addAuditionTakes(s, seqId, audition, clips); });
    if (ok) {
        m_controller->setSelection(edit::withLinkedClips(*m_controller->currentSequence(), std::vector<ClipId>{audition}),
                                   audition);
        const int added = count() - before;
        setMessage(added == 1 ? tr("Added 1 take") : tr("Added %1 takes").arg(added));
    }
    return ok;
}

bool AuditionsBridge::addSource() {
    if (!m_clip.isValid()) {
        setMessage(tr("Select an audition first"));
        return false;
    }
    if (!m_source || !m_source->mediaId().isValid()) {
        setMessage(tr("Open a clip in the Source Monitor first"));
        return false;
    }
    endPreview();
    edit::TakeSource source;
    source.media = m_source->mediaId();
    // The Source Monitor's In (kept on the media item), else the source's start.
    if (const ProjectSnapshot snap = m_controller->snapshot()) {
        if (const MediaItem* item = snap->findMedia(source.media))
            source.sourceIn = edit::storedSourceMarks(*item).in.value_or(Ticks{});
    }
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId audition = m_clip;
    return apply(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Add Source to Audition"),
                 [&](const ProjectState& s) { return edit::addAuditionSource(s, seq, audition, source); });
}

bool AuditionsBridge::useTake(int index) {
    if (!m_clip.isValid() || index < 0)
        return false;
    if (m_preview && m_preview->take == index) {
        // The auditioned take stays when the preview ends: the preview goes and Use Take becomes a real undo step (an
        // edit takes the session's preview back first). The take keeps playing.
        m_preview->kept = true;
        if (!m_preview->shown)
            return true; // it was the active take already
    } else {
        endPreview();
    }
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId audition = m_clip;
    return apply(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Use Take"), [&](const ProjectState& s) {
        return edit::setActiveTake(s, seq, audition, static_cast<std::size_t>(index));
    });
}

bool AuditionsBridge::isShownAudition(const QString& clipKey) const {
    const std::optional<ClipId> id = parseClipKey(clipKey);
    if (!id || !m_clip.isValid() || !m_controller)
        return false;
    if (*id == m_clip)
        return true;
    const SequencePtr seq = m_controller->currentSequence();
    if (!seq)
        return false;
    const std::vector<ClipId> partners = auditionPartners(*seq, m_clip);
    return std::find(partners.begin(), partners.end(), *id) != partners.end();
}

bool AuditionsBridge::previewTake(int index) {
    if (!m_controller || !m_clip.isValid() || index < 0 || index >= count())
        return false;
    endPreview();
    const SequencePtr seq = m_controller->currentSequence();
    const Clip* c = seq ? clipIn(*seq, m_clip) : nullptr;
    if (!c)
        return false;
    Preview p;
    p.clip = m_clip;
    p.take = index;
    p.original = activeIndex();
    const TimeRange whole = auditionRange(*c);
    if (index != p.original) {
        // Shown as the session's preview (EditSession::showPreview): only what plays changes (EditSession::shown). No
        // undo step, the redo history stays, and everything else - this bridge's Active take and Next / Previous Take,
        // saves and autosaves, exports, commands - keeps reading the take that is really active (fix F3, NEW-02).
        EditSession& session = m_controller->session();
        auto switched = edit::setActiveTake(*session.committed(), seq->id, m_clip, static_cast<std::size_t>(index));
        if (!switched) {
            setMessage(QString::fromStdString(switched.error().message));
            return false;
        }
        if (auto shown = session.showPreview("Play Take", switched->state); !shown) {
            setMessage(QString::fromStdString(shown.error().message));
            return false;
        }
        p.shown = true;
    }
    m_preview = p;
    emit previewChanged();
    refresh(true);
    if (m_playback)
        m_playback->playRange(whole.start, whole.end());
    return true;
}

void AuditionsBridge::stopPreview() {
    if (!m_preview)
        return;
    if (m_playback && m_playback->playing())
        m_playback->pause(); // onTransportChanged ends the preview
    endPreview();
}

void AuditionsBridge::onTransportChanged() {
    if (m_preview && m_playback && !m_playback->playing())
        endPreview();
}

void AuditionsBridge::endPreview() {
    if (!m_preview)
        return;
    const Preview p = *m_preview;
    m_preview.reset();
    // The committed state comes back as it was (an edit meanwhile has ended the session's preview already).
    if (p.shown && !p.kept && m_controller)
        m_controller->session().endPreview();
    emit previewChanged();
    refresh(true);
}

bool AuditionsBridge::removeTake(int index) {
    if (!m_clip.isValid() || index < 0)
        return false;
    endPreview();
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId audition = m_clip;
    return apply(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Remove Take"), [&](const ProjectState& s) {
        return edit::removeAuditionTake(s, seq, audition, static_cast<std::size_t>(index));
    });
}

bool AuditionsBridge::moveTake(int from, int to) {
    if (!m_clip.isValid() || from < 0 || to < 0 || from == to)
        return false;
    endPreview();
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId audition = m_clip;
    return apply(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Move Take"), [&](const ProjectState& s) {
        return edit::moveAuditionTake(s, seq, audition, static_cast<std::size_t>(from), static_cast<std::size_t>(to));
    });
}

bool AuditionsBridge::renameTake(int index, const QString& name) {
    const QString trimmed = name.trimmed();
    if (!m_clip.isValid() || index < 0 || trimmed.isEmpty())
        return false;
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId audition = m_clip;
    const std::string text = trimmed.toStdString();
    return apply(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Rename Take"), [&](const ProjectState& s) {
        return edit::renameAuditionTake(s, seq, audition, static_cast<std::size_t>(index), text);
    });
}

bool AuditionsBridge::duplicateTake(int index) {
    if (!m_controller)
        return false;
    endPreview();
    const SequenceId seq = m_controller->currentSequenceId();
    ClipId audition = m_clip;
    if (!audition.isValid()) {
        // Duplicate as Audition on a plain clip: it becomes an audition with a copy of itself.
        audition = m_controller->primaryClip();
        if (!audition.isValid()) {
            setMessage(tr("Select a clip first"));
            return false;
        }
        const ClipId one = audition;
        return apply(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Duplicate Take"), [&](const ProjectState& s) {
            return edit::createAudition(s, seq, std::span(&one, 1));
        });
    }
    const std::size_t take = index < 0 ? static_cast<std::size_t>(activeIndex()) : static_cast<std::size_t>(index);
    return apply(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Duplicate Take"), [&](const ProjectState& s) {
        return edit::duplicateAuditionTake(s, seq, audition, take);
    });
}

bool AuditionsBridge::flatten() {
    if (!m_clip.isValid()) {
        setMessage(tr("Select an audition first"));
        return false;
    }
    endPreview();
    const SequenceId seq = m_controller->currentSequenceId();
    const ClipId audition = m_clip;
    return apply(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Flatten Audition"), [&](const ProjectState& s) {
        return edit::flattenAuditions(s, seq, std::span(&audition, 1));
    });
}

bool AuditionsBridge::nextTake() {
    const int n = count();
    return n > 1 && useTake((activeIndex() + 1) % n);
}

bool AuditionsBridge::previousTake() {
    const int n = count();
    return n > 1 && useTake((activeIndex() + n - 1) % n);
}

bool AuditionsBridge::handlesCommand(const QString& commandId) {
    return std::any_of(std::begin(kCommands), std::end(kCommands),
                       [&](const char* id) { return commandId == QLatin1String(id); });
}

bool AuditionsBridge::runCommand(const QString& commandId) {
    if (commandId == QLatin1String("audition.create"))
        return createFromSelection();
    if (commandId == QLatin1String("audition.addSelected"))
        return addSelected();
    if (commandId == QLatin1String("audition.addSource"))
        return addSource();
    if (commandId == QLatin1String("audition.duplicateTake"))
        return duplicateTake(-1);
    if (commandId == QLatin1String("audition.nextTake"))
        return nextTake();
    if (commandId == QLatin1String("audition.previousTake"))
        return previousTake();
    if (commandId == QLatin1String("audition.flatten"))
        return flatten();
    if (commandId == QLatin1String("audition.toggleOverlay")) {
        setOverlayShown(!m_overlay);
        return true;
    }
    return false; // audition.showTakeSelector: Main opens the panel
}

QVariantMap AuditionsBridge::commandAvailability(const QString& commandId) const {
    if (!handlesCommand(commandId))
        return {};
    const auto result = [](bool enabled, const QString& reason) {
        return QVariantMap{{QStringLiteral("handled"), true},
                           {QStringLiteral("enabled"), enabled},
                           {QStringLiteral("reason"), enabled ? QString() : reason}};
    };
    if (commandId == QLatin1String("audition.create"))
        return result(canCreate(), tr("Select the clips to make an audition of"));
    if (commandId == QLatin1String("audition.addSelected"))
        return result(canAddSelected(), m_clip.isValid() ? tr("Select the clips to add to the audition")
                                                         : tr("Select an audition first"));
    if (commandId == QLatin1String("audition.addSource"))
        return result(canAddSource(), m_clip.isValid() ? tr("Open a clip in the Source Monitor first")
                                                       : tr("Select an audition first"));
    if (commandId == QLatin1String("audition.duplicateTake"))
        return result(m_clip.isValid() || (m_controller && m_controller->primaryClip().isValid()), tr("Select a clip first"));
    if (commandId == QLatin1String("audition.nextTake") || commandId == QLatin1String("audition.previousTake") ||
        commandId == QLatin1String("audition.flatten"))
        return result(m_clip.isValid(), tr("Select an audition first"));
    return result(true, {});
}

} // namespace wabcut::app
