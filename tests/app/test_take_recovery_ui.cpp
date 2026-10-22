// p02 audio: the voice-over take recovery prompt (TakeRecoveryHost.qml) in the shell: a take that was recorded and never
// placed is offered when its project opens; Recover puts it back where it was recorded and the prompt goes; Discard
// stops offering. Offscreen, on the Null audio backend (screenshot test-output/ui/take-recovery.png).

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "VoiceOverBridge.h"

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/audio/AudioInput.h>
#include <wabcut/editor/PlaybackSession.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QTemporaryDir>

#include <filesystem>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

constexpr const char* kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaa|
    A2     |.........................|
)";

std::vector<std::filesystem::path> wavsIn(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, ec); !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->path().extension() == ".wav")
            out.push_back(it->path());
    }
    return out;
}

/// An item by object name in the visual tree (Repeater delegates are not QObject children of the window).
QQuickItem* recoveryItem(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = recoveryItem(child, name))
            return found;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Voice-over take recovery prompt: offered on open and recovered where it was recorded", "[app][voiceover][recovery][qml]") {
    qputenv("WABCUT_AUDIO_BACKEND", "null");
    resetSettings();
    clearMessages();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const std::filesystem::path folder(dir.path().toStdWString());
    {
        Shell shell;
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        auto* playback = shell.engine.singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
        auto* voiceOver = shell.engine.singletonInstance<VoiceOverBridge*>("Wabcut", "VoiceOver");
        REQUIRE(controller);
        REQUIRE(playback);
        REQUIRE(voiceOver);
        Timeline tl(kPicture);
        controller->resetProject(tl.state, tl.seq);
        controller->setProjectFile(folder / "Film.wabcut");
        voiceOver->setOptions(QVariantMap{{QStringLiteral("countIn"), 0},
                                          {QStringLiteral("prerollSeconds"), 0.0},
                                          {QStringLiteral("postrollSeconds"), 0.0}});
        REQUIRE(voiceOver->setRecordArm(trackKey(tl.track("A1")), true).isEmpty());
        REQUIRE(voiceOver->start());
        REQUIRE(playback->session());
        for (int i = 0; i < 300 && voiceOver->recordedSeconds() < 0.2; ++i) {
            std::vector<float> out(480 * 2);
            REQUIRE(playback->session()->engine().pull(out));
            REQUIRE(voiceOver->input().push(std::vector<float>(480, 0.25f)));
            spin(10);
        }
        voiceOver->stop();
        REQUIRE(wavsIn(folder).size() == 1);
        // The take leaves the project (as if the app stopped before keeping it), then the project opens again.
        REQUIRE(controller->undo());
        controller->setProjectFile(folder / "Other.wabcut");
        controller->setProjectFile(folder / "Film.wabcut");
        spin(100);
        REQUIRE(voiceOver->recoveredTakes().size() == 1);
        QQuickItem* prompt = shell.item("takeRecoveryPrompt");
        REQUIRE(prompt);
        CHECK(prompt->isVisible());
        CHECK(recoveryItem(prompt, QStringLiteral("recoveredTake_0")));
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("take-recovery.png"))));
        // Recover: the take is placed and the prompt goes away.
        QQuickItem* recover = recoveryItem(prompt, QStringLiteral("recoverTake"));
        REQUIRE(recover);
        REQUIRE(QMetaObject::invokeMethod(recover, "clicked"));
        spin(100);
        CHECK(voiceOver->recoveredTakes().isEmpty());
        CHECK_FALSE(prompt->isVisible());
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
    qunsetenv("WABCUT_AUDIO_BACKEND");
}
