#include "TrimBridge.h"

#include "BridgeIds.h"
#include "KeymapDispatcher.h"
#include "PlaybackBridge.h"
#include "ProgramToolsBridge.h"
#include "ProjectController.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/Timecode.h>

#include <QCoreApplication>
#include <QQmlEngine>
#include <QThreadPool>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace wabcut::app {

namespace {

constexpr int kLoopTickMs = 30;
constexpr int kTrimViewWidth = 480; // px: the Trim view's pictures

} // namespace

TrimBridge::TrimBridge(QObject* parent) : QObject(parent) {
    m_loop.setInterval(kLoopTickMs);
    connect(&m_loop, &QTimer::timeout, this, &TrimBridge::loopTick);
}

TrimBridge* TrimBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new TrimBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setPlayback(qmlEngine->singletonInstance<PlaybackBridge*>("Wabcut", "Playback"));
        bridge->setProgramTools(qmlEngine->singletonInstance<ProgramToolsBridge*>("Wabcut", "ProgramTools"));
    }
    return bridge;
}

void TrimBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    m_points.clear();
    if (controller) {
        connect(controller, &ProjectController::revisionChanged, this, &TrimBridge::onModelChanged);
        m_sequence = controller->currentSequenceId();
        connect(controller, &ProjectController::sequenceChanged, this, [this] {
            // Also emitted when the sequence's settings or length change: only a switch leaves trim mode.
            if (m_controller && m_controller->currentSequenceId() != m_sequence) {
                m_sequence = m_controller->currentSequenceId();
                clear();
            }
        });
        connect(controller, &ProjectController::selectionChanged, this, &TrimBridge::onSelectionChanged);
    }
    emit pointsChanged();
}

void TrimBridge::setPlayback(PlaybackBridge* playback) {
    m_playback = playback;
}

void TrimBridge::setProgramTools(ProgramToolsBridge* tools) {
    m_tools = tools;
}

// ---- State ----------------------------------------------------------------------------------------------------------

QVariantList TrimBridge::points() const {
    QVariantList list;
    for (const Point& p : m_points)
        list << QVariantMap{{QStringLiteral("clip"), clipKey(p.clip)},
                            {QStringLiteral("edge"), p.head ? QStringLiteral("head") : QStringLiteral("tail")}};
    return list;
}

QString TrimBridge::type() const {
    switch (m_type) {
    case Type::Regular:
        return QStringLiteral("regular");
    case Type::Ripple:
        return QStringLiteral("ripple");
    case Type::Roll:
        break;
    }
    return QStringLiteral("roll");
}

void TrimBridge::setType(const QString& type) {
    const Type t = type == QLatin1String("regular") ? Type::Regular
                 : type == QLatin1String("ripple")  ? Type::Ripple
                                                    : Type::Roll;
    if (t == m_type)
        return;
    m_type = t;
    emit pointsChanged();
}

void TrimBridge::setLargeOffset(int frames) {
    frames = std::clamp(frames, 1, 1000);
    if (frames == m_largeOffset)
        return;
    m_largeOffset = frames;
    emit settingsChanged();
}

void TrimBridge::setPreroll(double seconds) {
    if (!std::isfinite(seconds) || seconds == m_preroll)
        return;
    m_preroll = std::clamp(seconds, 0.0, 60.0);
    emit settingsChanged();
}

void TrimBridge::setPostroll(double seconds) {
    if (!std::isfinite(seconds) || seconds == m_postroll)
        return;
    m_postroll = std::clamp(seconds, 0.0, 60.0);
    emit settingsChanged();
}

void TrimBridge::setLinkedSelection(bool on) {
    if (on == m_linked)
        return;
    m_linked = on;
    emit settingsChanged();
}

