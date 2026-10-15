// AI-12 in the app, on the UI shell with fixtures (a dictionary translator, a scripted voice provider; no model, no
// network): the "Translate captions" dialog stages Thai and Japanese tracks with the report per language and adds them
// as one undo step; the "Dub" dialog estimates, generates, shows the timing-fit report and adds a Dialogue "Dub" track
// with the original dialogue ducked, marked AI-generated on the timeline. The dialogs are made from the Wabcut module
// directly (their Captions-workspace hooks are Main.qml's). Screenshots: <build>/test-output/ui/translate-captions.png,
// dub-timing-report.png and dub-track-badge.png.

#include "ShellFixture.h"

#include "AiController.h"
#include "GlobalBridge.h"
#include "ProjectController.h"
#include "TimelineFixture.h"
#include "global/GlobalTestUtil.h"

#include <wabcut/ai/dub/DubCommands.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/translate/TranslateCommands.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <QQmlComponent>
#include <QQuickItem>
#include <QUrl>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using namespace wabcut::ai::global::test;

namespace {

void loadInto(ProjectController* controller, const ProjectState& state, const QString& name) {
    const QString path = outputDir() + "/" + name;
    REQUIRE(io::saveProject(state, std::filesystem::path(path.toStdWString())));
    REQUIRE(controller->loadProject(QUrl::fromLocalFile(path)));
}

QObject* make(Shell& shell, const char* type) {
    QQmlComponent component(&shell.engine);
    component.loadFromModule("Wabcut", type);
    INFO(component.errorString().toStdString());
    REQUIRE(component.isReady());
    QObject* object = component.createWithInitialProperties({{QStringLiteral("parent"), QVariant::fromValue(shell.window->contentItem())}});
    REQUIRE(object != nullptr);
    object->setParent(shell.window.get());
    return object;
}

Ticks sec(double s) { return Ticks{static_cast<std::int64_t>(s * static_cast<double>(kTicksPerSecond))}; }

/// The fixture: a 10 s interview (A1 dialogue, A2 music) with captions in `language`.
ProjectState studio(const std::string& language, std::vector<CaptionCue> cues) {
    Timeline tl("V1 |A" + std::string(249, 'a') + "|\nA1 dialogue |A" + std::string(249, 'a') + "|\nA2 music |M" + std::string(249, 'm') + "|");
    tl.linkMatching();
    CaptionTrack track;
    track.name = language == "th" ? "Captions (ไทย)" : "English";
    track.language = language;
    track.cues = std::move(cues);
    auto made = wabcut::edit::setCaptionTrack(tl.state, tl.seq, track);
    REQUIRE(made);
    return made->state;
}

/// A provider that speaks real WAV files (for the voice sample, decoded by FFmpeg).
class WavSpeech final : public ai::ISpeechSynth {
public:
    WavSpeech() : m_spec(modelSpec("eleven_v3")) {}
    const ai::ModelSpec& spec() const override { return m_spec; }
    std::expected<ai::SpeechResult, ai::AiError> synthesize(const ai::SpeechRequest&, std::stop_token) override {
        ai::SpeechResult r;
        const std::uint32_t frames = 24000;
        std::string w = "RIFF";
        const auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) w.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); };
        const auto u16 = [&](std::uint16_t v) { w.push_back(static_cast<char>(v & 0xFF)); w.push_back(static_cast<char>(v >> 8)); };
        u32(36 + frames * 2);
        w += "WAVEfmt ";
        u32(16); u16(1); u16(1); u32(24000); u32(48000); u16(2); u16(16);
        w += "data";
        u32(frames * 2);
        for (std::uint32_t i = 0; i < frames; ++i) {
            u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(8000.0 * std::sin(i * 0.0576))));
        }
        r.audio.assign(w.begin(), w.end());
        r.mediaType = "audio/wav";
        return r;
    }

private:
    ai::ModelSpec m_spec;
};

} // namespace

