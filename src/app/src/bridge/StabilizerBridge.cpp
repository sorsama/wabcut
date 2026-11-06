#include "StabilizerBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/stabilize/StabilizeCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/editor/Gyro.h>
#include <wabcut/editor/Tracking.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MasterClipEffects.h>
#include <wabcut/model/MotionOps.h>

#include <QCoreApplication>
#include <QPointer>
#include <QQmlEngine>
#include <QStringList>

#include <algorithm>
#include <expected>
#include <functional>

namespace wabcut::app {

namespace {

/// Stops an analysis from the GUI thread.
class CancelFlag final : public command::ExecutionControl {
public:
    explicit CancelFlag(std::shared_ptr<std::atomic<bool>> cancelled) : m_cancelled(std::move(cancelled)) {}
    std::optional<command::CommandError> checkpoint(std::string_view) override {
        if (m_cancelled && m_cancelled->load()) {
            command::CommandError error;
            error.code = command::ErrorCode::Cancelled;
            error.message = "cancelled";
            return error;
        }
        return std::nullopt;
    }

private:
    std::shared_ptr<std::atomic<bool>> m_cancelled;
};

/// A TrackingServices' pictures, with its progress sent to a callback.
class Forwarder final : public masks::TrackingServices {
public:
    Forwarder(std::shared_ptr<masks::TrackingServices> inner, std::function<void(const masks::TrackProgress&)> callback)
        : m_inner(std::move(inner)), m_callback(std::move(callback)) {}
    std::expected<masks::TrackingFrame, Error> frame(const ProjectState& state, MediaId media, int stream, Ticks time,
                                                     int maxWidth) override {
        return m_inner->frame(state, media, stream, time, maxWidth);
    }
    void progress(const masks::TrackProgress& progress) override {
        m_inner->progress(progress);
        m_callback(progress);
    }

private:
    std::shared_ptr<masks::TrackingServices> m_inner;
    std::function<void(const masks::TrackProgress&)> m_callback;
};

bool isStabilizer(const std::string& effect) { return effect == kWarpStabilizerEffect || effect == kRollingShutterRepairEffect; }

/// 0.7 (card #130): the camera's gyro for an analysis: the test's, else the media files' (the original of each clip;
/// reading stops with Cancel).
std::shared_ptr<gyro::GyroServices> gyroFor(const std::shared_ptr<gyro::GyroServices>& given, ProjectController* controller,
                                            const std::shared_ptr<std::atomic<bool>>& cancel) {
    if (given) {
        return given;
    }
    return std::make_shared<editor::MediaGyroService>(controller ? controller->projectFile() : std::filesystem::path(),
                                                      [cancel] { return cancel && cancel->load(); });
}

} // namespace

struct StabilizerBridge::Found {
    SequenceId sequence;
    ClipId clip;
    EffectInstance effect;
    MediaId master; ///< fix F3 (NEW-01): the media item whose Master Clip Effects hold the effect (else invalid)
};

StabilizerBridge::StabilizerBridge(QObject* parent) : QObject(parent) {}

StabilizerBridge::~StabilizerBridge() {
    if (m_cancel) {
        m_cancel->store(true);
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

ProjectController* StabilizerBridge::controller() const {
    QQmlEngine* engine = qmlEngine(this);
    return engine ? engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController") : nullptr;
}

std::optional<StabilizerBridge::Found> StabilizerBridge::find(const QString& section) const {
    const auto id = parseEffectKey(section);
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const ProjectSnapshot snap = c ? c->snapshot() : nullptr;
    if (!id || !seq || !snap) {
        return std::nullopt;
    }
    // Fix F3 (NEW-01): an effect of a media item's Master Clip Effects, which Effect Controls shows with a clip of that
    // media (the selected one first). Analyze keys it on the media item, for every clip of it.
    std::optional<Found> master;
    const ClipId selected = c->primaryClip();
    for (const TrackPtr& track : seq->videoTracks) {
        for (const Clip& clip : track->clips) {
            for (const EffectInstance& e : clip.effects()) {
                if (e.id == *id) {
                    return Found{seq->id, clip.id, e, {}};
                }
            }
            if ((master && master->clip == selected) || clip.isNested() || !clip.media.isValid()) {
                continue;
            }
            if (const MediaItem* holder = masterEffectHolder(*snap, clip.media, *id)) {
                for (const EffectInstance& e : holder->masterEffects) {
                    if (e.id == *id && (!master || clip.id == selected)) {
                        master = Found{seq->id, clip.id, e, holder->id};
                    }
                }
            }
        }
    }
    return master;
}

QVariantMap StabilizerBridge::effectInfo(const QString& section) const {
    const auto found = find(section);
    if (!found || !isStabilizer(found->effect.effect)) {
        return {{QStringLiteral("stabilizer"), false}};
    }
    const EffectData* status = found->effect.findData("status");
    // 0.7 (card #128): analysed, but the clip shows other frames now or a setting changed since.
    bool outOfDate = false;
    for (const ai::stabilize::StabilizerEntry& e : survey()) {
        outOfDate = outOfDate || (e.effect == found->effect.id && e.need == ai::stabilize::StabilizerNeed::OutOfDate);
    }
    return {{QStringLiteral("stabilizer"), true},
            {QStringLiteral("kind"), found->effect.effect == kWarpStabilizerEffect ? QStringLiteral("warp") : QStringLiteral("shutter")},
            {QStringLiteral("status"), status && status->text() ? QString::fromStdString(*status->text()) : QString()},
            {QStringLiteral("outOfDate"), outOfDate},
            // 0.7 (card #130): the Method is Gyro (camera metadata): the analysis reads the camera's recorded rotation.
            {QStringLiteral("gyro"), found->effect.effect == kWarpStabilizerEffect &&
                                         std::lround(effectParameterAt(found->effect, "method", Ticks{}, 3.0)) >= 4},
            // 0.7 (card #129): Smoothness has keyframes, so the analysis steadies some parts of the shot more than others.
            {QStringLiteral("smoothnessKeyed"), found->effect.effect == kWarpStabilizerEffect && found->effect.find("smoothness") &&
                                                    found->effect.find("smoothness")->isCurve()}};
}

bool StabilizerBridge::analyze(const QString& section) {
    ProjectController* c = controller();
    const auto found = find(section);
    if (busy() || !c || !found || !isStabilizer(found->effect.effect)) {
        return false;
    }
    std::shared_ptr<masks::TrackingServices> pictures = m_pictures;
    if (!pictures) {
        std::filesystem::path projectFile;
        if (QQmlEngine* engine = qmlEngine(this)) {
            if (auto* file = engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile")) {
                projectFile = file->document().file();
            }
        }
        pictures = std::make_shared<editor::MediaTrackingService>(projectFile);
    }
    const std::filesystem::path cacheRoot = m_cacheRoot.empty() ? ai::AnalysisCache::defaultRoot() : m_cacheRoot;
    m_section = section;
    m_progress = 0.0;
    m_message = tr("Analyzing in background");
    emit changed();

    m_cancel = std::make_shared<std::atomic<bool>>(false);
    const std::shared_ptr<gyro::GyroServices> gyroServices = gyroFor(m_gyro, c, m_cancel);
    const ProjectSnapshot base = c->snapshot();
    const ClipId clip = found->clip;
    const EffectInstanceId effect = found->effect.id;
    const std::string command =
        found->effect.effect == kWarpStabilizerEffect ? "effect.analyzeStabilization" : "effect.analyzeRollingShutter";
    const command::Json params{{"clip", clip.value}, {"effect", effect.value}, {"sequence", found->sequence.value}};
    QPointer<StabilizerBridge> self(this);
    const bool synchronous = m_synchronous;
    const auto onProgress = [self, synchronous](const masks::TrackProgress& p) {
        const auto post = [self, p] {
            if (self && self->busy()) {
                self->m_progress = p.total > 0 ? static_cast<double>(p.frame) / p.total : 0.0;
                self->m_message = tr("Analyzing in background (frame %1 of %2)").arg(p.frame).arg(p.total);
                emit self->changed();
            }
        };
        if (synchronous) {
            post();
        } else {
            QMetaObject::invokeMethod(QCoreApplication::instance(), post, Qt::QueuedConnection);
        }
    };
    auto work = [base, pictures, gyroServices, onProgress, params, command, cacheRoot,
                 cancel = m_cancel]() -> std::pair<command::CommandResult, ProjectSnapshot> {
        EditSession scratch(*base);
        command::EnginePolicy policy;
        policy.services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
            std::make_shared<Forwarder>(pictures, onProgress), gyroServices,
            std::make_shared<ai::stabilize::StabilizationServices>(std::make_shared<ai::AnalysisCache>(cacheRoot))});
        command::CommandEngine engine(scratch, ai::edit::editingCommands(), policy);
        command::CommandRequest request;
        request.command = command;
        request.params = params;
        CancelFlag control(cancel);
        command::CommandResult result = engine.execute(request, &control);
        return {std::move(result), scratch.snapshot()};
    };
    const auto done = [self, clip, effect, base, cancel = m_cancel](std::pair<command::CommandResult, ProjectSnapshot> outcome) {
        if (!self) {
            return;
        }
        const command::CommandResult& r = outcome.first;
        if (!r.ok()) {
            self->finish(false,
                         r.error->code == command::ErrorCode::Cancelled ? tr("Analysis cancelled")
                                                                        : tr("Could not analyze: %1").arg(QString::fromStdString(r.error->message)),
                         clip, effect, std::nullopt);
            return;
        }
        ProjectController* live = self->controller();
        if (cancel->load() || !live) {
            self->finish(false, tr("Analysis cancelled"), clip, effect, std::nullopt);
            return;
        }
        QString message = tr("Analysis complete");
        if (r.data.value("lostFrames", 0) > 0) {
            message = tr("Analysis complete (%n frame(s) could not be matched)", nullptr, r.data.value("lostFrames", 0));
        }
        if (!r.warnings.empty()) {
            message += QStringLiteral(": ") + QString::fromStdString(r.warnings.front());
        }
        self->finish(true, message, clip, effect, *outcome.second);
    };
    if (synchronous) {
        done(work());
        return true;
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_worker = std::jthread([work, done] {
        auto outcome = work();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [done, outcome = std::move(outcome)]() mutable { done(std::move(outcome)); },
                                  Qt::QueuedConnection);
    });
    return true;
}

void StabilizerBridge::finish(bool ok, const QString& message, ClipId clip, EffectInstanceId effect,
                              std::optional<ProjectState> result) {
    m_section.clear();
    m_progress = ok ? 1.0 : 0.0;
    m_message = message;
    ProjectController* c = controller();
    if (ok && result && c) {
        // The analysed instance from the worker's copy, applied to the project as it is now (one undo step): every
        // parameter and data entry, the user's settings included (they were what the analysis used).
        const SequenceId seq = c->currentSequenceId();
        const Sequence* analysed = result->findSequence(seq);
        const auto loc = analysed ? analysed->findClip(clip) : std::nullopt;
        const EffectInstance* e = nullptr;
        const MediaItem* holder = nullptr; // fix F3 (NEW-01): a master clip effect is keyed on its media item
        if (loc) {
            for (const EffectInstance& candidate : loc->clip().effects()) {
                e = candidate.id == effect ? &candidate : e;
            }
            holder = !e && loc->clip().media.isValid() ? masterEffectHolder(*result, loc->clip().media, effect) : nullptr;
            if (holder) {
                for (const EffectInstance& candidate : holder->masterEffects) {
                    e = candidate.id == effect ? &candidate : e;
                }
            }
        }
        if (e) {
            edit::EffectChange change;
            change.params = e->params;
            change.data = e->data;
            const QString label = e->effect == kWarpStabilizerEffect ? tr("Analyze Stabilization") : tr("Analyze Rolling Shutter");
            const std::optional<MediaId> master = holder ? std::optional<MediaId>(holder->id) : std::nullopt;
            auto r = c->session().apply(label.toStdString(), [&](const ProjectState& s) {
                return master ? edit::setEffect(s, seq, edit::EffectTarget::ofMedia(*master), effect, change)
                              : edit::setEffect(s, seq, clip, effect, change);
            });
            if (!r) {
                m_message = tr("Could not apply the analysis: %1").arg(QString::fromStdString(r.error().message));
            }
        } else {
            m_message = tr("The clip changed while analyzing; analyze again");
        }
    }
    if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id()) {
        m_worker.join();
    }
    emit changed();
}