std::optional<TrimBridge::Sides> TrimBridge::sidesOf(const Point& p) const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return std::nullopt;
    const auto loc = seq->findClip(p.clip);
    if (!loc)
        return std::nullopt;
    const Track& track = *loc->track;
    const Clip& clip = loc->clip();
    Sides sides;
    sides.track = track.id;
    if (p.head) {
        sides.incoming = &clip;
        sides.cut = clip.start();
        if (loc->index > 0 && track.clips[loc->index - 1].end() == clip.start())
            sides.outgoing = &track.clips[loc->index - 1];
    } else {
        sides.outgoing = &clip;
        sides.cut = clip.end();
        if (loc->index + 1 < track.clips.size() && track.clips[loc->index + 1].start() == clip.end())
            sides.incoming = &track.clips[loc->index + 1];
    }
    return sides;
}

std::optional<Ticks> TrimBridge::editTime() const {
    if (m_points.empty())
        return std::nullopt;
    const auto sides = sidesOf(m_points.front());
    return sides ? std::optional(sides->cut) : std::nullopt;
}

QString TrimBridge::outgoingName() const {
    const auto sides = m_points.empty() ? std::nullopt : sidesOf(m_points.front());
    return sides && sides->outgoing ? QString::fromStdString(sides->outgoing->name()) : QString();
}

QString TrimBridge::incomingName() const {
    const auto sides = m_points.empty() ? std::nullopt : sidesOf(m_points.front());
    return sides && sides->incoming ? QString::fromStdString(sides->incoming->name()) : QString();
}

QString TrimBridge::outgoingText() const {
    const auto sides = m_points.empty() ? std::nullopt : sidesOf(m_points.front());
    if (!sides || !sides->outgoing || !m_controller)
        return {};
    // The outgoing clip's last frame, in source time.
    const Clip& c = *sides->outgoing;
    return m_controller->formatTimecode(c.sourceTimeAt(std::max(c.start(), c.end() - m_controller->frameTicks())));
}

QString TrimBridge::incomingText() const {
    const auto sides = m_points.empty() ? std::nullopt : sidesOf(m_points.front());
    if (!sides || !sides->incoming || !m_controller)
        return {};
    return m_controller->formatTimecode(sides->incoming->sourceTimeAt(sides->incoming->start()));
}

QString TrimBridge::editText() const {
    const auto t = editTime();
    return t && m_controller ? m_controller->formatTimecode(*t) : QString();
}

void TrimBridge::setPoints(std::vector<Point> points) {
    const bool was = active();
    m_points = std::move(points);
    if (!was && active()) {
        m_outOffset = 0;
        m_inOffset = 0;
    }
    if (!active()) {
        m_viewOpen = false;
        if (m_dynamic) {
            m_dynamic = false;
            emit dynamicChanged();
        }
        stopAround();
    }
    emit pointsChanged();
    emit detailsChanged();
}

std::vector<TrimBridge::Point> TrimBridge::withLinkedEdges(std::vector<Point> points) const {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq || !m_linked)
        return points;
    std::vector<Point> out = points;
    std::unordered_set<std::uint64_t> tracks;
    for (const Point& p : points) {
        if (const auto loc = seq->findClip(p.clip))
            tracks.insert(loc->track->id.value);
    }
    for (const Point& p : points) {
        const auto loc = seq->findClip(p.clip);
        if (!loc)
            continue;
        const Ticks cut = p.head ? loc->clip().start() : loc->clip().end();
        const ClipId self[] = {p.clip};
        for (const ClipId partner : edit::withLinkedClips(*seq, self)) {
            const auto ploc = seq->findClip(partner);
            if (!ploc || partner == p.clip || tracks.contains(ploc->track->id.value))
                continue;
            const Clip& c = ploc->clip();
            if (p.head ? c.start() == cut : c.end() == cut) {
                out.push_back({partner, p.head});
                tracks.insert(ploc->track->id.value);
            }
        }
    }
    return out;
}

// ---- Selecting edit points --------------------------------------------------------------------------------------------

