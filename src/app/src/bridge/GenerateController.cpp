#include "GenerateController.h"
#include "AiController.h"
#include "ProjectController.h"
#include <wabcut/ai/generation/Conform.h>
#include <wabcut/model/RedactionMedia.h>
#include <wabcut/ai/Provenance.h>
#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/Sha256.h>
#include <wabcut/ai/llm/Tiers.h>
#include <wabcut/editor/ColorFrames.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/ProjectOps.h>
#include <QFile>
#include <QImage>
#include <QQmlEngine>
#include <QUrl>
#include <QUuid>
#include <QPointer>
#include <cmath>
namespace wabcut::app {
namespace {
using namespace ai;
MediaGenerationRequest requestFor(const ModelSpec& model, const QString& prompt, int seconds, const QString& aspect, bool loop = false) {
    MediaGenerationRequest r; r.prompt = prompt.toStdString(); r.aspectRatio = aspect.toStdString();
    if (model.generation.soundEffect) { // 0.2: a sound effect; 0 seconds = the model picks the length
        r.kind = GeneratedMediaKind::SoundEffect; r.aspectRatio.clear(); r.resolution.clear(); r.loop = loop;
        if (seconds > 0) r.duration = Ticks::fromSeconds(seconds);
        return r;
    }
    r.kind = model.generation.image ? GeneratedMediaKind::Image : GeneratedMediaKind::Video;
    r.resolution = "720p"; if (!model.generation.image) r.duration = Ticks::fromSeconds(seconds);
    return r;
}
QString qs(const std::string& s) { return QString::fromStdString(s); }
bool thumbnail(const media::Thumbnail& frame, const std::filesystem::path& path) {
    QImage image(frame.rgba.data(), frame.width, frame.height, frame.width * 4, QImage::Format_RGBA8888);
    return image.save(QString::fromStdWString(path.wstring()), "PNG");
}
}
namespace {
std::string fileSha256(const std::filesystem::path& file) {
    QFile in(QString::fromStdWString(file.wstring()));
    if (!in.open(QIODevice::ReadOnly)) return {};
    ai::Sha256 hash;
    while (!in.atEnd()) {
        const QByteArray chunk = in.read(1 << 16);
        hash.update(std::string_view(chunk.constData(), static_cast<std::size_t>(chunk.size())));
    }
    return hash.finishHex();
}
}
GenerateController::GenerateController(QObject* parent) : QObject(parent) {}
GenerateController::~GenerateController() { cancel(); if (m_worker.joinable()) m_worker.join(); }
void GenerateController::setAiController(AiController* ai) {
    if (m_ai) disconnect(m_ai, nullptr, this, nullptr);
    m_ai = ai;
    // The worker calls m_ai->platform(): it must end before AiController tears the platform down (review wave 4, P2-4).
    if (m_ai) connect(m_ai, &AiController::shuttingDown, this, [this] { cancel(); if (m_worker.joinable()) m_worker.join(); });
}
QUrl GenerateController::uploadPreview() const {
    return m_upload ? QUrl::fromLocalFile(QString::fromStdWString(m_upload->file.wstring())) : QUrl();
}
QString GenerateController::prepareExtend(const QString& clipKey, bool head) {
    m_upload.reset(); m_redactionNotice.clear();
    const auto finish = [this](QString error) { emit uploadChanged(); return error; };
    if (!m_project) return finish(tr("No project"));
    const auto sequence = m_project->currentSequence();
    if (!sequence) return finish(tr("Open a sequence first"));
    QString numeric = clipKey; if (numeric.startsWith('c')) numeric.remove(0, 1);
    bool ok = false; const auto value = numeric.toULongLong(&ok);
    const auto location = ok ? sequence->findClip(ClipId{value}) : std::nullopt;
    if (!location || location->clip().stream.kind != StreamKind::Video) return finish(tr("Choose a video clip"));
    const Clip& clip = location->clip();
    // The picture an export would show at the boundary, with only this clip: redactions, masks and the grade apply
    // exactly as they do everywhere else; the raw decoded frame never leaves this computer (review wave 4, P1-1).
    const ProjectState state = m_project->session().stateForSave();
    const Ticks at = head ? clip.start() : clip.end() - framesToTicks(1, sequence->frameRate);
    auto rendered = editor::renderSequenceFrame(editor::soloClipState(state, sequence->id, clip.id), sequence->id, at,
                                                std::min(sequence->width, 1920), m_project->projectFile());
    if (!rendered) return finish(qs(rendered.error().message));
    const auto directory = ai::userCacheDir() / "generated" / ("pending-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
    std::error_code ec; std::filesystem::create_directories(directory, ec);
    if (ec) return finish(tr("Cannot create generation directory"));
    const auto file = directory / "boundary.png";
    QImage image(rendered->rgba.data(), rendered->width, rendered->height, rendered->width * 4, QImage::Format_RGBA8888);
    if (!image.save(QString::fromStdWString(file.wstring()), "PNG")) return finish(tr("Cannot save boundary frame"));
    PendingUpload upload;
    upload.clip = clip.id; upload.head = head; upload.file = file; upload.revision = m_project->session().revision();
    upload.sha256 = fileSha256(file);
    const TimeRange used = TimeRange::fromStartEnd(clip.sourceIn, clip.sourceOut());
    for (const Redaction* r : redactionsCovering(state, clip.media)) { // a subclip's master's too (review wave 5)
        if (r->enabled && r->appliesTo(clip.id) && r->source.overlaps(used)) upload.redacted = true;
    }
    if (upload.redacted) {
        m_redactionNotice = tr("This clip has redactions. The picture above is what will be sent; the generated extension is new media that the redactions do not cover. Confirm before sending.");
    }
    m_upload = std::move(upload);
    return finish(QString());
}
void GenerateController::runWorkerForTests(std::function<void(std::stop_token)> work) {
    if (m_worker.joinable()) m_worker.join();
    m_worker = std::jthread([work = std::move(work)](std::stop_token stop) { work(stop); });
}
GenerateController* GenerateController::create(QQmlEngine* engine, QJSEngine*) {
    auto* self = new GenerateController;
    self->setAiController(engine->singletonInstance<AiController*>("Wabcut", "AiController"));
    self->m_project = engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    return self;
}
QVariantList GenerateController::models() const {
    QVariantList list;
    for (const auto* m : ai::ModelRegistry::builtin().modelsWith(ai::Capability::GenerateMedia))
        list.push_back(QVariantMap{{"id", qs(m->id)}, {"name", qs(m->name)}, {"provider", qs(m->provider)}, {"image", m->generation.image},
            {"sound", m->generation.soundEffect}, {"loop", m->generation.loop}, {"maxSeconds", m->generation.maxSeconds}});
    return list;
}
QVariantMap GenerateController::estimate(const QString& modelId, const QString& prompt, int seconds, const QString& aspect, bool loop) const {
    const auto* model = m_ai ? m_ai->platform().providers().models().model(modelId.toStdString()) : nullptr;
    if (!model) return {{"ok", false}, {"error", tr("Choose a generation model")}};
    if (model->generation.soundEffect) {
        if (seconds < 0 || seconds > model->generation.maxSeconds || (seconds > 0 && seconds < model->generation.minSeconds))
            return {{"ok", false}, {"error", tr("Choose up to %1 seconds (0: the model picks)").arg(model->generation.maxSeconds)}};
    } else if (seconds < 1 || seconds > 10) {
        return {{"ok", false}, {"error", tr("Choose 1 to 10 seconds")}};
    }
    auto p = m_ai->platform().preflightGeneration(model->id, requestFor(*model, prompt, seconds, aspect, loop));
    if (!p) return {{"ok", false}, {"error", qs(p.error().message)}};
    return {{"ok", p->cost.priced && p->consent != ai::ConsentState::Blocked}, {"cost", qs(ai::formatUsd(p->cost.ceiling))},
        {"dollars", static_cast<double>(p->cost.ceiling) / 1000000.0}, {"provider", qs(p->candidate.provider->name)},
        {"terms", qs(p->candidate.provider->termsUrl)}, {"dataPolicy", qs(p->candidate.provider->dataPolicy)},
        {"seconds", model->generation.soundEffect ? ai::soundEffectSeconds(*model, requestFor(*model, prompt, seconds, aspect, loop))
                                                  : static_cast<double>(ai::generatedSeconds(model->generation, Ticks::fromSeconds(seconds)))},
        {"error", p->consent == ai::ConsentState::Blocked ? tr("Offline / no-cloud is enabled") : QString{}}};
}
void GenerateController::run(const QString& id, const QString& prompt, int seconds, const QString& aspect,
    double maxDollars, bool consentOnce, double atSeconds, const QString& clipKey, bool head, bool acknowledgeRedactions, bool loop) {
    if (m_busy || !m_ai || !m_project) return;
    const QString effectivePrompt = !clipKey.isEmpty() && prompt.trimmed().isEmpty() ? QStringLiteral("Continue this shot naturally.") : prompt;
    auto preview = estimate(id, effectivePrompt, seconds, aspect, loop);
    if (!preview.value("ok").toBool() || !consentOnce || !std::isfinite(maxDollars) || maxDollars < 0 || maxDollars > 1000 ||
        !std::isfinite(atSeconds) || atSeconds > 86400) { m_message = tr("Review the cost and consent before running"); emit changed(); return; }
    // Cloud keys are never read from a plaintext fallback for generative media.
    if (dynamic_cast<ai::PlaintextFileSecretStore*>(&m_ai->platform().keys().store())) {
        m_message = tr("Generative media requires the system keychain or session keys"); emit changed(); return;
    }
    const auto* model = m_ai->platform().providers().models().model(id.toStdString());
    auto request = requestFor(*model, effectivePrompt, seconds, aspect, loop);
    const auto sequence = m_project->currentSequence();
    if (!sequence) { m_message = tr("Open a sequence first"); emit changed(); return; }
    std::optional<ClipId> extend;
    if (!clipKey.isEmpty()) {
        QString numeric = clipKey; if (numeric.startsWith('c')) numeric.remove(0, 1);
        bool ok = false; const auto value = numeric.toULongLong(&ok);
        if (!ok || !sequence->findClip(ClipId{value}) || model->generation.image || model->generation.soundEffect) { m_message = tr("Choose a video clip"); emit changed(); return; }
        extend = ClipId{value};
        // Validate placement before any upload or charge.
        MediaItem probe; probe.name = "Generation placement check";
        probe.generated = GeneratedProvenance{};
        probe.generated->provider = "preview"; probe.generated->model = "preview";
        probe.generated->promptHash = std::string(64, '0'); probe.generated->createdAt = "2026-10-11T00:00:00Z";
        probe.generated->kind = "video"; probe.generated->operation = "text-to-video";
        probe.info.video.resize(1); probe.info.video[0].index = 0;
        probe.info.video[0].width = sequence->width; probe.info.video[0].height = sequence->height;
        probe.info.video[0].frameRate = sequence->frameRate;
        probe.info.duration = Ticks::fromSeconds(seconds);
        auto valid = ai::generation::stageAsset(m_project->session().stateForSave(), sequence->id,
            std::move(probe), Ticks::fromSeconds(seconds), {}, extend, head);
        if (!valid) { m_message = qs(valid.error().message); emit changed(); return; }
        // Only the reviewed picture is sent: prepared for this clip and side, from the project as it is now, unchanged
        // on disk since (review wave 4, P1-1).
        if (!m_upload || m_upload->clip != *extend || m_upload->head != head || m_upload->revision != m_project->session().revision() ||
            fileSha256(m_upload->file) != m_upload->sha256) {
            (void)prepareExtend(clipKey, head);
            m_message = tr("Review the picture that will be sent, then approve again"); emit changed(); return;
        }
        if (m_upload->redacted && !acknowledgeRedactions) {
            m_message = tr("Confirm that the extension may be generated from a redacted clip"); emit changed(); return;
        }
    }
    if (m_worker.joinable()) m_worker.join();
    m_base = m_project->snapshot(); m_staged.reset(); m_results.clear(); m_progress = 0; m_busy = true;
    m_message = tr("Preparing generation"); emit changed();
    const ProjectState original = m_project->session().stateForSave();
    const auto projectFile = m_project->projectFile();
    const auto directory = ai::userCacheDir() / "generated" / QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    (void)m_ai->transport();
    const auto cap = static_cast<ai::MicroUsd>(std::llround(maxDollars * 1000000.0));
    const std::filesystem::path reviewed = extend && m_upload ? m_upload->file : std::filesystem::path();
    m_worker = std::jthread([this, original, sequence, request, directory, projectFile, id, seconds, atSeconds, extend, head, cap, reviewed](std::stop_token stop) mutable {
        const auto update = [this](QString message, double progress) { QMetaObject::invokeMethod(this, [this, message, progress] { m_message = message; m_progress = progress; emit changed(); }, Qt::QueuedConnection); };
        const auto fail = [this](const std::string& message) { QMetaObject::invokeMethod(this, [this, message] { m_busy = false; m_message = qs(message); emit changed(); }, Qt::QueuedConnection); };
        try {
        std::error_code ec; std::filesystem::create_directories(directory, ec);
        if (ec) { fail("Cannot create generation directory"); return; }
        if (extend) {
            // The exact picture the user reviewed (rendered through the compositor by prepareExtend), nothing else.
            request.firstFrame = directory / "boundary.png";
            std::filesystem::copy_file(reviewed, request.firstFrame, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) { fail("Cannot prepare the reviewed boundary frame"); return; }
        }
        ai::GenerationEvents events;
        events.onStatus = [update](const ai::GenerationStatus& status) { update(qs(std::string(ai::toString(status.state))), status.progress * 0.7); };
        auto result = m_ai->platform().generate(id.toStdString(), request, directory, events, stop, {true, cap});
        if (!result) { fail(result.error().message); return; }
        if (result->files.empty()) { fail("Provider returned no generated asset"); return; }
        auto assetPath = result->files.front();
        Ticks duration = Ticks::fromSeconds(seconds);
        auto info = media::probe(pathToUtf8(assetPath));
        if (!info) { fail(info.error().message); return; }
        const bool sound = request.kind == ai::GeneratedMediaKind::SoundEffect;
        if (sound) { // 0.2: the sound effect as it came (its own length: the model may have picked it)
            if (info->audio.empty() || info->duration.value <= 0) { fail("The provider sent no playable sound"); return; }
            duration = info->duration;
        }
        if (request.kind == ai::GeneratedMediaKind::Video) {
            update(tr("Matching the sequence format"), 0.75);
            auto conformedPath = directory / "conformed.mp4";
            auto conformed = ai::generation::conformVideo(assetPath, conformedPath, *sequence, duration, extend && head, stop,
                [update](float progress) { update(QObject::tr("Matching the sequence format"), 0.75 + progress * 0.2); });
            if (!conformed) { fail(conformed.error().message); return; }
            info = std::move(conformed); assetPath = conformedPath;
        }
        MediaItem asset; asset.name = (sound ? "AI sound effect " : "AI-generated ") + id.toStdString(); asset.locator.path = pathToUtf8(assetPath); asset.info = *info;
        asset.locator.relativePath = relativeMediaPath(projectFile, assetPath);
        auto fingerprint = fingerprintFile(assetPath);
        if (!fingerprint) { fail(fingerprint.error().message); return; }
        asset.locator.fingerprint = *fingerprint;
        GeneratedProvenance provenance; provenance.provider = result->provider; provenance.model = result->model;
        provenance.promptHash = ai::promptHash(request.prompt); provenance.createdAt = result->createdAt;
        provenance.kind = std::string(ai::toString(request.kind));
        provenance.operation = extend ? (head ? "extend-head" : "extend-tail") : request.kind == ai::GeneratedMediaKind::Image ? "text-to-image"
                               : sound ? "text-to-sound-effect" : "text-to-video";
        if (extend) provenance.source = sequence->findClip(*extend)->clip().media;
        asset.generated = provenance;
        if (auto wrote = ai::writeProvenanceSidecars(assetPath, provenance); !wrote) { fail(wrote.error().message); return; }
        const auto at = atSeconds >= 0 && request.kind != ai::GeneratedMediaKind::Image ? std::optional<Ticks>(Ticks{static_cast<std::int64_t>(atSeconds * kTicksPerSecond)}) : std::nullopt;
        auto stage = ai::generation::stageAsset(original, sequence->id, std::move(asset), duration, at, extend, head);
        if (!stage) { fail(stage.error().message); return; }
        auto picture = directory / "preview.png";
        if (request.kind == ai::GeneratedMediaKind::Image) picture = assetPath;
        else if (sound) picture.clear(); // nothing to look at: "Open preview" plays it
        else if (auto frame = ai::generation::boundaryFrame(pathToUtf8(assetPath), Ticks{}, stop)) (void)thumbnail(*frame, picture);
        if (stop.stop_requested()) { fail("Generation cancelled"); return; }
        auto state = std::make_shared<const ProjectState>(std::move(stage->state));
        QMetaObject::invokeMethod(this, [this, state, picture, assetPath, stop, cost = result->cost] {
            if (stop.stop_requested()) { m_busy = false; m_message = tr("Generation cancelled"); emit changed(); return; }
            m_staged = state; m_busy = false; m_progress = 1; m_message = tr("Preview ready — accept to import in one undo step");
            m_results = {QVariantMap{{"preview", picture.empty() ? QUrl() : QUrl::fromLocalFile(QString::fromStdWString(picture.wstring()))},
                {"file", QUrl::fromLocalFile(QString::fromStdWString(assetPath.wstring()))}, {"cost", qs(ai::formatUsd(cost))}, {"aiGenerated", true},
                {"sound", picture.empty()}}};
            emit changed();
        }, Qt::QueuedConnection);
        } catch (const std::exception&) { fail("Generation stopped after an internal error"); }
        catch (...) { fail("Generation stopped after an internal error"); }
    });
}
void GenerateController::cancel() { if (m_worker.joinable()) m_worker.request_stop(); }
void GenerateController::accept() {
    if (!m_staged || !m_project || m_busy) return;
    if (m_project->snapshot() != m_base) { m_message = tr("The project changed. Generate a new preview before applying."); emit changed(); return; }
    auto result = m_project->session().commit("Import generated media", *m_staged);
    m_message = result ? tr("Generated media imported") : qs(result.error().message);
    if (result) { m_staged.reset(); m_base.reset(); } emit changed();
}
void GenerateController::discard() { if (!m_busy) { m_staged.reset(); m_base.reset(); m_results.clear(); emit changed(); } }
void GenerateController::openExtend(const QString& clip) { emit showRequested(clip); }
void GenerateController::open() { emit showRequested({}); }
namespace {
class ProposalServices final : public ai::generation::GenerationServices {
public:
    QPointer<GenerateController> owner;
    QPointer<AiController> ai;
    std::string status;
    ProjectSnapshot base;
    command::CommandOutput generate(std::string_view operation, command::ExecutionContext& context) override {
        if (!owner) command::fail(command::ErrorCode::InvalidRequest, "Generate is unavailable");
        QVariantMap p{{"operation", qs(std::string(operation))}, {"prompt", qs(context.string("prompt"))},
            {"aspect", qs(context.string("aspect", "16:9"))}, {"head", context.string("side") == "head"},
            {"clip", operation == "extend" ? QStringLiteral("c%1").arg(context.integer("clip")) : QString{}},
            {"seconds", operation == "image" ? 4 : operation == "sfx" ? (context.has("duration") ? static_cast<int>(std::ceil(context.number("duration"))) : 0)
                                                 : static_cast<int>(std::ceil(context.number(operation == "extend" ? "seconds" : "duration")))},
            {"loop", operation == "sfx" && context.boolean("loop", false)},
            {"at", context.has("at") ? static_cast<double>(context.integer("at")) / kTicksPerSecond : -1.0}};
        const auto sequence = context.sequenceId();
        p["sequence"] = QString::number(sequence.value);
        auto target = owner; auto expected = base;
        QMetaObject::invokeMethod(target, [target, p, expected] {
            // The GUI checks its current sequence again; an agent cannot authorize uploads or charges.
            if (target) target->propose(p, expected);
        }, Qt::QueuedConnection);
        command::CommandOutput out; out.state = context.state();
        out.data = {{"status", "approval-required"}, {"operation", operation}, {"sequence", sequence.value},
            {"message", "Review the prefilled Generate panel, provider, cost and consent; no media was generated or imported."}};
        return out;
    }
    command::Json local(std::string_view operation, const command::Json& params) override {
        if (!ai) return {{"state", "unavailable"}};
        if (operation == "status") return {{"message", status}, {"snapshot", true}};
        std::string id = params.value("model", std::string());
        if (id.empty()) id = std::string(ai::llm::defaultModel(ai::llm::probeSystem())); // AI-15: the recommended tier
        auto target = ai; const auto model = qs(id);
        QMetaObject::invokeMethod(target, [target, model, start = operation == "start"] {
            if (target) { if (start) target->startLocal(model); else target->stopLocal(); }
        }, Qt::QueuedConnection);
        return {{"requestQueued", true}, {"operation", operation}};
    }
};
}
void GenerateController::propose(const QVariantMap& p, ProjectSnapshot expected) {
    if (m_busy || !m_project || m_project->snapshot() != expected ||
        QString::number(m_project->currentSequenceId().value) != p.value("sequence").toString()) {
        m_message = tr("The project changed; request a new generation proposal"); emit changed(); return;
    }
    emit proposalRequested(p);
}
std::shared_ptr<command::CommandServices> GenerateController::services() {
    auto service = std::make_shared<ProposalServices>();
    service->owner = this; service->ai = m_ai;
    service->base = m_project ? m_project->snapshot() : ProjectSnapshot{};
    service->status = m_ai ? m_ai->localStatus().toStdString() : "unavailable";
    return service;
}

}
