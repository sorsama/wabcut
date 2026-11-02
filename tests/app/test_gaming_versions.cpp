// 0.6 (P1 round, stream 3A): Make Shorts / square version as a job (#55) and the facecam choices it reuses (#51).
//   * MomentsBridge.makeVersion runs off the GUI thread (synchronous here): the subjects of each video media first
//     (analysis.subjects, when the version will be Auto Reframed), with "Finding the subject 1 of 2 · 40 %" progress and
//     Cancel; then sequence.makeVersion on a copy, committed as ONE undo step ("Make Shorts Version") that opens the new
//     sequence, unless the project changed meanwhile;
//   * no subject analysis when a facecam track or a stored facecam region is there; the region drawn in the layout
//     picker for the gameplay's file and the picker's last split and facecam side go to sequence.makeVersion;
//   * in the window: makeVersion returns at once, the Moments popover shows the step, the bar and Cancel (screenshots,
//     dark and light), the version buttons wait, a toast follows a version started from the menus.
// Stand-in commands (the catalog's analysis.subjects decodes media; the real makeVersion is covered by its own suite).

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "LayoutBridge.h"
#include "MomentsBridge.h"
#include "ProjectController.h"

#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QSettings>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using command::Json;

namespace {

/// 250 frames of A on V1 and A1 (linked), then 10 frames of B: two video media.
std::string picture() {
    const std::string body = "A" + std::string(249, 'a') + "Bbbbbbbbbb";
    return "V1 tgt |" + body + "|\nA1 tgt |" + body + "|";
}
/// The same with a webcam clip C on V2.
std::string pictureWithCam() {
    const std::string body = "A" + std::string(249, 'a') + "Bbbbbbbbbb";
    return "V2     |C" + std::string(259, 'c') + "|\nV1 tgt |" + body + "|\nA1 tgt |" + body + "|";
}

std::mutex g_mutex;
std::vector<std::uint64_t> g_subjects; ///< the media analysis.subjects ran on, in order
Json g_versionParams;                  ///< what sequence.makeVersion last got
std::function<void()> g_onSubjects;    ///< runs inside analysis.subjects, after its 40 % checkpoint
std::function<void()> g_onVersion;     ///< runs inside sequence.makeVersion
std::atomic<bool> g_hold{false};       ///< analysis.subjects waits (checkpointing) while set

void resetStubs() {
    std::lock_guard lock(g_mutex);
    g_subjects.clear();
    g_versionParams = Json();
    g_onSubjects = nullptr;
    g_onVersion = nullptr;
    g_hold = false;
}

std::vector<std::uint64_t> subjectsRun() {
    std::lock_guard lock(g_mutex);
    return g_subjects;
}

Json versionParams() {
    std::lock_guard lock(g_mutex);
    return g_versionParams;
}

const command::CommandRegistry& versionCatalog() {
    static const command::CommandRegistry registry = [] {
        command::CommandRegistry r;
        command::CommandSpec subjects;
        subjects.name = "analysis.subjects";
        subjects.title = "Find Subjects";
        subjects.description = "Stand-in";
        subjects.capability = command::Capability::MediaAnalysis;
        subjects.params = Json{{"type", "object"}};
        subjects.execute = [](command::ExecutionContext& c) -> command::CommandOutput {
            std::function<void()> hook;
            {
                std::lock_guard lock(g_mutex);
                g_subjects.push_back(c.params().value("media", std::uint64_t{0}));
                hook = g_onSubjects;
            }
            c.checkpoint("finding subjects (40%)");
            if (hook)
                hook();
            while (g_hold.load()) {
                c.checkpoint("finding subjects (50%)"); // stops here once cancelled
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            c.checkpoint("finding subjects (100%)");
            command::CommandOutput out;
            out.data = Json{{"cached", false}};
            return out;
        };
        (void)r.add(std::move(subjects));
        command::CommandSpec version;
        version.name = "sequence.makeVersion";
        version.title = "Make Vertical or Square Version";
        version.description = "Stand-in";
        version.capability = command::Capability::EditProject;
        version.params = Json{{"type", "object"}};
        version.execute = [](command::ExecutionContext& c) -> command::CommandOutput {
            std::function<void()> hook;
            {
                std::lock_guard lock(g_mutex);
                g_versionParams = c.params();
                hook = g_onVersion;
            }
            if (hook)
                hook();
            const bool square = c.string("format") == "square";
            edit::SequenceSettings settings;
            settings.name = square ? "Edit (Square)" : "Edit (Shorts)";
            settings.width = 1080;
            settings.height = square ? 1080 : 1920;
            auto created = edit::addSequence(c.state(), settings);
            if (!created)
                command::fail(command::ErrorCode::EditRejected, created.error().message);
            command::CommandOutput out;
            out.data = Json{{"sequence", created->id.value}, {"name", settings.name}};
            out.createdIds = {created->id.value};
            out.state = std::move(created->state);
            return out;
        };
        (void)r.add(std::move(version));
        return r;
    }();
    return registry;
}

struct VersionRig {
    Timeline tl;
    ProjectController controller;
    MomentsBridge moments;

    explicit VersionRig(const std::string& text = picture()) : tl(text) {
        resetStubs();
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        moments.setController(&controller);
        moments.setRegistry(&versionCatalog());
        moments.setSynchronous(true);
    }
    ~VersionRig() {
        resetStubs();
        moments.setRegistry(nullptr);
    }
    std::size_t undo() { return controller.session().undoCount(); }
    QVariantMap version() const { return moments.version(); }
};

/// The project with V2 named like a webcam track (sequence.makeVersion's facecam detection).
ProjectState withCamTrack(const Timeline& tl) {
    const Sequence& seq = *tl.state.findSequence(tl.seq);
    Sequence renamed = seq;
    Track cam = *renamed.videoTracks[1];
    cam.name = "Facecam";
    renamed.videoTracks[1] = std::make_shared<const Track>(std::move(cam));
    return tl.state.withSequence(std::make_shared<const Sequence>(std::move(renamed)));
}

QQuickItem* findNamed(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findNamed(child, name))
            return found;
    }
    return nullptr;
}