bool TrimBridge::trimEdit() {
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq)
        return false;
    std::vector<const Track*> tracks;
    seq->forEachTrack([&](const Track& t) {
        if (t.targeted)
            tracks.push_back(&t);
    });
    if (tracks.empty())
        seq->forEachTrack([&](const Track& t) { tracks.push_back(&t); });
    // The cut nearest the playhead on those tracks.
    const Ticks playhead = m_controller->playhead();
    std::optional<Ticks> best;
    for (const Track* t : tracks) {
        for (const Clip& c : t->clips) {
            for (const Ticks cut : {c.start(), c.end()}) {
                if (!best || std::abs((cut - playhead).value) < std::abs((*best - playhead).value))
                    best = cut;
            }
        }
    }
    if (!best)
        return false;
    // One edit point per track at that cut: the outgoing clip's tail, else the incoming clip's head.
    std::vector<Point> points;
    for (const Track* t : tracks) {
        const Clip* outgoing = nullptr;
        const Clip* incoming = nullptr;
        for (const Clip& c : t->clips) {
            if (c.end() == *best)
                outgoing = &c;
            if (c.start() == *best)
                incoming = &c;
        }
        if (outgoing)
            points.push_back({outgoing->id, false});
        else if (incoming)
            points.push_back({incoming->id, true});
    }
    if (points.empty())
        return false;
    m_type = Type::Roll;
    m_outOffset = m_inOffset = 0;
    m_viewOpen = true;
    setPoints(withLinkedEdges(std::move(points)));
    m_outOffset = m_inOffset = 0;
    emit detailsChanged();
    refreshFrames();
    return true;
}

void TrimBridge::toggleType() {
    if (!active()) {
        trimEdit();
        return;
    }
    m_type = m_type == Type::Roll ? Type::Ripple : m_type == Type::Ripple ? Type::Regular : Type::Roll;
    emit pointsChanged();
}

void TrimBridge::selectEditPoint(const QString& clipId, const QString& edge, const QString& type, int modifiers) {
    const auto clip = parseClipKey(clipId);
    if (!clip || !m_controller)
        return;
    const Point point{*clip, edge == QLatin1String("head")};
    std::vector<Point> points;
    if (Qt::KeyboardModifiers::fromInt(modifiers) & Qt::ShiftModifier)
        points = m_points;
    std::erase_if(points, [&](const Point& p) { return p.clip == point.clip; });
    points.push_back(point);
    setType(type);
    setPoints(withLinkedEdges(std::move(points)));
    if (m_viewOpen)
        refreshFrames();
}

void TrimBridge::clear() {
    if (!active() && !m_viewOpen)
        return;
    setPoints({});
}

void TrimBridge::openView() {
    if (!active() || m_viewOpen)
        return;
    m_viewOpen = true;
    emit pointsChanged();
    refreshFrames();
}

void TrimBridge::onSelectionChanged() {
    // Selecting clips leaves trim mode (Premiere); an edge click selects its edit point right after.
    if (!m_selecting && m_controller && !m_controller->selection().empty())
        clear();
}

void TrimBridge::onModelChanged() {
    if (m_points.empty())
        return;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    std::vector<Point> kept;
    for (const Point& p : m_points) {
        if (seq && seq->findClip(p.clip))
            kept.push_back(p);
    }
    if (kept.size() != m_points.size())
        setPoints(std::move(kept));
    else
        emit detailsChanged();
}

// ---- Trimming ---------------------------------------------------------------------------------------------------------

edit::EditOutcome TrimBridge::applyTrim(const ProjectState& state, Ticks delta, Ticks* applied) const {
    const SequenceId seqId = m_controller->currentSequenceId();
    ProjectState current = state;
    std::unordered_set<std::uint64_t> covered;
    std::optional<Ticks> first;
    for (const Point& p : m_points) {
        if (covered.contains(p.clip.value))
            continue;
        const Sequence* seq = current.findSequence(seqId);
        if (!seq || !seq->findClip(p.clip))
            continue;
        const ClipId self[] = {p.clip};
        for (const ClipId partner : edit::withLinkedClips(*seq, self))
            covered.insert(partner.value);
        const edit::Edge edge = p.head ? edit::Edge::Head : edit::Edge::Tail;
        const Ticks d = first.value_or(delta);
        edit::EditOutcome step = m_type == Type::Roll     ? edit::rollEdit(current, seqId, p.clip, edge, d)
                                 : m_type == Type::Ripple ? edit::rippleTrim(current, seqId, p.clip, edge, d)
                                                          : edit::trimEdge(current, seqId, p.clip, edge, d);
        if (!step)
            return step;
        if (!first)
            first = step->appliedDelta;
        current = std::move(step->state);
    }
    if (!first)
        return std::unexpected(Error{ErrorCode::InvalidArgument, "no edit point to trim"});
    if (applied)
        *applied = *first;
    edit::EditResult result;
    result.changes = diff(state, current);
    result.state = std::move(current);
    result.appliedDelta = *first;
    return result;
}

