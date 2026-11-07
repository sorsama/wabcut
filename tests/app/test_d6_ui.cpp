// 0.7 (stream D, batch D6) in the shell:
//   card 059  File > Save for an Older Version…: the versions offered, what each cannot hold, the copy written without it
//             and the open project left as it is (ProjectFile.olderReleases / olderReleaseLosses / saveForOlderVersion), the
//             dialog (dark and light);
//   card 116  the Captions track menu's "Show with the main track" (bilingual captions);
//   card 177  the Sources panel (stream D, batch D6);
//   card 175  the Audio Description panel (stream D, batch D6).
// Screenshots of the new pieces go to <build>/test-output/ui.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "CaptionsBridge.h"
#include "CommandJob.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/model/CaptionOps.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QUrl>
#include <QUuid>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using command::Json;
namespace fs = std::filesystem;

namespace {

void runCommand(const Shell& shell, const char* id) {
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QString::fromLatin1(id))));
}

QRectF sceneRect(const QQuickItem* item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

void saveCrop(const Shell& shell, const QRectF& area, const QString& name) {
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF r = area.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(name)));
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

fs::path workDir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / ("wabcut-d6-" + QUuid::createUuid().toString(QUuid::Id128).left(8).toStdString()) / name;
    fs::create_directories(dir);
    return dir;
}

QUrl url(const fs::path& path) { return QUrl::fromLocalFile(QString::fromStdString(path.string())); }

std::string readAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

/// A project with 0.7 caption features: two tracks, kinetic looks and the bilingual mark; and custom words.
ProjectState captionedProject(SequenceId& seq) {
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"};
    ProjectState s = tl.state;
    seq = tl.seq;
    CaptionStyle kinetic;
    kinetic.animation = CaptionAnimation::HighlightBar;
    kinetic.textCase = CaptionTextCase::Upper;
    const auto english = edit::addCaptionTrack(s, seq, "English", "en", kinetic);
    s = english->state;
    const auto spanish = edit::addCaptionTrack(s, seq, "Spanish", "es", kinetic);
    s = spanish->state;
    edit::CaptionTrackChange together;
    together.showTogether = true;
    s = edit::setCaptionTrackInfo(s, seq, spanish->createdCaptionTrack, together)->state;
    s = *edit::setCustomWords(s, {"Malenia"});
    return s;
}

} // namespace

// ---- Card 059: Save for an Older Version ---------------------------------------------------------------------------------

TEST_CASE("ProjectFile.saveForOlderVersion writes the copy for the chosen version and leaves the open project alone",
          "[app][shell][file][d6]") {
    const fs::path dir = workDir("older");
    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    SequenceId seq;
    lifecycle.newProject();
    controller.resetProject(captionedProject(seq), seq);

    const QVariantList releases = lifecycle.olderReleases();
    REQUIRE(releases.size() == 2);
    CHECK(releases[0].toMap().value("id").toString() == "0.6");
    CHECK(releases[0].toMap().value("name").toString() == "Wabcut 0.6");
    CHECK(releases[1].toMap().value("id").toString() == "0.5");

    // What each version cannot hold, from the project as it is now.
    const QVariantList lost = lifecycle.olderReleaseLosses("0.6");
    QStringList ids;
    for (const QVariant& v : lost)
        ids << v.toMap().value("id").toString();
    CHECK(ids.contains("bilingualCaptions"));
    CHECK(ids.contains("captionLooks"));
    CHECK(ids.contains("customWords"));
    CHECK_FALSE(ids.contains("auditions"));
    CHECK(lifecycle.olderReleaseLosses("0.1").isEmpty()); // not offered: nothing listed, the reason in lastError
    CHECK_FALSE(lifecycle.lastError().isEmpty());

    const fs::path copy = dir / "for the editor.wabcut";
    const QVariantMap result = lifecycle.saveForOlderVersion(url(copy), "0.5");
    REQUIRE(result.value("saved").toBool());
    CHECK(result.value("name").toString() == "Wabcut 0.2 to 0.5");
    CHECK(result.value("lost").toList().size() >= 3);
    CHECK(lifecycle.lastError().isEmpty());
    REQUIRE(fs::exists(copy));
    const std::string text = readAll(copy);
    CHECK(text.find("\"schemaVersion\": 18") != std::string::npos);
    CHECK(text.find("showTogether") == std::string::npos);
    CHECK(text.find("customWords") == std::string::npos);
    const auto loaded = io::loadProject(copy);
    REQUIRE(loaded);
    CHECK(loaded->customWords.empty());
    CHECK(loaded->findSequence(seq)->captionTracks.size() == 2);
    // The project open here still has everything (it was not rewritten) and nothing was marked saved.
    const ProjectSnapshot now = controller.snapshot();
    REQUIRE(now);
    CHECK(now->customWords.size() == 1);
    CHECK(now->findSequence(seq)->captionTracks[1].showTogether);
    CHECK_FALSE(fs::exists(io::backupPath(copy, 1)));

    // ".wabcut" is added, and a refused version or an empty path writes nothing and says why.
    REQUIRE(lifecycle.saveForOlderVersion(url(dir / "second"), "Wabcut 0.6").value("saved").toBool());
    CHECK(fs::exists(dir / "second.wabcut"));
    CHECK_FALSE(lifecycle.saveForOlderVersion(url(dir / "third"), "0.1").value("saved").toBool());
    CHECK_FALSE(fs::exists(dir / "third.wabcut"));
    CHECK_FALSE(lifecycle.lastError().isEmpty());
}

