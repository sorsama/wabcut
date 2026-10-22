#include "StabilizerBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/stabilize/StabilizeCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/editor/Tracking.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MotionOps.h>

#include <QCoreApplication>
#include <QPointer>
#include <QQmlEngine>

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

} // namespace

struct StabilizerBridge::Found {
    SequenceId sequence;
    ClipId clip;
    EffectInstance effect;
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
    if (!id || !seq) {
        return std::nullopt;
    }
    for (const TrackPtr& track : seq->videoTracks) {
        for (const Clip& clip : track->clips) {
            for (const EffectInstance& e : clip.effects()) {
                if (e.id == *id) {
                    return Found{seq->id, clip.id, e};
                }
            }
        }
    }
    return std::nullopt;
}

QVariantMap StabilizerBridge::effectInfo(const QString& section) const {
    const auto found = find(section);
    if (!found || !isStabilizer(found->effect.effect)) {
        return {{QStringLiteral("stabilizer"), false}};
    }
    const EffectData* status = found->effect.findData("status");
    return {{QStringLiteral("stabilizer"), true},
            {QStringLiteral("kind"), found->effect.effect == kWarpStabilizerEffect ? QStringLiteral("warp") : QStringLiteral("shutter")},
            {QStringLiteral("status"), status && status->text() ? QString::fromStdString(*status->text()) : QString()}};
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
    auto work = [base, pictures, onProgress, params, command, cacheRoot,
                 cancel = m_cancel]() -> std::pair<command::CommandResult, ProjectSnapshot> {
        EditSession scratch(*base);
        command::EnginePolicy policy;
        policy.services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
            std::make_shared<Forwarder>(pictures, onProgress),
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
        if (loc) {
            for (const EffectInstance& candidate : loc->clip().effects()) {
                e = candidate.id == effect ? &candidate : e;
            }
        }
        if (e) {
            edit::EffectChange change;
            change.params = e->params;
            change.data = e->data;
            const QString label = e->effect == kWarpStabilizerEffect ? tr("Analyze Stabilization") : tr("Analyze Rolling Shutter");
            auto r = c->session().apply(label.toStdString(),
                                        [&](const ProjectState& s) { return edit::setEffect(s, seq, clip, effect, change); });
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

} // namespace wabcut::app
