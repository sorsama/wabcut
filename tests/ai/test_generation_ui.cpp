#include "ShellFixture.h"
#include <atomic>
#include "AiController.h"
#include "GenerateController.h"
#include "ProjectController.h"
#include "TimelineFixture.h"
#include <wabcut/ai/generation/GenerationCommands.h>
#include <wabcut/ai/Provenance.h>
#include <wabcut/model/ProjectIO.h>
#include <catch2/catch_test_macros.hpp>
#include <wabcut/media/Probe.h>
#include <QFile>
#include <QImage>
#include <fstream>
#include <iterator>
using namespace wabcut;
using namespace wabcut::test;
using namespace wabcut::app;
TEST_CASE("Generate cost preview and local model page screenshots", "[ai][ui][generation][screenshot]") {
    qputenv("WABCUT_AI_KEYSTORE", "memory");
    for (const bool localPage : {false, true}) {
        resetSettings(); clearMessages(); Shell shell; INFO(join(shell.errors)); REQUIRE(shell.window);
        shell.theme->setProperty("reduceMotion", true); shell.resize(1600, 1100);
        auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController"); REQUIRE(ai);
        auto* generation = shell.engine.singletonInstance<GenerateController*>("Wabcut", "GenerateController"); REQUIRE(generation);
        if (localPage) {
            // 0.1.3: Settings > AI (the window opens it on that page: window.openAiPreferences).
            REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "openAiPreferences"));
            auto* settings = shell.object("settingsWindowLoader")->property("item").value<QObject*>(); REQUIRE(settings);
            std::function<QQuickItem*(QQuickItem*)> walk = [&](QQuickItem* item) -> QQuickItem* {
                if (!item || item->objectName() == QStringLiteral("aiPreferences")) return item;
                for (QQuickItem* child : item->childItems()) if (QQuickItem* found = walk(child)) return found;
                return nullptr;
            };
            QObject* preferences = walk(settings->property("contentItem").value<QQuickItem*>());
            REQUIRE(preferences);
            preferences->setProperty("currentTab", 4);
            REQUIRE(waitFor([&] { return preferences->property("opened").toBool(); }));
        } else {
            ai->setOfflineMode(false); ai->setProjectLock(false);
            generation->open();
            auto* prompt = shell.object("generationPrompt"); REQUIRE(prompt); prompt->setProperty("text", "A slow aerial view of the city at sunrise");
            auto cost = generation->estimate("veo-3.1-fast-generate-preview", "Aerial city", 4, "16:9");
            REQUIRE(cost.value("ok").toBool()); CHECK(cost.value("dollars").toDouble() == 0.4);
            auto* run = shell.object("generationRun"); REQUIRE(run); CHECK_FALSE(run->property("enabled").toBool());
            auto* project = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController"); REQUIRE(project);
            Timeline tl("V1 tgt |Aaaaaaaaaaaaaaaaaaaa....|\nA1 tgt |Aaaaaaaaaaaaaaaaaaaa....|");
            MediaItem asset = *tl.state.findMedia(tl.media('A')); asset.id = {}; asset.name = "AI-generated city extension";
            asset.generated = GeneratedProvenance{"runway", "gen4.5", ai::promptHash("Aerial city"), "2026-10-11T12:00:00Z", "video", "extend-tail", {}};
            auto staged = ai::generation::stageAsset(tl.state, tl.seq, asset, Ticks::fromSeconds(4), {}, tl.clip("V1@0")); REQUIRE(staged);
            auto saved = io::toJson(staged->state); REQUIRE(saved);
            REQUIRE(io::writeFileAtomically(std::filesystem::path(outputDir().toStdWString()) / "ai-extension-fixture.wabcut", *saved));
            project->resetProject(staged->state, tl.seq);
        }
        spin(500);
        // Keep earlier test autosaves intact; dismiss their recovery prompt as "Later" for this capture.
        if (auto* recovery = shell.object("recoveryDialog")) {
            REQUIRE(QMetaObject::invokeMethod(recovery, "close"));
            spin(100);
            REQUIRE_FALSE(recovery->property("opened").toBool());
        }
        auto image = shell.grab(); REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(outputDir() + (localPage ? "/ai-local-models.png" : "/ai-generate-cost-extension.png")));
        INFO(join(takeMessages()));
    }
}
#include <wabcut/export/ExportJob.h>
#include <wabcut/model/ProjectOps.h>
namespace {
class FixtureGenerator final : public ai::IMediaGenerator {
public:
    ai::ModelSpec model;
    std::filesystem::path fixture;
    std::shared_ptr<std::atomic<int>> submissions;
    const ai::ModelSpec& spec() const override { return model; }
    std::expected<ai::GenerationTicket, ai::AiError> create(const ai::MediaGenerationRequest&, std::stop_token) override {
        ++*submissions; return ai::GenerationTicket{model.provider, model.id, "fixture", {}, {}, {}};
    }
    std::expected<ai::GenerationStatus, ai::AiError> poll(const ai::GenerationTicket&, std::stop_token) override {
        ai::GenerationStatus result; result.state = ai::GenerationState::Succeeded; return result;
    }
    std::expected<std::vector<std::filesystem::path>, ai::AiError> fetch(const ai::GenerationStatus&, const std::filesystem::path& dir, std::stop_token) override {
        auto target = dir / "provider-fixture.mp4"; std::filesystem::copy_file(fixture, target);
        return std::vector<std::filesystem::path>{target};
    }
};
}
TEST_CASE("Generate app workflow requires consent and acceptance before a single undoable import", "[ai][ui][generation]") {
    qputenv("WABCUT_AI_KEYSTORE", "memory"); resetSettings(); clearMessages(); Shell shell; REQUIRE(shell.window);
    auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController"); REQUIRE(ai);
    auto* generation = shell.engine.singletonInstance<GenerateController*>("Wabcut", "GenerateController"); REQUIRE(generation);
    auto* project = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController"); REQUIRE(project);
    edit::SequenceSettings settings; settings.width = 160; settings.height = 90; settings.frameRate = kRate25;
    auto added = edit::addSequence({}, settings); REQUIRE(added);
    auto timed = edit::setSequenceDuration(added->state, added->id, Ticks::fromSeconds(2)); REQUIRE(timed);
    auto file = std::filesystem::path(outputDir().toStdWString()) / "generated-source-fixture.mp4";
    exporter::ExportRequest request; request.snapshot = std::make_shared<const ProjectState>(timed->state);
    request.sequence = added->id; request.output = file; request.settings.audio.enabled = false;
    request.settings.video.encoder = "libx264"; request.videoSource = exporter::syntheticVideoSourceFactory();
    REQUIRE(exporter::ExportJob(std::move(request)).run());
    project->resetProject(timed->state, added->id);
    auto count = std::make_shared<std::atomic<int>>(0);
    ai->platform().providers().registerMediaGenerator("runway-generation", [file, count](ai::ProviderContext context) {
        auto fake = std::make_unique<FixtureGenerator>(); fake->fixture = file; fake->model = context.model; fake->submissions = count; return fake;
    });
    ai->saveKey("runway", "fixture-only-secret", true); ai->setOfflineMode(false); ai->setProjectLock(false);
    const auto before = project->snapshot();
    generation->run("gen4.5", "clouds", 2, "16:9", 0.24, false, 0);
    CHECK_FALSE(generation->busy()); CHECK(count->load() == 0); CHECK(project->snapshot() == before);
    generation->run("gen4.5", "clouds", 2, "16:9", 0.24, true, 0);
    REQUIRE(waitFor([&] { return !generation->busy(); }, 30000));
    INFO(generation->message().toStdString()); REQUIRE(generation->staged());
    CHECK(count->load() == 1); CHECK(project->snapshot() == before);
    generation->accept(); REQUIRE(project->snapshot()->media.size() == 1);
    REQUIRE(project->snapshot()->media[0]->generated);
    CHECK(project->snapshot()->media[0]->generated->promptHash == ai::promptHash("clouds"));
    auto saved = io::toJson(project->session().stateForSave()); REQUIRE(saved);
    REQUIRE(io::writeFileAtomically(std::filesystem::path(outputDir().toStdWString()) / "ai-generated-export-fixture.wabcut", *saved));
    REQUIRE(project->session().undo()); CHECK(project->snapshot()->media.empty());
}