bool TrimBridge::trim(int frames) {
    if (!active() || frames == 0 || !m_controller)
        return false;
    const Ticks delta{m_controller->frameTicks().value * frames};
    Ticks applied;
    const auto probe = m_controller->previewEdit([&](const ProjectState& s) { return applyTrim(s, delta, &applied); });
    if (!probe || applied == Ticks{})
        return false;
    m_selecting = true;
    const bool ok = m_controller->applyEditFn(m_type == Type::Roll     ? "Roll Edit"
                                              : m_type == Type::Ripple ? "Ripple Trim"
                                                                       : "Trim",
                                              [&](const ProjectState& s) { return applyTrim(s, delta, nullptr); });
    m_selecting = false;
    if (!ok)
        return false;
    const int n = static_cast<int>(ticksToFrames(applied, m_controller->currentSequence()->frameRate));
    const bool head = m_points.front().head;
    if (m_type == Type::Roll) {
        m_outOffset += n;
        m_inOffset += n;
    } else if (head) {
        m_inOffset += n;
    } else {
        m_outOffset += n;
    }
    emit detailsChanged();
    refreshFrames();
    return true;
}

bool TrimBridge::runCommand(const QString& commandId) {
    if (commandId == QLatin1String("sequence.trimEdit")) {
        trimEdit();
        return true;
    }
    if (commandId == QLatin1String("sequence.toggleTrimType")) {
        toggleType();
        return true;
    }
    const int step = commandId == QLatin1String("sequence.trimBackward")       ? -1
                   : commandId == QLatin1String("sequence.trimForward")        ? 1
                   : commandId == QLatin1String("sequence.trimBackwardMany")   ? -m_largeOffset
                   : commandId == QLatin1String("sequence.trimForwardMany")    ? m_largeOffset
                                                                               : 0;
    if (step == 0)
        return false;
    if (active())
        trim(step);
    else if (m_controller && !m_controller->selection().empty() && !m_controller->selectedEdge().isEmpty())
        m_controller->trimSelectedEdge(step); // a clip edge chosen with [ or ]: a regular trim
    return true;
}

// ---- Dynamic trimming and play around ----------------------------------------------------------------------------------

void TrimBridge::transportIntent(int kind, int speedNum, int speedDen, int frames) {
    using Kind = KeymapDispatcher::TransportKind;
    switch (static_cast<Kind>(kind)) {
    case Kind::ShuttleStep:
    case Kind::SetSpeed:
        stopAround();
        if (!m_dynamic) {
            const auto cut = editTime();
            if (!cut || !m_controller)
                return;
            m_dynamicStart = *cut;
            m_controller->setPlayhead(*cut);
            m_dynamic = true;
            emit dynamicChanged();
        }
        if (m_playback)
            m_playback->applyTransportIntent(kind, speedNum, speedDen, frames);
        break;
    case Kind::StepFrames:
        trim(frames);
        break;
    case Kind::Stop:
        if (m_playback)
            m_playback->applyTransportIntent(kind, speedNum, speedDen, frames);
        stopAround();
        if (m_dynamic)
            finishDynamic();
        break;
    }
}

void TrimBridge::finishDynamic() {
    m_dynamic = false;
    emit dynamicChanged();
    if (!m_controller || !m_controller->currentSequence())
        return;
    const Rate rate = m_controller->currentSequence()->frameRate;
    const auto frames = static_cast<int>(ticksToFrames(m_controller->playhead() - m_dynamicStart, rate));
    if (frames != 0)
        trim(frames);
    if (const auto cut = editTime())
        m_controller->setPlayhead(*cut);
}

