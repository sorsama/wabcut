// The editing assistant's UI (AI-5; owner: AI platform engineer): AgentController on the real Main.qml with a scripted
// model (no network, no keys). The palette's "Ask AI…" row opens the assistant panel, a run stages its edit without
// touching the project, the timeline shows the staged edit as a read-only preview, Accept applies it as one undo
// step. Screenshots of the panel with the preview and its verification report land in <build>/test-output/ui/.

#include "AgentTestUtil.h"
#include "ShellFixture.h"

#include "AgentController.h"
#include "AiController.h"
#include "CommandPalette.h"
#include "ProjectController.h"
#include "TimelineView.h"

#include <wabcut/model/Timecode.h>

#include <catch2/catch_test_macros.hpp>

#include <QStandardPaths>

#include <algorithm>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using wabcut::ai::Json;
using wabcut::ai::test::ScriptedModel;
using wabcut::ai::test::SharedModel;

namespace {

const QColor kViolet(0x9B, 0x6C, 0xFF); // design language v3 violet, the default accent

void isolate() {
    qputenv("WABCUT_AI_KEYSTORE", "memory");
    QStandardPaths::setTestModeEnabled(true);
    resetSettings();
    clearMessages();
}

/// The first V1 clip of the shown sequence that is at least 3 s long.
const Clip* longClip(const Sequence& seq) {
    for (const TrackPtr& t : seq.videoTracks) {
        for (const Clip& c : t->clips) {
            if (c.duration() >= Ticks::fromSeconds(3)) {
                return &c;
            }
        }
    }
    return nullptr;
}

struct Scene {
    Scene() {
        isolate();
        REQUIRE(shell.window != nullptr);
        INFO(join(shell.errors));
        shell.resize(kShotWidth, kShotHeight);
        agent = shell.engine.singletonInstance<AgentController*>("Wabcut", "AgentController");
        project = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(agent != nullptr);
        REQUIRE(project != nullptr);
        agent->setModelFactory([model = model] { return std::make_unique<SharedModel>(model); });
        agent->setServices(nullptr); // no transcripts in the demo project
        ai::agent::AgentOptions options;
        options.transcriptDir = std::filesystem::temp_directory_path() / "wabcut-agent-ui-test";
        agent->setAgentOptions(options);
        view = qobject_cast<TimelineView*>(shell.object("timelineView"));
        REQUIRE(view != nullptr);
    }

    const Sequence& sequence() const { return *project->session().snapshot()->findSequence(project->currentSequenceId()); }
    std::string tc(Ticks t) const { return ticksToTimecode(t, sequence().frameRate, sequence().dropFrame); }

    /// Opens the panel the way a user does: palette query, then its "Ask AI…" row.
    void askThroughPalette(const QString& query) {
        auto* palette = shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
        REQUIRE(palette != nullptr);
        QObject* overlay = shell.object("commandPalette");
        REQUIRE(overlay != nullptr);
        QMetaObject::invokeMethod(overlay, "openPalette");
        palette->setQuery(query);
        REQUIRE(palette->activate(QStringLiteral("ai:ask")).isEmpty());
        REQUIRE(waitFor([&] { return agent->panelOpen(); }));
        QObject* panel = shell.object("assistantPanel");
        REQUIRE(panel != nullptr);
        REQUIRE(waitFor([&] { return panel->property("opened").toBool(); }));
        CHECK(agent->prompt() == query);
        CHECK_FALSE(overlay->property("opened").toBool()); // the palette handed over and closed
    }

    void screenshot(const char* file) {
        REQUIRE(shell.settle(true, kViolet));
        spin(400); // the panel's step list and the overlay paint
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        CHECK(image.size() == QSize(kShotWidth, kShotHeight));
        const QString path = outputDir() + QLatin1Char('/') + QLatin1String(file);
        REQUIRE(image.save(path));
        UNSCOPED_INFO("wrote " << path.toStdString());
    }

    Shell shell;
    std::shared_ptr<ScriptedModel> model = std::make_shared<ScriptedModel>();
    AgentController* agent = nullptr;
    ProjectController* project = nullptr;
    TimelineView* view = nullptr;
};

} // namespace