QObject* findObject(QObject* root, const QString& name) {
    for (QObject* o : root->findChildren<QObject*>()) {
        if (o->objectName() == name)
            return o;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Gaming versions: Make Shorts version finds the subjects first then commits one undo step and opens it",
          "[app][moments][versions]") {
    QSettings().remove(QStringLiteral("layout"));
    VersionRig r;
    const SequenceId original = r.controller.currentSequenceId();
    // What the step said while the first subject analysis ran.
    QString during;
    double progressDuring = -1;
    bool busyDuring = false;
    g_onSubjects = [&] {
        if (!during.isEmpty())
            return;
        during = r.version().value(QStringLiteral("text")).toString();
        progressDuring = r.version().value(QStringLiteral("progress")).toDouble();
        busyDuring = r.version().value(QStringLiteral("busy")).toBool();
    };
    const std::size_t before = r.undo();
    CHECK(r.moments.makeVersion(QStringLiteral("shorts")).isEmpty());
    // The subjects of both video media, in the sequence's order, before the version.
    CHECK(subjectsRun() == std::vector<std::uint64_t>{r.tl.media('A').value, r.tl.media('B').value});
    CHECK(busyDuring);
    CHECK(during == QStringLiteral("Finding the subject 1 of 2 · 40 %"));
    CHECK(progressDuring == Catch::Approx(0.9 * 0.4 / 2));
    // ONE undo step with the version's own name, the version shown.
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Make Shorts Version"));
    CHECK(versionParams().value("format", std::string()) == "shorts");
    CHECK(versionParams().value("sequence", std::uint64_t{0}) == original.value);
    CHECK_FALSE(versionParams().contains("facecamRegion"));
    CHECK(r.controller.currentSequenceId() != original);
    CHECK(r.controller.currentSequence()->height == 1920);
    const QVariantMap done = r.version();
    CHECK_FALSE(done.value(QStringLiteral("busy")).toBool());
    CHECK(done.value(QStringLiteral("done")).toString() == QStringLiteral("Edit (Shorts)"));
    CHECK(done.value(QStringLiteral("error")).toString().isEmpty());
    CHECK(r.moments.message().isEmpty());
    // Undo takes the whole version back.
    r.controller.session().undo();
    CHECK(r.controller.snapshot()->sequences.size() == 1);

    // The square version through the preset name: its own undo name, and no split options (they are the vertical
    // split's), whatever the layout picker last used.
    QSettings().setValue(QStringLiteral("layout/lastOptions"), QStringLiteral(R"({"split":0.7})"));
    CHECK(r.moments.apply(QStringLiteral("squareVersion")).isEmpty());
    CHECK(r.controller.undoText() == QStringLiteral("Undo Make Square Version"));
    CHECK(r.controller.currentSequence()->height == 1080);
    CHECK(r.version().value(QStringLiteral("format")).toString() == QStringLiteral("square"));
    CHECK_FALSE(versionParams().contains("split"));
    QSettings().remove(QStringLiteral("layout"));
}

TEST_CASE("Gaming versions: Cancel stops the version and nothing changes", "[app][moments][versions]") {
    QSettings().remove(QStringLiteral("layout"));
    VersionRig r;
    const SequenceId original = r.controller.currentSequenceId();
    g_onSubjects = [&] { r.moments.cancelVersion(); };
    QString cancelling;
    QObject::connect(&r.moments, &MomentsBridge::versionChanged, &r.moments, [&] {
        if (r.version().value(QStringLiteral("busy")).toBool() && cancelling.isEmpty() &&
            r.version().value(QStringLiteral("text")).toString().startsWith(QStringLiteral("Cancel")))
            cancelling = r.version().value(QStringLiteral("text")).toString();
    });
    const std::size_t before = r.undo();
    CHECK(r.moments.makeVersion(QStringLiteral("shorts")).isEmpty());
    CHECK(cancelling == QStringLiteral("Cancelling…"));
    CHECK(subjectsRun().size() == 1); // stopped in the first analysis
    CHECK(versionParams().is_null()); // the version never ran
    CHECK(r.undo() == before);
    CHECK(r.controller.currentSequenceId() == original);
    CHECK_FALSE(r.version().value(QStringLiteral("busy")).toBool());
    CHECK(r.version().value(QStringLiteral("error")).toString().isEmpty());
    CHECK(r.version().value(QStringLiteral("done")).toString().isEmpty());
    CHECK(r.moments.message().isEmpty());
    // Cancel with nothing running does nothing.
    r.moments.cancelVersion();
    CHECK_FALSE(r.version().value(QStringLiteral("busy")).toBool());
}

TEST_CASE("Gaming versions: a project edited while the version is made is left alone", "[app][moments][versions]") {
    QSettings().remove(QStringLiteral("layout"));
    VersionRig r;
    const SequenceId original = r.controller.currentSequenceId();
    // The user adds a marker while the worker makes the version.
    g_onVersion = [&] {
        Marker marker;
        marker.name = "Meanwhile";
        const ProjectSnapshot now = r.controller.snapshot();
        auto edited = edit::addMarker(*now, original, marker);
        REQUIRE(edited);
        REQUIRE(r.controller.session().commit("Add Marker", edited->state));
    };
    const std::size_t before = r.undo();
    const QString error = r.moments.makeVersion(QStringLiteral("shorts"));
    CHECK(error == QStringLiteral("The project changed while this ran. Run it again."));
    CHECK(r.moments.message() == error);
    CHECK(r.version().value(QStringLiteral("error")).toString() == error);
    CHECK_FALSE(r.version().value(QStringLiteral("busy")).toBool());
    CHECK(r.undo() == before + 1); // only the user's marker
    CHECK(r.controller.snapshot()->sequences.size() == 1);
    CHECK(r.controller.currentSequenceId() == original);
}

TEST_CASE("Gaming versions: a facecam track or the stored facecam region skips the subjects; the picker's split goes along",
          "[app][moments][versions][layout]") {
    QSettings().remove(QStringLiteral("layout"));
    // The layout picker's last applied options (saved by its QML).
    QSettings().setValue(QStringLiteral("layout/lastOptions"),
                         QStringLiteral(R"({"preset":"verticalSplit","split":0.7,"facecamOnTop":true,"facecamSource":"region"})"));
    const QVariantMap region{{QStringLiteral("x"), 0.7}, {QStringLiteral("y"), 0.05},
                             {QStringLiteral("width"), 0.25}, {QStringLiteral("height"), 0.3}};

    SECTION("a facecam track: no subjects, no region (the track wins)") {
        VersionRig r(pictureWithCam());
        r.controller.resetProject(withCamTrack(r.tl), r.tl.seq);
        LayoutBridge::storeRegionFor(*r.controller.snapshot()->findMedia(r.tl.media('A')), region);
        CHECK(r.moments.makeVersion(QStringLiteral("shorts")).isEmpty());
        CHECK(subjectsRun().empty());
        const Json p = versionParams();
        INFO(p.dump());
        CHECK_FALSE(p.contains("facecamRegion"));
        CHECK(p.value("split", 0.0) == Catch::Approx(0.7));
        CHECK(p.value("facecamOnTop", false));
    }
    SECTION("the region drawn for the gameplay's file: no subjects, the region and the split") {
        VersionRig r;
        const MediaItem& gameplay = *r.controller.snapshot()->findMedia(r.tl.media('A'));
        LayoutBridge::storeRegionFor(gameplay, region);
        CHECK(LayoutBridge::storedRegionFor(gameplay).value(QStringLiteral("x")).toDouble() == Catch::Approx(0.7));
        CHECK(r.moments.makeVersion(QStringLiteral("shorts")).isEmpty());
        CHECK(subjectsRun().empty());
        const Json p = versionParams();
        INFO(p.dump());
        REQUIRE(p.contains("facecamRegion"));
        CHECK(p["facecamRegion"].value("x", 0.0) == Catch::Approx(0.7));
        CHECK(p["facecamRegion"].value("y", 0.0) == Catch::Approx(0.05));
        CHECK(p["facecamRegion"].value("width", 0.0) == Catch::Approx(0.25));
        CHECK(p["facecamRegion"].value("height", 0.0) == Catch::Approx(0.3));
        CHECK(p.value("split", 0.0) == Catch::Approx(0.7));
        CHECK(p.value("facecamOnTop", false));
    }
    SECTION("a region stored for another file is not used") {
        VersionRig r;
        LayoutBridge::storeRegionFor(*r.controller.snapshot()->findMedia(r.tl.media('B')), region);
        CHECK(r.moments.makeVersion(QStringLiteral("shorts")).isEmpty());
        CHECK_FALSE(versionParams().contains("facecamRegion"));
        CHECK(subjectsRun().size() == 2);
    }
    QSettings().remove(QStringLiteral("layout"));
}

TEST_CASE("Gaming versions in the window: the popover shows the step and Cancel while the job runs; screenshots",
          "[app][moments][versions][qml][screenshot]") {
    resetSettings();
    clearMessages();
    resetStubs();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* moments = shell.engine.singletonInstance<MomentsBridge*>("Wabcut", "Moments");
    REQUIRE(controller);
    REQUIRE(moments);
    moments->setRegistry(&versionCatalog());
    moments->setSynchronous(false);
    Timeline tl(picture());
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    controller->setPlayhead(f(100));
    spin(200);
    const SequenceId original = controller->currentSequenceId();
    const auto undo = [&] { return controller->session().undoCount(); };

    // The Tracks header's zap button opens the Moments popover.
    QQuickItem* panel = findNamed(shell.window->contentItem(), QStringLiteral("timelinePanel"));
    REQUIRE(panel);
    panel->setProperty("showSpeed", true);
    QQuickItem* button = findNamed(shell.window->contentItem(), QStringLiteral("momentsButton"));
    REQUIRE(button);
    REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
    QObject* popover = nullptr;
    REQUIRE(waitFor([&] {
        popover = findObject(button, QStringLiteral("momentsPopover"));
        return popover && popover->property("opened").toBool();
    }, 5000));
    auto* content = popover->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    QQuickItem* shortsButton = findNamed(content, QStringLiteral("momentsShortsVersionButton"));
    QQuickItem* squareButton = findNamed(content, QStringLiteral("momentsSquareVersionButton"));
    QQuickItem* busyRow = findNamed(content, QStringLiteral("momentsVersionBusy"));
    REQUIRE(shortsButton);
    REQUIRE(squareButton);
    REQUIRE(busyRow);
    CHECK(shortsButton->isEnabled());
    CHECK_FALSE(busyRow->isVisible());

    // Make Shorts version: it returns at once; the step, the bar and Cancel show while the subjects are found.
    g_hold = true;
    const std::size_t before = undo();
    REQUIRE(QMetaObject::invokeMethod(shortsButton, "clicked"));
    REQUIRE(waitFor([&] {
        return moments->version().value(QStringLiteral("text")).toString().endsWith(QStringLiteral("50 %"));
    }, 5000));
    CHECK(moments->version().value(QStringLiteral("busy")).toBool());
    CHECK(moments->version().value(QStringLiteral("text")).toString() ==
          QStringLiteral("Finding the subject 1 of 2 · 50 %"));
    CHECK(busyRow->isVisible());
    CHECK_FALSE(shortsButton->isEnabled());
    CHECK_FALSE(squareButton->isEnabled());
    QQuickItem* versionText = findNamed(content, QStringLiteral("momentsVersionText"));
    QQuickItem* cancel = findNamed(content, QStringLiteral("momentsVersionCancel"));
    QQuickItem* bar = findNamed(content, QStringLiteral("momentsVersionProgress"));
    REQUIRE(versionText);
    REQUIRE(cancel);
    REQUIRE(bar);
    CHECK(versionText->property("text").toString() == QStringLiteral("Finding the subject 1 of 2 · 50 %"));
    CHECK(bar->property("value").toDouble() == Catch::Approx(0.9 * 0.5 / 2));
    CHECK_FALSE(bar->property("indeterminate").toBool());
    // The toast follows it too (a version started from the menus has no popover).
    QObject* toast = shell.window->findChild<QObject*>(QStringLiteral("toast"));
    REQUIRE(toast);
    CHECK(toast->property("text").toString().startsWith(QStringLiteral("Finding the subject")));
    CHECK(toast->property("actionText").toString() == QStringLiteral("Cancel"));
    // Screenshots: the popover with the job, dark and light.
    const auto shoot = [&](const char* file) {
        spin(100);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = image.devicePixelRatio();
        QRectF shown = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
        if (QQuickItem* popup = content->parentItem())
            shown = shown.united(popup->mapRectToScene(QRectF(0, 0, popup->width(), popup->height())));
        const QRectF area = shown.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
        REQUIRE(image.copy(QRectF(area.topLeft() * dpr, area.size() * dpr).toRect())
                    .save(QDir(outputDir()).filePath(QString::fromLatin1(file))));
    };
    shoot("moments-version-job-dark.png");
    shell.setAppearance(false, "ultraviolet");
    shoot("moments-version-job-light.png");
    shell.setAppearance(true, "ultraviolet");
    // The menus and the palette say why they wait; a second one is refused.
    CHECK(moments->commandAvailability(QStringLiteral("moments.shortsVersion")).value(QStringLiteral("reason")).toString() ==
          QStringLiteral("A version is already being made"));
    CHECK(moments->makeVersion(QStringLiteral("square")) == QStringLiteral("A version is already being made"));

    // Cancel: the job stops, nothing changes, the buttons are back.
    REQUIRE(QMetaObject::invokeMethod(cancel, "clicked"));
    REQUIRE(waitFor([&] { return !moments->version().value(QStringLiteral("busy")).toBool(); }, 5000));
    g_hold = false;
    CHECK(undo() == before);
    CHECK(controller->currentSequenceId() == original);
    CHECK(shortsButton->isEnabled());
    CHECK_FALSE(busyRow->isVisible());
    CHECK(toast->property("text").toString() == QStringLiteral("Stopped: nothing was changed"));

    // Again, to the end: one undo step, the version shown, the toast says what it made.
    REQUIRE(moments->makeVersion(QStringLiteral("shorts")).isEmpty());
    REQUIRE(waitFor([&] { return !moments->version().value(QStringLiteral("busy")).toBool(); }, 5000));
    CHECK(undo() == before + 1);
    CHECK(controller->currentSequence()->height == 1920);
    CHECK(toast->property("text").toString() == QStringLiteral("Made Edit (Shorts)"));
    QMetaObject::invokeMethod(popover, "close");
    moments->setRegistry(nullptr);
    moments->setSynchronous(false);
    resetStubs();
}