void StabilizerBridge::cancel() {
    if (m_cancel) {
        m_cancel->store(true);
    }
}

// ---- Analyze All and the export warning (0.7, cards #107, #128) -----------------------------------------------------

const std::vector<ai::stabilize::StabilizerEntry>& StabilizerBridge::survey() const {
    ProjectController* c = controller();
    const ProjectSnapshot snap = c ? c->snapshot() : nullptr;
    const SequenceId sequence = c ? c->currentSequenceId() : SequenceId{};
    if (snap != m_surveyOf || sequence != m_surveySequence) {
        m_surveyOf = snap;
        m_surveySequence = sequence;
        m_survey = snap && sequence.isValid() ? ai::stabilize::surveyStabilizers(*snap, sequence, true)
                                              : std::vector<ai::stabilize::StabilizerEntry>{};
    }
    return m_survey;
}

QVariantMap StabilizerBridge::overview() const {
    using ai::stabilize::StabilizerNeed;
    int notAnalyzed = 0, outOfDate = 0, current = 0, skipped = 0;
    for (const ai::stabilize::StabilizerEntry& e : survey()) {
        switch (e.need) {
        case StabilizerNeed::Current: ++current; break;
        case StabilizerNeed::NotAnalyzed: ++notAnalyzed; break;
        case StabilizerNeed::OutOfDate: ++outOfDate; break;
        case StabilizerNeed::Skipped: ++skipped; break;
        }
    }
    return {{QStringLiteral("count"), notAnalyzed + outOfDate + current},
            {QStringLiteral("pending"), notAnalyzed + outOfDate},
            {QStringLiteral("notAnalyzed"), notAnalyzed},
            {QStringLiteral("outOfDate"), outOfDate},
            {QStringLiteral("current"), current},
            {QStringLiteral("skipped"), skipped}};
}