TEST_CASE("Assistant: the data-flow badge and the consent prompt come from the AI platform", "[ai][ui][agent]") {
    isolate();
    AiController ai;
    AgentController agent;
    agent.setAiController(&ai);
    CHECK(agent.dataFlow().value(QStringLiteral("consent")) == QStringLiteral("local")); // local-first default
    CHECK(agent.dataFlow().value(QStringLiteral("local")).toBool());

    ai.saveKey(QStringLiteral("anthropic"), QStringLiteral("sk-ant-ui-test-key-0123456789abcd"), true);
    ai.setChatModel(QStringLiteral("claude-sonnet-5-5"));
    const QVariantMap flow = agent.dataFlow();
    CHECK(flow.value(QStringLiteral("provider")) == QStringLiteral("Anthropic"));
    CHECK(flow.value(QStringLiteral("destination")) == QStringLiteral("api.anthropic.com"));
    CHECK(flow.value(QStringLiteral("consent")) == QStringLiteral("needed"));
    CHECK_FALSE(flow.value(QStringLiteral("local")).toBool());
    CHECK(flow.value(QStringLiteral("sends")).toString().contains(QStringLiteral("Never media files")));

    // Without consent nothing is sent: the request waits for the answer, and a "no" sends nothing.
    ProjectController project;
    agent.setProjectController(&project);
    agent.ask(QStringLiteral("Remove silences"));
    CHECK(agent.state() == QStringLiteral("consent"));
    CHECK(agent.consentPrompt().value(QStringLiteral("providerName")) == QStringLiteral("Anthropic"));
    agent.answerConsent(false, false);
    CHECK(agent.state() == QStringLiteral("idle"));
    CHECK(agent.message().contains(QStringLiteral("Nothing was sent")));
    CHECK(ai.platform().log().entries().empty());

    // Quick actions: "Cut to the beat" (C.5 beat detection) is listed only with a music track to cut to.
    QStringList ids;
    for (const QVariant& a : agent.quickActions()) {
        const QVariantMap m = a.toMap();
        ids << m.value(QStringLiteral("id")).toString();
        if (ids.last() == QStringLiteral("beatCut")) {
            CHECK_FALSE(m.value(QStringLiteral("track")).toString().isEmpty());
        }
    }
    ids.removeAll(QStringLiteral("beatCut"));
    CHECK(ids == QStringList{QStringLiteral("removeSilences"), QStringLiteral("removeFillers"), QStringLiteral("highlight"),
                          QStringLiteral("topicMarkers"), QStringLiteral("captions")});
}