TEST_CASE("Translate captions dialog: Thai and Japanese tracks staged with the report, added as one undo step; screenshot",
          "[ai][ui][global][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    INFO(join(shell.errors));
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* translate = shell.engine.singletonInstance<TranslateBridge*>("Wabcut", "TranslateCaptions");
    REQUIRE(controller != nullptr);
    REQUIRE(translate != nullptr);

    loadInto(controller,
             studio("en", {CaptionCue{sec(0.5), sec(2.0), "Welcome back to the market.", {}},
                           CaptionCue{sec(3.0), sec(1.2), "Today we try the famous [bleep] noodles.", {}},
                           CaptionCue{sec(5.0), sec(2.5), "Thanks for watching!", {}}}),
             QStringLiteral("translate-fixture.wabcut"));
    std::map<std::string, std::string> words{
        {"Welcome back to the market.", "ยินดีต้อนรับกลับสู่ตลาดอีกครั้งนะครับ"},
        {"Today we try the famous", "วันนี้เราจะลองชิมก๋วยเตี๋ยวชื่อดัง"},
        {"noodles.", "ร้านนี้กันครับ"},
        {"Thanks for watching!", "ขอบคุณที่รับชมครับ"}};
    auto local = [words](std::string_view, std::string_view target) -> std::expected<std::unique_ptr<ai::ITranslator>, ai::AiError> {
        if (target == "ja") {
            return std::make_unique<DictionaryTranslator>(std::map<std::string, std::string>{
                {"Welcome back to the market.", "市場へおかえりなさい。"}, {"Today we try the famous", "今日は有名な"}, {"noodles.", "麺を食べます。"},
                {"Thanks for watching!", "ご視聴ありがとう！"}});
        }
        return std::make_unique<DictionaryTranslator>(words);
    };
    translate->setServices(std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        std::make_shared<ai::translate::TranslateServices>(local, ai::translate::TranslateServices::ChatOpener{}),
        std::make_shared<command::CaptionServices>(qtLineBreaker())}));
    translate->setSynchronous(true);
    shell.resize(1600, 1100);

    QObject* dialog = make(shell, "TranslateDialog");
    REQUIRE(QMetaObject::invokeMethod(dialog, "open"));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    dialog->setProperty("selected", QStringList{QStringLiteral("th"), QStringLiteral("ja")});
    auto* provider = dialog->findChild<QObject*>("translateProvider");
    REQUIRE(provider != nullptr);
    provider->setProperty("currentIndex", 1); // this computer
    REQUIRE(QMetaObject::invokeMethod(dialog, "translate"));
    REQUIRE(translate->waitForIdle(20'000));
    INFO("Qt messages: " << join(takeMessages()));
    INFO(translate->message().toStdString());
    const QVariantList results = translate->report().value(QStringLiteral("languages")).toList();
    REQUIRE(results.size() == 2);
    CHECK(results[0].toMap().value(QStringLiteral("language")).toString() == QStringLiteral("th"));
    CHECK(results[0].toMap().value(QStringLiteral("provider")).toString() == QStringLiteral("local"));
    CHECK(translate->report().value(QStringLiteral("redacted")).toInt() == 1); // the cue's own [bleep], kept as it is
    CHECK(translate->staged());

    auto* list = dialog->findChild<QQuickItem*>("translateReport");
    REQUIRE(list != nullptr);
    REQUIRE(waitFor([&] { return list->property("count").toInt() == 2; }));
    spin(800);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/translate-captions.png")));

    CHECK(translate->apply().isEmpty());
    const Sequence* seq = controller->snapshot()->findSequence(controller->currentSequenceId());
    REQUIRE(seq != nullptr);
    REQUIRE(seq->captionTracks.size() == 3);
    const CaptionTrack& thai = seq->captionTracks[1];
    CHECK(thai.language == "th");
    CHECK(thai.name == "English (ไทย)");
    bool bleepKept = false;
    for (const CaptionCue& c : thai.cues) {
        bleepKept = bleepKept || c.text.find("[bleep]") != std::string::npos;
    }
    CHECK(bleepKept);
    REQUIRE(controller->session().undo());
    CHECK(controller->snapshot()->findSequence(controller->currentSequenceId())->captionTracks.size() == 1);
    delete dialog;
}

TEST_CASE("Dub dialog: the estimate, the timing-fit report, a ducked Dialogue Dub track marked AI; screenshots",
          "[ai][ui][global][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    INFO(join(shell.errors));
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* dub = shell.engine.singletonInstance<DubBridge*>("Wabcut", "Dub");
    REQUIRE(controller != nullptr);
    REQUIRE(dub != nullptr);

    loadInto(controller,
             studio("th", {CaptionCue{sec(0.5), sec(2.0), "ยินดีต้อนรับกลับสู่ตลาดอีกครั้งนะครับ", {}},
                           CaptionCue{sec(3.0), sec(1.2), "วันนี้เราจะลองชิมก๋วยเตี๋ยวชื่อดัง [bleep] ร้านนี้กันครับ", {}},
                           CaptionCue{sec(5.0), sec(2.5), "ขอบคุณที่รับชมครับ", {}}}),
             QStringLiteral("dub-fixture.wabcut"));
    auto audio = std::make_shared<FakeSpeechAudio>();
    auto opener = [](const ai::ModelSpec& model, std::optional<ai::MicroUsd>) -> std::expected<std::unique_ptr<ai::ISpeechSynth>, ai::AiError> {
        return std::make_unique<ScriptedSpeech>(model.id, 14.0);
    };
    const auto dir = std::filesystem::path(outputDir().toStdWString()) / "dub";
    dub->setServices(std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
        std::make_shared<ai::dub::DirectDubServices>(opener, audio, dir)}));
    dub->setSynchronous(true);
    dub->setOpenSamples(false);
    dub->setPreviewSpeech(std::make_shared<WavSpeech>());
    shell.resize(1600, 1100);

    QObject* dialog = make(shell, "DubDialog");
    const QVariantList models = dialog->property("models").toList();
    int eleven = -1;
    for (int i = 0; i < models.size(); ++i) {
        eleven = models[i].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("eleven_v3") ? i : eleven;
    }
    REQUIRE(eleven >= 0);
    dialog->setProperty("modelIndex", eleven);
    REQUIRE(QMetaObject::invokeMethod(dialog, "open"));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    REQUIRE(dub->waitForIdle(20'000));
    INFO(dub->message().toStdString());
    const QVariantMap plan = dub->plan();
    CHECK(plan.value(QStringLiteral("lines")).toInt() == 3);
    CHECK(plan.value(QStringLiteral("track")).toString() == QStringLiteral("Dub (ไทย)"));
    CHECK(plan.value(QStringLiteral("estimate")).toMap().value(QStringLiteral("usd")).toDouble() > 0.0);

    // "Hear it": one line through the voice, decoded and written for the player.
    CHECK(dub->previewVoice(QVariantMap{{QStringLiteral("model"), QStringLiteral("eleven_v3")}, {QStringLiteral("voice"), QStringLiteral("JBFqnCBsd6RMkjVDRZzb")}}).isEmpty());
    REQUIRE(dub->waitForIdle(20'000));
    CHECK_FALSE(dub->lastSample().isEmpty());

    QVariant options;
    REQUIRE(QMetaObject::invokeMethod(dialog, "options", Q_RETURN_ARG(QVariant, options), Q_ARG(QVariant, true)));
    CHECK(dub->run(options.toMap()).isEmpty());
    REQUIRE(dub->waitForIdle(20'000));
    INFO(dub->message().toStdString());
    const QVariantMap fit = dub->report().value(QStringLiteral("fit")).toMap();
    REQUIRE_FALSE(fit.isEmpty());
    CHECK(fit.value(QStringLiteral("stretched")).toInt() + fit.value(QStringLiteral("fitted")).toInt() +
              fit.value(QStringLiteral("shifted")).toInt() >= 1);
    CHECK(fit.value(QStringLiteral("maxSpeed")).toDouble() <= 1.15 + 1e-9);
    CHECK(dub->staged());
    auto* list = dialog->findChild<QQuickItem*>("dubReport");
    REQUIRE(list != nullptr);
    REQUIRE(waitFor([&] { return list->property("count").toInt() == 3; }));
    spin(800);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/dub-timing-report.png")));

    const std::size_t undoBefore = controller->session().undoCount();
    CHECK(dub->apply().isEmpty());
    CHECK(controller->session().undoCount() == undoBefore + 1);
    const Sequence* seq = controller->snapshot()->findSequence(controller->currentSequenceId());
    REQUIRE(seq != nullptr);
    REQUIRE(seq->audioTracks.size() == 3);
    const Track& track = *seq->audioTracks.back();
    CHECK(track.name == "Dub (ไทย)");
    CHECK(track.role == TrackRole::Dialogue);
    REQUIRE(track.clips.size() == 3);
    const MediaItem* media = controller->snapshot()->findMedia(track.clips[0].media);
    REQUIRE(media != nullptr);
    REQUIRE(media->generated);
    CHECK(media->generated->kind == "audio");
    CHECK(seq->audioTracks[0]->clips[0].properties()[ParamId::VolumeDb].curve() != nullptr); // the dialogue ducked
    delete dialog;
    spin(800);
    REQUIRE(shell.grab().save(outputDir() + QStringLiteral("/dub-track-badge.png")));
    REQUIRE(controller->session().undo());
    CHECK(controller->snapshot()->findSequence(controller->currentSequenceId())->audioTracks.size() == 2);
}