namespace {
/// Records the conditioning picture a provider would receive.
class CapturingGenerator final : public ai::IMediaGenerator {
public:
    ai::ModelSpec model;
    std::filesystem::path fixture;
    std::shared_ptr<std::atomic<int>> submissions;
    std::shared_ptr<std::string> uploaded;
    const ai::ModelSpec& spec() const override { return model; }
    std::expected<ai::GenerationTicket, ai::AiError> create(const ai::MediaGenerationRequest& request, std::stop_token) override {
        ++*submissions;
        std::ifstream in(request.firstFrame, std::ios::binary);
        *uploaded = std::string(std::istreambuf_iterator<char>(in), {});
        return ai::GenerationTicket{model.provider, model.id, "fixture", {}, {}, {}};
    }
    std::expected<ai::GenerationStatus, ai::AiError> poll(const ai::GenerationTicket&, std::stop_token) override {
        ai::GenerationStatus result; result.state = ai::GenerationState::Succeeded; return result;
    }
    std::expected<std::vector<std::filesystem::path>, ai::AiError> fetch(const ai::GenerationStatus&, const std::filesystem::path& dir, std::stop_token) override {
        auto target = dir / "provider-fixture.mp4"; std::filesystem::copy_file(fixture, target);
        return std::vector<std::filesystem::path>{target};
    }
};
}
// Review wave 4, P1-1: Generative Extend used to upload the raw decoded source frame, so a face redacted everywhere
// else went to the provider in clear. The frame is now rendered like an export (the clip alone, redactions applied),
// shown before consent, sent byte for byte as shown, and a redacted clip needs an explicit acknowledgement.
TEST_CASE("Generative Extend sends the reviewed, redacted boundary frame only", "[ai][ui][generation]") {
    qputenv("WABCUT_AI_KEYSTORE", "memory"); resetSettings(); clearMessages(); Shell shell; REQUIRE(shell.window);
    auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController"); REQUIRE(ai);
    auto* generation = shell.engine.singletonInstance<GenerateController*>("Wabcut", "GenerateController"); REQUIRE(generation);
    auto* project = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController"); REQUIRE(project);
    // A 2 s synthetic source file, its clip on V1 at 0, and a solid magenta redaction over the middle of the picture.
    edit::SequenceSettings settings; settings.width = 320; settings.height = 180; settings.frameRate = kRate25;
    auto blank = edit::addSequence({}, settings); REQUIRE(blank);
    auto timed = edit::setSequenceDuration(blank->state, blank->id, Ticks::fromSeconds(2)); REQUIRE(timed);
    auto file = std::filesystem::path(outputDir().toStdWString()) / "extend-source-fixture.mp4";
    exporter::ExportRequest render; render.snapshot = std::make_shared<const ProjectState>(timed->state);
    render.sequence = blank->id; render.output = file; render.settings.audio.enabled = false;
    render.settings.video.encoder = "libx264"; render.videoSource = exporter::syntheticVideoSourceFactory();
    REQUIRE(exporter::ExportJob(std::move(render)).run());
    auto info = media::probe(file.string()); REQUIRE(info);
    MediaItem item; item.name = "Interview"; item.locator.path = file.string(); item.info = *info;
    auto added = edit::addSequence({}, settings); REQUIRE(added);
    auto withMedia = edit::addMedia(added->state, item); REQUIRE(withMedia);
    const Sequence& seq = *withMedia->state.findSequence(added->id);
    edit::SourceClip source; source.media = withMedia->id; source.sourceRange = TimeRange{Ticks{}, Ticks::fromSeconds(1)};
    source.patches = {{StreamRef{StreamKind::Video, 0}, seq.videoTracks.front()->id}};
    auto placed = edit::overwriteEdit(withMedia->state, added->id, source, Ticks{}); REQUIRE(placed);
    Redaction r; r.media = withMedia->id; r.source = TimeRange{Ticks{}, Ticks::fromSeconds(2)}; r.style = RedactionStyle::Solid;
    r.colorR = 1.0; r.colorB = 1.0; r.region.shape = MaskShape::Ellipse; r.region.set("size.x", 0.4); r.region.set("size.y", 0.4);
    auto redacted = edit::addRedaction(placed->state, r); REQUIRE(redacted);
    project->resetProject(redacted->state, added->id);
    const ClipId clip = placed->placed.front();
    const QString key = QStringLiteral("c%1").arg(clip.value);

    auto count = std::make_shared<std::atomic<int>>(0);
    auto uploaded = std::make_shared<std::string>();
    ai->platform().providers().registerMediaGenerator("runway-generation", [file, count, uploaded](ai::ProviderContext context) {
        auto fake = std::make_unique<CapturingGenerator>(); fake->fixture = file; fake->model = context.model;
        fake->submissions = count; fake->uploaded = uploaded; return fake;
    });
    ai->saveKey("runway", "fixture-only-secret", true); ai->setOfflineMode(false); ai->setProjectLock(false);

    // Without a reviewed picture nothing is sent.
    generation->run("gen4.5", "continue", 2, "16:9", 0.24, true, -1, key, false, true);
    CHECK_FALSE(generation->busy()); CHECK(count->load() == 0);
    // The picture to send: rendered with the redaction (magenta in the middle), shown before consent.
    REQUIRE(generation->prepareExtend(key, false).isEmpty());
    REQUIRE_FALSE(generation->uploadPreview().isEmpty());
    CHECK_FALSE(generation->redactionNotice().isEmpty());
    const QImage preview(generation->uploadPreview().toLocalFile());
    REQUIRE_FALSE(preview.isNull());
    const QColor middle = preview.pixelColor(preview.width() / 2, preview.height() / 2);
    INFO(middle.name().toStdString());
    CHECK(middle.red() > 200); CHECK(middle.green() < 60); CHECK(middle.blue() > 200);
    // A redacted clip needs the acknowledgement.
    generation->run("gen4.5", "continue", 2, "16:9", 0.24, true, -1, key, false, false);
    CHECK_FALSE(generation->busy()); CHECK(count->load() == 0);
    generation->run("gen4.5", "continue", 2, "16:9", 0.24, true, -1, key, false, true);
    REQUIRE(waitFor([&] { return !generation->busy(); }, 30000));
    INFO(generation->message().toStdString());
    CHECK(count->load() == 1);
    QFile shown(generation->uploadPreview().toLocalFile()); REQUIRE(shown.open(QIODevice::ReadOnly));
    const QByteArray expected = shown.readAll();
    CHECK(*uploaded == std::string(expected.constData(), static_cast<std::size_t>(expected.size()))); // byte for byte
}