TEST_CASE("Assistant: Ask AI from the palette stages an edit, previews it on the timeline and applies it on Accept",
          "[ai][ui][agent][screenshot]") {
    Scene s;
    const Clip* clip = longClip(s.sequence());
    REQUIRE(clip != nullptr);
    const Rate rate = s.sequence().frameRate;
    const Ticks cutFrom = snapToFrame(clip->start() + Ticks::fromSeconds(1), rate); // whole frames: exact timecodes
    const Ticks cutTo = snapToFrame(clip->start() + Ticks::fromSeconds(2), rate);
    const QString prompt = QStringLiteral("Tighten the opening: cut %1 to %2 and mark the intro")
                               .arg(QString::fromStdString(s.tc(cutFrom)), QString::fromStdString(s.tc(cutTo)));
    s.model->tool("sequence_summary", Json{{"to", s.tc(clip->end())}})
        .tool("timeline_removeSilences", Json{{"ranges", Json::array({Json{{"start", s.tc(cutFrom)}, {"end", s.tc(cutTo)}}})}})
        .tool("marker_add", Json{{"time", s.tc(clip->start())}, {"name", "Intro"}})
        .text("I cut one second from the opening shot and added an Intro marker at its start.");

    s.askThroughPalette(prompt);
    // The demo's music bed is on A3: "Cut to the beat of track A3" is offered.
    bool beatCut = false;
    for (const QVariant& a : s.agent->quickActions()) {
        const QVariantMap m = a.toMap();
        beatCut = beatCut || (m.value(QStringLiteral("id")) == QStringLiteral("beatCut") && m.value(QStringLiteral("track")) == QStringLiteral("A3"));
    }
    CHECK(beatCut);
    const ProjectSnapshot before = s.project->session().snapshot();
    const std::uint64_t revision = s.project->session().revision();
    const std::size_t undo = s.project->session().undoCount();
    s.agent->ask(prompt);
    REQUIRE(s.agent->waitForIdle(15'000));
    INFO(s.agent->message().toStdString() << " / " << s.agent->statusText().toStdString());
    REQUIRE(s.agent->state() == QStringLiteral("result"));
    CHECK(s.agent->status() == QStringLiteral("Staged"));
    CHECK(s.agent->checksPassed());
    CHECK(s.agent->canAccept());
    CHECK(s.agent->statusText().startsWith(QStringLiteral("All ")));
    REQUIRE(s.agent->steps().size() == 4);
    CHECK(s.agent->steps()[1].toMap().value(QStringLiteral("title")).toString().startsWith(QStringLiteral("Remove 1 silent range")));
    CHECK(s.agent->changeSummary().contains(QStringLiteral("marker")));

    // Staged only: the project is exactly as it was; the timeline shows the edit as a preview.
    CHECK(s.project->session().snapshot() == before);
    CHECK(s.project->session().revision() == revision);
    const TimelineView::ExternalPreview* preview = s.view->externalPreview();
    REQUIRE(preview != nullptr);
    REQUIRE_FALSE(preview->removed.empty());
    CHECK(std::ranges::any_of(preview->removed, [&](const auto& r) { return r.start == cutFrom && r.end == cutTo; }));
    CHECK(preview->markers == std::vector<Ticks>{clip->start()});

    s.screenshot("assistant-panel-staged.png");

    // Accept: one undo step, the preview goes away.
    s.agent->accept();
    CHECK(s.agent->state() == QStringLiteral("applied"));
    CHECK(s.agent->message() == QStringLiteral("Applied as one undo step."));
    CHECK(s.project->session().undoCount() == undo + 1);
    CHECK(s.view->externalPreview() == nullptr);
    CHECK(s.sequence().markers.size() == before->findSequence(s.project->currentSequenceId())->markers.size() + 1);
    s.project->session().undo();
    CHECK(*s.project->session().snapshot() == *before);
}

TEST_CASE("Assistant: a failed check is reported, not hidden (screenshot)", "[ai][ui][agent][screenshot]") {
    Scene s;
    const Clip* clip = longClip(s.sequence());
    REQUIRE(clip != nullptr);
    const Ticks length = s.sequence().contentEnd();
    REQUIRE(length > Ticks::fromSeconds(8));
    // "a 3-second cut": the model removes one second and claims success twice.
    s.model->tool("timeline_removeSilences",
                  Json{{"ranges", Json::array({Json{{"start", s.tc(clip->start() + Ticks::fromSeconds(1))},
                                                    {"end", s.tc(clip->start() + Ticks::fromSeconds(2))}}})}})
        .text("Done: the cut is 3 seconds long.")
        .text("The cut is fine as it is.");
    const QString prompt = QStringLiteral("Make a 3-second cut of this sequence");
    s.askThroughPalette(prompt);
    s.agent->ask(prompt);
    REQUIRE(s.agent->waitForIdle(15'000));
    REQUIRE(s.agent->state() == QStringLiteral("result"));
    CHECK(s.agent->status() == QStringLiteral("VerificationFailed"));
    CHECK_FALSE(s.agent->checksPassed());
    CHECK(s.agent->statusText().contains(QStringLiteral("checks failed")));
    bool durationFailed = false;
    for (const QVariant& c : s.agent->checks()) {
        const QVariantMap m = c.toMap();
        durationFailed = durationFailed || (m.value(QStringLiteral("id")) == QStringLiteral("duration") && !m.value(QStringLiteral("passed")).toBool());
    }
    CHECK(durationFailed);
    CHECK(s.view->externalPreview() != nullptr);

    s.screenshot("assistant-panel-failed-check.png");

    // Reject drops the staged edit and its preview; the project never changed.
    s.agent->reject();
    CHECK(s.agent->state() == QStringLiteral("idle"));
    CHECK(s.view->externalPreview() == nullptr);
}
