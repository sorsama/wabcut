#include "TrackMotionBridge.h"

#include "BridgeIds.h"
#include "FramePlacement.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/editor/Tracking.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/render/PreparedComposite.h>

#include <QCoreApplication>
#include <QPointer>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>
#include <expected>
#include <functional>

namespace wabcut::app {

namespace {

constexpr std::string_view kOpacityMask = "wabcut.mask";
constexpr double kMinBox = 0.004; ///< the smallest box side, as a fraction of the canvas

QString toQString(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

/// A TrackingServices' pictures, with its progress sent to a callback. When `stop` is set the pictures run out (NotFound, "past
/// the end of the media"): the tracker ends where it is and the job keys the frames it has, instead of the cancelled job
/// throwing a long run away (review: Stop kept nothing).
class Forwarder final : public masks::TrackingServices {
public:
    Forwarder(std::shared_ptr<masks::TrackingServices> inner, std::function<void(const masks::TrackProgress&)> callback,
              std::shared_ptr<std::atomic<bool>> stop)
        : m_inner(std::move(inner)), m_callback(std::move(callback)), m_stop(std::move(stop)) {}
    std::expected<masks::TrackingFrame, Error> frame(const ProjectState& state, MediaId media, int stream, Ticks time,
                                                     int maxWidth) override {
        if (m_stop && m_stop->load()) {
            return std::unexpected(Error{ErrorCode::NotFound, "stopped"});
        }
        return m_inner->frame(state, media, stream, time, maxWidth);
    }
    void progress(const masks::TrackProgress& progress) override {
        m_inner->progress(progress);
        m_callback(progress);
    }
    std::expected<std::vector<masks::DetectedRegion>, Error> detect(const masks::GrayImage& picture, std::string_view kind) override {
        return m_inner->detect(picture, kind);
    }

private:
    std::shared_ptr<masks::TrackingServices> m_inner;
    std::function<void(const masks::TrackProgress&)> m_callback;
    std::shared_ptr<std::atomic<bool>> m_stop;
};

/// A clip the tracker can look at: camera or screen footage (not a generated item, a still, a nest or a title).
bool isFootage(const ProjectState& state, const Clip& clip) {
    if (clip.stream.kind != StreamKind::Video || clip.isNested() || clip.hasUnknownSource() || !clip.media.isValid()) {
        return false;
    }
    const MediaItem* item = state.findMedia(clip.media);
    return item && !item->isGenerator() && !item->info.isStillImage && clip.stream.index >= 0 &&
           static_cast<std::size_t>(clip.stream.index) < item->info.video.size();
}

/// "V2" for a video track of the sequence.
QString trackLabel(const Sequence& seq, const Track& track) {
    const auto pos = seq.trackPosition(track.id);
    return pos ? (pos->kind == TrackKind::Video ? QStringLiteral("V") : QStringLiteral("A")) + QString::number(pos->index + 1) : QString();
}

QVariantMap choiceOf(const Sequence& seq, const Track& track, const Clip& clip) {
    const QString name = QString::fromStdString(clip.name());
    return {{QStringLiteral("key"), clipKey(clip.id)},
            {QStringLiteral("name"), name},
            {QStringLiteral("track"), trackLabel(seq, track)}};
}

} // namespace

/// Where the footage clip shows its stored frame on the canvas at one curve time, as playback places it: the one shared
/// placement (FramePlacement.h: the stream's display orientation with its mirror, the sequence's pixel aspect, the clip's
/// Motion), the same as masks and object masks use.
struct TrackMotionBridge::Placement : FramePlacement {};

TrackMotionBridge::TrackMotionBridge(QObject* parent) : QObject(parent) {
    // A singleton cannot create the others while it is being made: attach to the project once the engine is done.
    QMetaObject::invokeMethod(this, &TrackMotionBridge::refresh, Qt::QueuedConnection);
}

TrackMotionBridge::~TrackMotionBridge() {
    if (m_cancel) {
        m_cancel->store(true);
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

TrackMotionBridge* TrackMotionBridge::create(QQmlEngine*, QJSEngine*) { return new TrackMotionBridge; }

void TrackMotionBridge::setController(ProjectController* controller) {
    m_controller = controller;
    m_attached = false;
    refresh();
}

ProjectController* TrackMotionBridge::controller() const {
    if (m_controller) {
        return m_controller;
    }
    QQmlEngine* engine = qmlEngine(this);
    return engine ? engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController") : nullptr;
}

void TrackMotionBridge::attach() {
    if (m_attached) {
        return;
    }
    ProjectController* c = controller();
    if (!c) {
        return;
    }
    m_attached = true;
    connect(c, &ProjectController::historyChanged, this, &TrackMotionBridge::refresh);
    connect(c, &ProjectController::playheadChanged, this, &TrackMotionBridge::refresh);
    connect(c, &ProjectController::selectionChanged, this, &TrackMotionBridge::refresh);
    connect(c, &ProjectController::sequenceChanged, this, &TrackMotionBridge::refresh);
    connect(c, &ProjectController::sessionClosing, this, [this] {
        cancelTrack();
        m_basis = m_subject = m_follower = {};
        m_hasBox = false;
    });
}

QString TrackMotionBridge::subject() const { return m_subject.isValid() ? clipKey(m_subject) : QString(); }
QString TrackMotionBridge::follower() const { return m_follower.isValid() ? clipKey(m_follower) : QString(); }

QVariantList TrackMotionBridge::box() const {
    QVariantList out;
    if (m_hasBox) {
        out = {m_box[0], m_box[1], m_box[2], m_box[3]};
    }
    return out;
}

void TrackMotionBridge::setPanelOpen(bool open) {
    if (open == m_panelOpen) {
        return;
    }
    m_panelOpen = open;
    if (!open) {
        m_boxTool = false; // nothing left to draw on
        emit toolChanged();
    }
    emit stateChanged();
    emit geometryChanged();
}

void TrackMotionBridge::setSubject(const QString& key) {
    const auto id = parseClipKey(key);
    if (!id || *id == m_subject) {
        return;
    }
    m_subject = *id;
    m_subjectChosen = true;
    m_mask.clear();
    m_forceEmit = true;
    refresh();
}

void TrackMotionBridge::setFollower(const QString& key) {
    const auto id = parseClipKey(key);
    if (!id || *id == m_follower) {
        return;
    }
    m_follower = *id;
    m_followerChosen = true;
    m_forceEmit = true;
    refresh();
}

void TrackMotionBridge::setStabilize(bool stabilize) {
    if (stabilize == m_stabilize) {
        return;
    }
    m_stabilize = stabilize;
    m_forceEmit = true;
    refresh();
}

void TrackMotionBridge::setMatch(const QString& match) {
    const QString next = match == QLatin1String("full") ? match : QStringLiteral("position");
    if (next == m_match) {
        return;
    }
    m_match = next;
    m_forceEmit = true;
    refresh();
}

void TrackMotionBridge::setMask(const QString& key) {
    if (key == m_mask) {
        return;
    }
    m_mask = key;
    if (!key.isEmpty()) {
        m_hasBox = false; // a mask or a box, not both
        m_boxTool = false;
        emit toolChanged();
        emit geometryChanged();
    }
    m_forceEmit = true;
    refresh();
}

void TrackMotionBridge::setBoxTool(bool on) {
    if (on == m_boxTool) {
        return;
    }
    m_boxTool = on;
    emit toolChanged();
}

bool TrackMotionBridge::setBox(double x0, double y0, double x1, double y1) {
    if (!std::isfinite(x0 + y0 + x1 + y1)) {
        return false;
    }
    const auto clamp01 = [](double v) { return std::clamp(v, 0.0, 1.0); };
    std::array<double, 4> next{clamp01(std::min(x0, x1)), clamp01(std::min(y0, y1)), clamp01(std::max(x0, x1)), clamp01(std::max(y0, y1))};
    if (next[2] - next[0] < kMinBox || next[3] - next[1] < kMinBox) {
        return false;
    }
    m_box = next;
    m_hasBox = true;
    m_mask.clear();
    m_boxTool = false;
    emit toolChanged();
    emit geometryChanged();
    m_forceEmit = true;
    refresh();
    return true;
}

void TrackMotionBridge::placeBox() {
    double cx = 0.5, cy = 0.5;
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    if (seq && m_follower.isValid() && !m_stabilize && seq->width > 0 && seq->height > 0) {
        if (const auto loc = seq->findClip(m_follower)) {
            const ClipProperties& p = loc->clip().properties();
            const Ticks t = c->curveTimeAtPlayhead(loc->clip());
            cx = std::clamp(0.5 + p.value(ParamId::PositionX, t) / seq->width, 0.0, 1.0);
            cy = std::clamp(0.5 + p.value(ParamId::PositionY, t) / seq->height, 0.0, 1.0);
        }
    }
    const double w = 0.2;
    const double aspect = seq && seq->height > 0 ? static_cast<double>(seq->width) / seq->height : 16.0 / 9.0;
    const double h = std::min(0.5, w * aspect); // as tall as wide in pixels
    const double x0 = std::clamp(cx - w / 2, 0.0, 1.0 - w);
    const double y0 = std::clamp(cy - h / 2, 0.0, 1.0 - h);
    setBox(x0, y0, x0 + w, y0 + h);
}

void TrackMotionBridge::clearBox() {
    if (!m_hasBox) {
        return;
    }
    m_hasBox = false;
    emit geometryChanged();
    m_forceEmit = true;
    refresh();
}

void TrackMotionBridge::setBlocker(const QString& blocker) {
    if (blocker == m_blocker) {
        return;
    }
    m_blocker = blocker;
}

// ---- State ----------------------------------------------------------------------------------------------------------

void TrackMotionBridge::refresh() {
    attach();
    const ClipId previousSubject = m_subject;
    const ClipId previousFollower = m_follower;
    const QString previousMask = m_mask;
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const ProjectSnapshot snap = c ? c->snapshot() : nullptr;

    // The clip Effect Controls shows: the selected video clip, else the topmost video clip under the playhead.
    ClipId basis;
    struct Entry {
        const Track* track;
        const Clip* clip;
    };
    std::vector<Entry> under; // video clips under the playhead, topmost first
    if (seq && snap) {
        for (auto it = seq->videoTracks.rbegin(); it != seq->videoTracks.rend(); ++it) {
            const Track& track = **it;
            if (const auto i = track.clipIndexAt(c->playhead()); i && track.clips[*i].enabled) {
                under.push_back({&track, &track.clips[*i]});
            }
        }
        if (const auto t = c->propertyTarget(StreamKind::Video); t && seq->findClip(*t)) {
            basis = *t;
        } else if (!under.empty()) {
            basis = under.front().clip->id;
        }
    }
    if (basis != m_basis) {
        // Another clip: the pick starts over (the box stays: it is where the user looked on screen).
        m_basis = basis;
        m_subjectChosen = false;
        m_followerChosen = false;
        m_mask.clear();
    }

    // Candidates and the defaults from the shown clip.
    std::vector<Entry> footage;
    for (const Entry& e : under) {
        if (snap && isFootage(*snap, *e.clip)) {
            footage.push_back(e);
        }
    }
    const auto isUnder = [&](ClipId id) {
        return std::any_of(under.begin(), under.end(), [&](const Entry& e) { return e.clip->id == id; });
    };
    const auto isFootageId = [&](ClipId id) {
        return std::any_of(footage.begin(), footage.end(), [&](const Entry& e) { return e.clip->id == id; });
    };
    if (!(m_subjectChosen && isFootageId(m_subject))) {
        m_subjectChosen = false;
        ClipId pick;
        if (isFootageId(basis)) {
            pick = basis;
        } else if (!footage.empty()) {
            // A graphic is the follower of the footage under it: the topmost footage below it (else any).
            bool below = false;
            for (const Entry& e : under) {
                below = below || e.clip->id == basis;
                if (below && isFootageId(e.clip->id)) {
                    pick = e.clip->id;
                    break;
                }
            }
            if (!pick.isValid()) {
                pick = footage.front().clip->id;
            }
        }
        if (pick != m_subject) {
            m_mask.clear();
        }
        m_subject = pick;
    }
    // "Follows": another clip under the playhead. A graphic that is shown follows; footage gets the clip above it.
    if (!(m_followerChosen && isUnder(m_follower) && m_follower != m_subject)) {
        m_followerChosen = false;
        ClipId pick;
        if (basis.isValid() && basis != m_subject && isUnder(basis)) {
            pick = basis;
        } else {
            bool above = true;
            for (const Entry& e : under) { // topmost first: the clips above the subject come before it
                if (e.clip->id == m_subject) {
                    above = false;
                    continue;
                }
                if (above || !pick.isValid()) {
                    pick = e.clip->id;
                    if (above) {
                        break;
                    }
                }
            }
        }
        m_follower = pick;
    }

    QVariantList subjects, followers;
    for (const Entry& e : footage) {
        subjects.append(choiceOf(*seq, *e.track, *e.clip));
    }
    for (const Entry& e : under) {
        if (e.clip->id != m_subject) {
            followers.append(choiceOf(*seq, *e.track, *e.clip));
        }
    }
    QString subjectName, followerName;
    QVariantList masks;
    if (m_subject.isValid() && seq) {
        if (const auto loc = seq->findClip(m_subject)) {
            subjectName = QString::fromStdString(loc->clip().name());
            for (const EffectInstance& e : loc->clip().effects()) {
                for (const Mask& m : e.masks) {
                    const bool opacity = e.effect == kOpacityMask;
                    const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
                    const QString holder = opacity ? tr("Opacity mask") : d ? toQString(d->displayName) : toQString(e.effect);
                    const QString name = QString::fromStdString(m.name).isEmpty() ? tr("Mask %1").arg(m.id) : QString::fromStdString(m.name);
                    masks.append(QVariantMap{{QStringLiteral("key"), effectKey(e.id) + QLatin1Char('/') + QString::number(m.id)},
                                             {QStringLiteral("name"), QStringLiteral("%1 (%2)").arg(name, holder)}});
                }
            }
        }
    }
    if (!masks.isEmpty() || !m_mask.isEmpty()) {
        const bool known = std::any_of(masks.begin(), masks.end(), [&](const QVariant& v) { return v.toMap().value(QStringLiteral("key")).toString() == m_mask; });
        if (!known) {
            m_mask.clear();
        }
    }
    if (m_follower.isValid() && seq) {
        if (const auto loc = seq->findClip(m_follower)) {
            followerName = QString::fromStdString(loc->clip().name());
        }
    }

    // Why Track cannot run.
    QString blocker;
    const Clip* subjectClip = nullptr;
    if (seq && m_subject.isValid()) {
        const auto loc = seq->findClip(m_subject);
        subjectClip = loc ? &loc->clip() : nullptr;
    }
    if (!subjectClip) {
        blocker = tr("Put the playhead over footage to track");
    } else if (subjectClip->isRemapped()) {
        blocker = tr("Turn off Time Remapping on %1 to track it").arg(subjectName);
    } else if (subjectClip->isFreeze()) {
        blocker = tr("%1 is a frame hold: there is no motion to track").arg(subjectName);
    } else if (!m_stabilize && !m_follower.isValid()) {
        blocker = tr("Pick the clip that should follow, or choose Stabilize");
    } else if (!m_hasBox && m_mask.isEmpty()) {
        blocker = tr("Draw a box around the subject on the program monitor, or pick a mask");
    }

    const bool changed = m_forceEmit || previousSubject != m_subject || previousFollower != m_follower || previousMask != m_mask ||
                         subjects != m_subjects || followers != m_followers || masks != m_masks || subjectName != m_subjectName ||
                         followerName != m_followerName || blocker != m_blocker;
    m_forceEmit = false;
    m_subjects = std::move(subjects);
    m_followers = std::move(followers);
    m_masks = std::move(masks);
    m_subjectName = subjectName;
    m_followerName = followerName;
    m_blocker = blocker;
    if (changed) {
        emit stateChanged();
    }
}

// ---- Tracking -------------------------------------------------------------------------------------------------------

std::optional<TrackMotionBridge::Placement> TrackMotionBridge::placementAtPlayhead() const {
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const ProjectSnapshot snap = c ? c->snapshot() : nullptr;
    const auto loc = seq && snap && m_subject.isValid() ? seq->findClip(m_subject) : std::nullopt;
    if (!loc) {
        return std::nullopt;
    }
    const auto shared = framePlacementOf(*snap, *seq, loc->clip(), c->curveTimeAtPlayhead(loc->clip()));
    if (!shared) {
        return std::nullopt;
    }
    Placement out;
    static_cast<FramePlacement&>(out) = *shared;
    return out;
}

bool TrackMotionBridge::track(const QString& direction, int frames) {
    ProjectController* c = controller();
    if (m_tracking || !c) {
        return false;
    }
    refresh();
    if (!m_blocker.isEmpty()) {
        m_trackStatus = m_blocker;
        emit trackingChanged();
        return false;
    }
    const SequencePtr seq = c->currentSequence();
    const auto subjectLoc = seq->findClip(m_subject);
    if (!subjectLoc) {
        return false;
    }
    const Clip& subjectClip = subjectLoc->clip();
    const Ticks playhead = c->playhead();
    if (playhead < subjectClip.start() || playhead >= subjectClip.end()) {
        m_trackStatus = tr("Put the playhead on %1 to start from there").arg(m_subjectName);
        emit trackingChanged();
        return false;
    }
    if (!m_stabilize) {
        const auto followerLoc = seq->findClip(m_follower);
        if (!followerLoc || playhead < followerLoc->clip().start() || playhead >= followerLoc->clip().end()) {
            m_trackStatus = tr("Put the playhead where %1 is over the subject: tracking starts from there").arg(m_followerName);
            emit trackingChanged();
            return false;
        }
    }
    // The region: a mask of the footage clip, or the box turned into frame fractions through the clip's Motion.
    command::Json region;
    if (!m_mask.isEmpty()) {
        const qsizetype slash = m_mask.indexOf(QLatin1Char('/'));
        const auto effect = parseEffectKey(m_mask.left(slash));
        bool ok = false;
        const uint maskId = m_mask.mid(slash + 1).toUInt(&ok);
        if (slash < 0 || !effect || !ok) {
            m_trackStatus = tr("Pick the mask again");
            emit trackingChanged();
            return false;
        }
        region = command::Json{{"mask", maskId}, {"effect", effect->value}};
    } else {
        const auto placement = placementAtPlayhead();
        if (!placement) {
            m_trackStatus = tr("%1 has no picture to track here").arg(m_subjectName);
            emit trackingChanged();
            return false;
        }
        double lo[2] = {1e9, 1e9}, hi[2] = {-1e9, -1e9};
        for (const auto& corner : {std::pair{m_box[0], m_box[1]}, std::pair{m_box[2], m_box[1]}, std::pair{m_box[2], m_box[3]},
                                   std::pair{m_box[0], m_box[3]}}) {
            const render::Vec2 p = placement->canvasToFrame.map({corner.first * placement->canvasWidth, corner.second * placement->canvasHeight});
            const double u = p.x / placement->frameWidth, v = p.y / placement->frameHeight;
            lo[0] = std::min(lo[0], u);
            lo[1] = std::min(lo[1], v);
            hi[0] = std::max(hi[0], u);
            hi[1] = std::max(hi[1], v);
        }
        for (double& v : lo) {
            v = std::clamp(v, 0.0, 1.0);
        }
        for (double& v : hi) {
            v = std::clamp(v, 0.0, 1.0);
        }
        if (hi[0] - lo[0] < kMinBox || hi[1] - lo[1] < kMinBox) {
            m_trackStatus = tr("The box lies outside the picture of %1: draw it over the subject").arg(m_subjectName);
            emit trackingChanged();
            return false;
        }
        region = command::Json{{"x", lo[0]}, {"y", lo[1]}, {"width", hi[0] - lo[0]}, {"height", hi[1] - lo[1]}};
    }
    command::Json params{{"clip", m_subject.value},
                         {"region", region},
                         {"mode", m_stabilize ? "stabilize" : "follow"},
                         {"model", m_match == QLatin1String("full") ? "similarity" : "translation"},
                         {"direction", direction == QLatin1String("backward") ? "backward" : "forward"},
                         {"time", playhead.value},
                         {"sequence", seq->id.value}};
    if (!m_stabilize) {
        params["target"] = m_follower.value;
    }
    if (frames > 0) {
        params["frames"] = frames;
    }
    m_lastRequest = params.dump();

    std::shared_ptr<masks::TrackingServices> pictures = m_trackingServices;
    if (!pictures) {
        std::filesystem::path projectFile;
        if (QQmlEngine* engine = qmlEngine(this)) {
            if (auto* file = engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile")) {
                projectFile = file->document().file();
            }
        }
        pictures = std::make_shared<editor::MediaTrackingService>(projectFile);
    }
    m_tracking = true;
    m_boxTool = false;
    m_trackProgress = 0.0;
    m_trackTotal = frames > 0 ? frames : 0;
    m_strip.clear();
    m_stripFramesPerValue = 1;
    m_confidence.clear();
    m_announced.invalidate();
    m_trackStatus = tr("Tracking…");
    emit toolChanged();
    emit trackingChanged();

    m_cancel = std::make_shared<std::atomic<bool>>(false);
    const ProjectSnapshot base = c->snapshot();
    const ClipId subjectId = m_subject;
    QPointer<TrackMotionBridge> self(this);
    const bool synchronous = m_synchronous;
    const auto onProgress = [self, synchronous](const masks::TrackProgress& progress) {
        const auto post = [self, progress] {
            if (self) {
                self->noteProgress(progress);
            }
        };
        if (synchronous) {
            post();
        } else {
            QMetaObject::invokeMethod(QCoreApplication::instance(), post, Qt::QueuedConnection);
        }
    };
    auto work = [base, pictures, onProgress, params, cancel = m_cancel]() -> std::pair<command::CommandResult, ProjectSnapshot> {
        EditSession scratch(*base);
        command::EnginePolicy policy;
        policy.services = std::make_shared<Forwarder>(pictures, onProgress, cancel);
        command::CommandEngine engine(scratch, ai::edit::editingCommands(), policy);
        command::CommandRequest request;
        request.command = "track.transform";
        request.params = params;
        command::CommandResult result = engine.execute(request);
        return {std::move(result), scratch.snapshot()};
    };
    const auto finish = [self, subjectId, base, sequence = seq->id, cancel = m_cancel](std::pair<command::CommandResult, ProjectSnapshot> outcome) {
        if (!self) {
            return;
        }
        const bool stoppedByUser = cancel->load();
        const command::CommandResult& r = outcome.first;
        if (!r.ok()) {
            // Stopped before the tracker had a first picture: nothing was tracked.
            self->finishTrack(false,
                              stoppedByUser ? tr("Tracking stopped before the first frame")
                                            : tr("Could not track: %1").arg(QString::fromStdString(r.error->message)),
                              base, std::nullopt, subjectId, Ticks{});
            return;
        }
        ProjectController* live = self->controller();
        if (!live || live->snapshot() != base || live->currentSequenceId() != sequence) {
            self->finishTrack(false, tr("Project changed while tracking; track again"), base, std::nullopt, subjectId, Ticks{});
            return;
        }
        const int done = r.data.value("frames", 0);
        if (stoppedByUser && done == 0) {
            self->finishTrack(false, tr("Tracking stopped before the first frame"), base, std::nullopt, subjectId, Ticks{});
            return;
        }
        const std::string stopped = r.data.value("stopped", std::string());
        const Ticks last{r.data.value("lastMediaTime", std::int64_t{0})};
        // Stop keeps what was tracked until then (one undo step like a run to the end).
        QString message = stopped == "lost" ? tr("Lost after %n frame(s): the picture changed too much", nullptr, done)
                          : stoppedByUser   ? tr("Stopped after %n frame(s)", nullptr, done)
                                            : tr("Tracked %n frame(s)", nullptr, done);
        self->finishTrack(true, message, base, *outcome.second, subjectId, last);
    };
    if (synchronous) {
        finish(work());
        return true;
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_worker = std::jthread([work, finish] {
        auto outcome = work();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [finish, outcome = std::move(outcome)]() mutable { finish(std::move(outcome)); },
                                  Qt::QueuedConnection);
    });
    return true;
}

void TrackMotionBridge::finishTrack(bool ok, const QString& message, const ProjectSnapshot&, std::optional<ProjectState> result,
                                    ClipId subject, Ticks lastMedia) {
    m_tracking = false;
    m_trackStatus = message;
    publishConfidence(); // the strip as it ended (progress was announced at most ten times a second)
    ProjectController* c = controller();
    if (ok && result && c) {
        // The keyed project from the worker's copy (nothing changed while it ran): ONE undo step.
        auto r = c->session().apply(tr("Track Motion").toStdString(),
                                    [&](const ProjectState&) -> std::expected<ProjectState, Error> { return *result; });
        if (!r) {
            m_trackStatus = tr("Could not apply the track: %1").arg(QString::fromStdString(r.error().message));
        } else {
            m_hasBox = false; // the box was where the subject started: used up
            emit geometryChanged();
            if (const auto now = c->currentSequence()->findClip(subject)) {
                // The playhead follows to the last frame tracked (as a step through the clip does).
                const Clip& cl = now->clip();
                const Ticks offset = cl.reversed ? cl.sourceOut() - Ticks{1} - lastMedia : lastMedia - cl.sourceIn;
                const Ticks timeline = cl.start() + toTimelineTicks(offset, cl.speed);
                if (timeline >= cl.start() && timeline < cl.end()) {
                    c->setPlayhead(snapToFrame(timeline, c->currentSequence()->frameRate));
                }
            }
        }
    }
    if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id()) {
        m_worker.join();
    }
    emit trackingChanged();
    refresh();
}

void TrackMotionBridge::cancelTrack() {
    if (m_cancel) {
        m_cancel->store(true);
    }
}

void TrackMotionBridge::noteProgress(const masks::TrackProgress& progress) {
    m_trackProgress = progress.total > 0 ? static_cast<double>(progress.frame) / progress.total : 0.0;
    m_trackTotal = progress.total;
    m_strip.add(progress.lost ? -1.0 : progress.confidence);
    // The strip is converted to a list for QML each time it is announced: ten times a second is plenty for a progress bar,
    // and the cost no longer grows with the frames done (it was a quadratic copy of every frame so far).
    if (!m_announced.isValid() || m_announced.elapsed() >= 100) {
        m_announced.restart();
        publishConfidence();
        emit trackingChanged();
    }
}

void TrackMotionBridge::publishConfidence() {
    QVariantList list;
    list.reserve(static_cast<qsizetype>(m_strip.values().size()));
    for (const double v : m_strip.values()) {
        list.append(v);
    }
    m_confidence = std::move(list);
    m_stripFramesPerValue = m_strip.framesPerBucket();
}

} // namespace wabcut::app