void TrimBridge::playAround() {
    const auto cut = editTime();
    if (!cut || !m_controller)
        return;
    m_loopFrom = std::max(Ticks{}, *cut - Ticks{static_cast<std::int64_t>(m_preroll * static_cast<double>(kTicksPerSecond))});
    m_loopTo = *cut + Ticks{static_cast<std::int64_t>(m_postroll * static_cast<double>(kTicksPerSecond))};
    m_controller->setPlayhead(m_loopFrom);
    if (m_playback)
        m_playback->play();
    m_loop.start();
    emit dynamicChanged();
}

void TrimBridge::stopAround() {
    if (!m_loop.isActive())
        return;
    m_loop.stop();
    if (m_playback && m_playback->playing())
        m_playback->pause();
    emit dynamicChanged();
}

void TrimBridge::loopTick() {
    if (!m_controller)
        return;
    if (m_playback && !m_playback->playing()) {
        stopAround(); // stopped from elsewhere
        return;
    }
    if (m_controller->playhead() >= m_loopTo)
        m_controller->setPlayhead(m_loopFrom);
}

// ---- The Trim view's pictures -------------------------------------------------------------------------------------------

void TrimBridge::refreshFrames() {
    if (!viewOpen() || !m_controller) {
        if (!m_outFrame.isNull() || !m_inFrame.isNull()) {
            m_outFrame = {};
            m_inFrame = {};
            emit framesChanged();
        }
        return;
    }
    const auto sides = sidesOf(m_points.front());
    if (!sides)
        return;
    const ProjectSnapshot snap = m_controller->snapshot();
    const SequenceId seqId = m_controller->currentSequenceId();
    const Ticks frame = m_controller->frameTicks();
    // Each side rendered with only its track visible.
    const auto isolated = [&](TrackId keep) {
        ProjectState state = *snap;
        for (SequencePtr& s : state.sequences) {
            if (s->id != seqId)
                continue;
            Sequence copy = *s;
            for (TrackPtr& t : copy.videoTracks) {
                if (t->id != keep && t->enabled) {
                    Track hidden = *t;
                    hidden.enabled = false;
                    t = std::make_shared<const Track>(std::move(hidden));
                }
            }
            s = std::make_shared<const Sequence>(std::move(copy));
        }
        return state;
    };
    const bool video = snap->findSequence(seqId) && snap->findSequence(seqId)->findTrack(sides->track)
                       && snap->findSequence(seqId)->findTrack(sides->track)->kind == TrackKind::Video;
    const int serial = ++m_frameSerial;
    struct Job {
        bool outgoing;
        std::optional<ProjectState> state;
        Ticks at;
    };
    std::vector<Job> jobs;
    jobs.push_back({true, sides->outgoing && video ? std::optional(isolated(sides->track)) : std::nullopt,
                    std::max(Ticks{}, sides->cut - frame)});
    jobs.push_back({false, sides->incoming && video ? std::optional(isolated(sides->track)) : std::nullopt, sides->cut});
    for (Job& job : jobs) {
        if (!job.state) {
            (job.outgoing ? m_outFrame : m_inFrame) = QImage();
            continue;
        }
        QPointer<TrimBridge> self(this);
        QThreadPool::globalInstance()->start([self, serial, seqId, job = std::move(job)]() mutable {
            auto image = ProgramToolsBridge::renderStateFrame(*job.state, seqId, job.at, kTrimViewWidth);
            QImage result = image ? std::move(*image) : QImage();
            QMetaObject::invokeMethod(
                qApp, [self, serial, outgoing = job.outgoing, result = std::move(result)]() mutable {
                    if (!self || serial != self->m_frameSerial)
                        return;
                    (outgoing ? self->m_outFrame : self->m_inFrame) = std::move(result);
                    emit self->framesChanged();
                },
                Qt::QueuedConnection);
        });
    }
    emit framesChanged();
}

} // namespace wabcut::app