QVariantMap StabilizerBridge::exportNeeds(const QString& range) const {
    using ai::stabilize::StabilizerNeed;
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    const ProjectSnapshot snap = c ? c->snapshot() : nullptr;
    std::optional<TimeRange> exported;
    if (seq && range == QLatin1String("inOut")) {
        exported = seq->markedRange();
    } else if (seq && range == QLatin1String("workArea")) {
        exported = seq->workArea;
    }
    QStringList names;
    for (const ai::stabilize::StabilizerEntry& e : survey()) {
        if (e.need != StabilizerNeed::NotAnalyzed && e.need != StabilizerNeed::OutOfDate) {
            continue;
        }
        // A clip of the sequence itself counts when the export reaches it; one inside a nest (any range) always does.
        if (e.depth == 0 && exported && (e.where.end() <= exported->start || e.where.start >= exported->end())) {
            continue;
        }
        QString where;
        if (const Sequence* owner = snap ? snap->findSequence(e.sequence) : nullptr) {
            if (const auto loc = owner->findClip(e.clip)) {
                const auto pos = owner->trackPosition(loc->track->id);
                // "V1: Interview at 00:00:12:00": the track, the clip and where it starts (clips share names).
                where = (pos ? (pos->kind == TrackKind::Video ? QStringLiteral("V") : QStringLiteral("A")) + QString::number(pos->index + 1)
                             + QStringLiteral(": ")
                             : QString())
                      + QString::fromStdString(loc->clip().name());
                if (c) {
                    where += tr(" at %1").arg(c->formatTimecode(loc->clip().start()));
                }
            }
        }
        names << (where.isEmpty() ? QString() : where + QLatin1Char(' ')) +
                     (e.warp ? tr("(Warp Stabilizer)") : tr("(Rolling Shutter Repair)"));
    }
    return {{QStringLiteral("count"), static_cast<int>(names.size())}, {QStringLiteral("names"), names}};
}