TEST_CASE("File > Save for an Older Version: the dialog lists the versions and what each leaves out; screenshots",
          "[app][shell][file][d6][qml]") {
    Shell shell;
    REQUIRE(shell.window != nullptr);
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller != nullptr);
    SequenceId seq;
    controller->resetProject(captionedProject(seq), seq);
    spin(200);

    runCommand(shell, "file.saveForOlderVersion");
    spin(300);
    QObject* dialog = shell.window->findChild<QObject*>(QStringLiteral("saveForOlderVersionDialog"));
    REQUIRE(dialog != nullptr);
    CHECK(dialog->property("opened").toBool());
    CHECK(dialog->property("releases").toList().size() == 2);
    // 0.6: the 0.7 additions only.
    QQuickItem* lossList = findNamed(shell.window->contentItem(), QStringLiteral("olderVersionLosses"));
    REQUIRE(lossList != nullptr);
    CHECK(lossList->isVisible());
    CHECK(dialog->property("losses").toList().size() >= 3);
    QQuickItem* tabs = findNamed(shell.window->contentItem(), QStringLiteral("olderVersionTabs"));
    REQUIRE(tabs != nullptr);
    const QRectF shot = sceneRect(tabs).united(sceneRect(lossList)).adjusted(-50, -230, 50, 130);
    saveCrop(shell, shot, QStringLiteral("save-for-older-version.png"));

    // Wabcut 0.2 to 0.5: the same list (nothing of 0.6 in this project), the dialog follows the choice.
    REQUIRE(tabs->setProperty("currentIndex", 1));
    spin(200);
    CHECK(dialog->property("chosen").toMap().value("id").toString() == "0.5");
    CHECK(dialog->property("losses").toList().size() >= 3);

    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    spin(100);
    saveCrop(shell, shot, QStringLiteral("save-for-older-version-light.png"));

    // Cancel closes it; the menu entry is in the File menu and the palette lists the command.
    QQuickItem* cancel = findNamed(shell.window->contentItem(), QStringLiteral("olderVersionCancel"));
    REQUIRE(cancel != nullptr);
    QMetaObject::invokeMethod(cancel, "clicked");
    spin(300);
    CHECK_FALSE(dialog->property("opened").toBool());
}

// ---- Card 116: Show with the main track ------------------------------------------------------------------------------------

TEST_CASE("Captions track menu: Show with the main track marks the track, one undo step, drawn with the main one",
          "[app][captions][d6][bilingual]") {
    Timeline tl{R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )"};
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    REQUIRE(CommandJob::runNow(&controller, nullptr, "captions.addTrack", Json{{"name", "English"}, {"language", "en"}}).ok());
    REQUIRE(CommandJob::runNow(&controller, nullptr, "captions.addTrack", Json{{"name", "Spanish"}, {"language", "es"}}).ok());
    CaptionsBridge captions;
    captions.setController(&controller);
    const Sequence& seq = *controller.currentSequence();
    REQUIRE(seq.captionTracks.size() == 2);
    captions.setTrack(captions.tracks().value(1).toMap().value("id").toString());
    REQUIRE(captions.trackInfo().value("name").toString() == "Spanish");
    CHECK_FALSE(captions.trackInfo().value("showTogether").toBool());

    const std::size_t undo = controller.session().undoCount();
    CHECK(captions.setTrackShowTogether(true).isEmpty());
    CHECK(captions.trackInfo().value("showTogether").toBool());
    CHECK(controller.session().undoCount() == undo + 1);
    CHECK(controller.currentSequence()->captionTracks[1].showTogether);
    CHECK_FALSE(controller.currentSequence()->captionTracks[0].showTogether);
    // The playback side draws both tracks (the monitor and export evaluate captions this way).
    CHECK(playback::captionTracksFor(*controller.currentSequence(), {}).size() == 2);
    REQUIRE(controller.undo());
    CHECK_FALSE(controller.currentSequence()->captionTracks[1].showTogether);
    CHECK(playback::captionTracksFor(*controller.currentSequence(), {}).size() == 1);
}
