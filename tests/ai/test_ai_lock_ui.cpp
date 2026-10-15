// AI-6 (docs/internal/reviews-codex-wave2.md P1-4): the project no-cloud lock in the app. It lives in the project
// (ProjectState::aiNoCloud): Preferences > AI writes it into the open project, saving and reopening keeps it, recovery
// copies keep it, another project has its own, and recorded consent never overrides it, for chat, the editing
// assistant and cloud transcription alike. Real Main.qml singletons; keys in memory; nothing is sent.

#include "ShellFixture.h"

#include "AgentController.h"
#include "AiController.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "TranscriptBridge.h"

#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <QStandardPaths>
#include <QUrl>

#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

void isolate() {
    qputenv("WABCUT_AI_KEYSTORE", "memory");
    QStandardPaths::setTestModeEnabled(true);
    resetSettings();
    clearMessages();
}

ai::ChatRequest hello() {
    ai::ChatRequest r;
    r.messages.push_back(ai::ChatMessage::user("hello"));
    r.maxOutputTokens = 16;
    return r;
}

} // namespace

TEST_CASE("AI no-cloud lock: saved with the project, reopened, recovered, per project; consent never overrides it",
          "[ai][ui][security]") {
    isolate();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
    auto* project = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* agent = shell.engine.singletonInstance<AgentController*>("Wabcut", "AgentController");
    REQUIRE(ai);
    REQUIRE(project);
    REQUIRE(file);
    REQUIRE(agent);

    // A cloud provider with a key and recorded consent: normally a call would go out.
    ai->saveKey(QStringLiteral("anthropic"), QStringLiteral("sk-ant-lock-test-0123456789abcd"), /*skipCheck=*/true);
    ai->saveKey(QStringLiteral("deepgram"), QStringLiteral("dg-lock-test-0123456789abcdef"), true);
    ai->grantConsent(QStringLiteral("anthropic"), QStringLiteral("chat"));
    ai->grantConsent(QStringLiteral("deepgram"), QStringLiteral("transcribe"));
    ai->setChatModel(QStringLiteral("claude-sonnet-5-5"));
    const ai::ProviderSpec& anthropic = *ai::ModelRegistry::builtin().provider("anthropic");

    file->newProject();
    CHECK_FALSE(ai->projectLock());
    CHECK(ai->platform().consent().authorize(anthropic, anthropic.baseUrl, ai::Capability::Chat));

    // Lock it from Preferences > AI: the setting goes into the project (one undo step).
    const std::size_t undo = project->session().undoCount();
    ai->setProjectLock(true);
    CHECK(project->session().snapshot()->aiNoCloud);
    CHECK(project->session().undoCount() == undo + 1);
    CHECK(ai->projectLock());

    // Consent and allow-once do not override it: chat, the agent and cloud transcription are all refused before
    // anything is sent.
    auto chat = ai->platform().chat("claude-sonnet-5-5", ai::CallOptions{true});
    REQUIRE(chat);
    const auto refused = (*chat)->chat(hello(), {}, {});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ai::AiErrorCode::Offline);
    CHECK(refused.error().message == "this project is locked to no-cloud AI");

    const auto audio = std::filesystem::temp_directory_path() / "wabcut-lock-test.flac";
    std::ofstream(audio, std::ios::binary) << "fLaC";
    auto stt = ai->platform().transcriber("nova-3", ai::CallOptions{true});
    REQUIRE(stt);
    ai::TranscribeRequest transcribe;
    transcribe.audioFile = audio;
    transcribe.audioMilliseconds = 1000;
    const auto noUpload = (*stt)->transcribe(transcribe, {}, {});
    REQUIRE_FALSE(noUpload);
    CHECK(noUpload.error().code == ai::AiErrorCode::Offline);
    std::filesystem::remove(audio);

    agent->ask(QStringLiteral("Remove silences longer than 1 s"));
    REQUIRE(agent->waitForIdle(10'000));
    INFO("agent state " << agent->state().toStdString() << ", message \"" << agent->message().toStdString() << "\", status \""
                        << agent->statusText().toStdString() << "\"");
    CHECK(agent->message().contains(QStringLiteral("locked to no-cloud AI")));
    CHECK_FALSE(agent->canAccept());

    // Save, start another project (unlocked: the lock is per project), reopen the saved one (locked again).
    const auto dir = std::filesystem::temp_directory_path() / ("wabcut-lock-" + std::to_string(QCoreApplication::applicationPid()));
    std::filesystem::create_directories(dir);
    const auto path = dir / "nda.wabcut";
    REQUIRE(file->saveAs(QUrl::fromLocalFile(QString::fromStdWString(path.wstring()))));
    file->newProject();
    CHECK_FALSE(project->session().snapshot()->aiNoCloud);
    CHECK_FALSE(ai->projectLock());
    CHECK(ai->platform().consent().authorize(anthropic, anthropic.baseUrl, ai::Capability::Chat));
    REQUIRE(file->open(QUrl::fromLocalFile(QString::fromStdWString(path.wstring()))));
    CHECK(project->session().snapshot()->aiNoCloud);
    CHECK(ai->projectLock());
    CHECK_FALSE(ai->platform().consent().authorize(anthropic, anthropic.baseUrl, ai::Capability::Chat));

    // The saved file and its recovery copy both carry the lock.
    const auto saved = io::loadProject(path);
    REQUIRE(saved);
    CHECK(saved->aiNoCloud);
    REQUIRE(io::writeAutosave(*project->session().snapshot(), path));
    const auto recovery = io::loadProject(io::autosavePath(path));
    REQUIRE(recovery);
    CHECK(recovery->aiNoCloud);
    file->newProject();
    CHECK_FALSE(ai->projectLock());
    REQUIRE(file->recover(QUrl::fromLocalFile(QString::fromStdWString(io::autosavePath(path).wstring()))));
    CHECK(ai->projectLock()); // recovering the copy brings its lock back

    // Unlocking is a project edit too, and undo restores the lock.
    ai->setProjectLock(false);
    CHECK_FALSE(ai->projectLock());
    project->session().undo();
    CHECK(ai->projectLock());

    file->newProject();
    std::filesystem::remove_all(dir);
}

TEST_CASE("Cloud transcription uploads nothing without consent, whichever path starts it", "[ai][ui][security]") {
    isolate();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
    auto* transcript = shell.engine.singletonInstance<TranscriptBridge*>("Wabcut", "Transcript");
    REQUIRE(ai);
    REQUIRE(transcript);
    ai->saveKey(QStringLiteral("deepgram"), QStringLiteral("dg-consent-test-0123456789abcdef"), /*skipCheck=*/true);
    transcript->setProvider(QStringLiteral("cloud:nova-3"));
    // Past the consent gate a scripted transcriber stands in for the provider: nothing may reach the network.
    int uploads = 0;
    transcript->setTranscriber(QStringLiteral("cloud:nova-3"), QStringLiteral("Deepgram (scripted)"),
                               [&uploads](const std::string&, int, const std::function<void(float)>&, std::stop_token)
                                   -> std::expected<ai::Transcript, Error> {
                                   ++uploads;
                                   return std::unexpected(Error(ErrorCode::Unknown, "scripted"));
                               });

    // Called directly (not from the panel's consent flow): refused before any audio is prepared or sent. Before the
    // fix every cloud transcription went out with "Allow once" set.
    const QString refused = transcript->transcribe(QStringLiteral("sequence"));
    CHECK(refused.contains(QStringLiteral("needs your consent")));
    CHECK(ai->platform().log().entries().empty());
    CHECK(uploads == 0);

    // The consent dialog's answer, or a recorded consent, lets it through the consent gate.
    CHECK_FALSE(transcript->transcribe(QStringLiteral("sequence"), /*allowOnce=*/true).contains(QStringLiteral("consent")));
    transcript->cancelTranscription();
    ai->grantConsent(QStringLiteral("deepgram"), QStringLiteral("transcribe"));
    CHECK_FALSE(transcript->transcribe(QStringLiteral("sequence")).contains(QStringLiteral("consent")));
    transcript->cancelTranscription();
}

TEST_CASE("The consent ledger knows which project is active: another project changes its identity (wave 3, P1-5)",
          "[ai][ui][security]") {
    isolate();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    auto* ai = shell.engine.singletonInstance<AiController*>("Wabcut", "AiController");
    auto* project = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(ai);
    REQUIRE(project);
    REQUIRE(file);
    const ai::ConsentLedger& consent = ai->platform().consent();

    file->newProject();
    const std::uint64_t first = consent.projectIdentity();
    CHECK(first != 0);
    // Edits and undo stay in the same project: an operation running across them keeps going.
    ai->setProjectLock(true);
    project->session().undo();
    CHECK(consent.projectIdentity() == first);
    // Another project (new, opened, recovered): operations of the previous one stop at their next request.
    file->newProject();
    CHECK(consent.projectIdentity() != first);
}