bool StabilizerBridge::analyzeAll(bool reanalyze) {
    using ai::stabilize::StabilizerNeed;
    ProjectController* c = controller();
    if (busy() || !c) {
        return false;
    }
    const auto& entries = survey();
    const bool anything = std::any_of(entries.begin(), entries.end(), [&](const ai::stabilize::StabilizerEntry& e) {
        return e.need != StabilizerNeed::Skipped && (reanalyze || e.need != StabilizerNeed::Current);
    });
    if (!anything) {
        m_message = entries.empty() ? tr("There is no Warp Stabilizer or Rolling Shutter Repair in this sequence")
                                    : tr("Every stabilizer is already analyzed");
        emit changed();
        return false;
    }
    std::shared_ptr<masks::TrackingServices> pictures = m_pictures;
    if (!pictures) {
        std::filesystem::path projectFile;
        if (QQmlEngine* engine = qmlEngine(this)) {
            if (auto* file = engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile")) {
                projectFile = file->document().file();
            }
        }
        pictures = std::make_shared<editor::MediaTrackingService>(projectFile);
    }
    const std::filesystem::path cacheRoot = m_cacheRoot.empty() ? ai::AnalysisCache::defaultRoot() : m_cacheRoot;
    m_section = QStringLiteral("*"); // the whole sequence: no header shows it as its own
    m_progress = 0.0;
    m_message = tr("Analyzing all stabilizers in background");
    emit changed();

    m_cancel = std::make_shared<std::atomic<bool>>(false);
    const std::shared_ptr<gyro::GyroServices> gyroServices = gyroFor(m_gyro, c, m_cancel);
    const ProjectSnapshot base = c->snapshot();
    const command::Json params{{"sequence", c->currentSequenceId().value}, {"reanalyze", reanalyze}};
    QPointer<StabilizerBridge> self(this);
    const bool synchronous = m_synchronous;
    const auto onProgress = [self, synchronous](const masks::TrackProgress& p) {
        const auto post = [self, p] {
            if (self && self->busy()) {
                self->m_progress = p.total > 0 ? static_cast<double>(p.frame) / p.total : 0.0;
                self->m_message = tr("Analyzing all stabilizers in background (frame %1 of %2)").arg(p.frame).arg(p.total);
                emit self->changed();
            }
        };
        if (synchronous) {
            post();
        } else {
            QMetaObject::invokeMethod(QCoreApplication::instance(), post, Qt::QueuedConnection);
        }
    };
    auto work = [base, pictures, gyroServices, onProgress, params, cacheRoot,
                 cancel = m_cancel]() -> std::pair<command::CommandResult, ProjectSnapshot> {
        EditSession scratch(*base);
        command::EnginePolicy policy;
        policy.services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
            std::make_shared<Forwarder>(pictures, onProgress), gyroServices,
            std::make_shared<ai::stabilize::StabilizationServices>(std::make_shared<ai::AnalysisCache>(cacheRoot))});
        command::CommandEngine engine(scratch, ai::edit::editingCommands(), policy);
        command::CommandRequest request;
        request.command = "effect.analyzeAll";
        request.params = params;
        CancelFlag control(cancel);
        command::CommandResult result = engine.execute(request, &control);
        return {std::move(result), scratch.snapshot()};
    };
    const auto done = [self, base, cancel = m_cancel](std::pair<command::CommandResult, ProjectSnapshot> outcome) {
        if (!self) {
            return;
        }
        const command::CommandResult& r = outcome.first;
        if (!r.ok()) {
            self->finishAll(false,
                            r.error->code == command::ErrorCode::Cancelled ? tr("Analysis cancelled")
                                                                           : tr("Could not analyze: %1").arg(QString::fromStdString(r.error->message)),
                            base, std::nullopt);
            return;
        }
        ProjectController* live = self->controller();
        if (cancel->load() || !live) {
            self->finishAll(false, tr("Analysis cancelled"), base, std::nullopt);
            return;
        }
        if (live->snapshot() != base) {
            self->finishAll(false, tr("The project changed while analyzing; analyze again"), base, std::nullopt);
            return;
        }
        const int analysed = r.data.value("analyzed", 0);
        QString message = tr("Analyzed %n stabilizer(s)", nullptr, analysed);
        if (r.data.contains("skipped") && !r.data["skipped"].empty()) {
            message += tr(", %n not analyzed", nullptr, static_cast<int>(r.data["skipped"].size()));
        }
        if (!r.warnings.empty()) {
            message += QStringLiteral(": ") + QString::fromStdString(r.warnings.front());
        }
        self->finishAll(true, message, base, *outcome.second);
    };
    if (synchronous) {
        done(work());
        return true;
    }
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_worker = std::jthread([work, done] {
        auto outcome = work();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [done, outcome = std::move(outcome)]() mutable { done(std::move(outcome)); },
                                  Qt::QueuedConnection);
    });
    return true;
}

void StabilizerBridge::finishAll(bool ok, const QString& message, const ProjectSnapshot&, std::optional<ProjectState> result) {
    m_section.clear();
    m_progress = ok ? 1.0 : 0.0;
    m_message = message;
    ProjectController* c = controller();
    if (ok && result && c) {
        // The analysed project from the worker's copy (the project did not change while it ran): one undo step.
        auto r = c->session().apply(tr("Analyze All Stabilizers").toStdString(),
                                    [&](const ProjectState&) -> std::expected<ProjectState, Error> { return *result; });
        if (!r) {
            m_message = tr("Could not apply the analysis: %1").arg(QString::fromStdString(r.error().message));
        }
    }
    if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id()) {
        m_worker.join();
    }
    emit changed();
}

} // namespace wabcut::app
