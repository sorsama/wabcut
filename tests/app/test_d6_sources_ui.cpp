// 0.7 (stream D, batch D6) in the shell, part two:
//   card 177  the Sources bridge (claims at the playhead, over In-Out, the selected clips or the Transcript tab's words;
//             the unsourced ones stand out; export as a YouTube block, a bibliography, a text file and a PDF; lower
//             thirds) and Window > Sources (screenshots);
//   card 175  the Audio Description bridge (gaps, lines edited by hand, the track, the script) and its dialog (screenshots).
// Screenshots go to <build>/test-output/ui.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "AudioDescriptionBridge.h"
#include "CommandJob.h"
#include "GlobalBridge.h"
#include "ProjectController.h"
#include "SourcesBridge.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QImage>
#include <QQuickItem>
#include <QUrl>
#include <QUuid>

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using Catch::Approx;
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
    const fs::path dir = fs::temp_directory_path() / ("wabcut-d6s-" + QUuid::createUuid().toString(QUuid::Id128).left(8).toStdString()) / name;
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

/// A 16 s sequence (V1 and A1 clips), for the Sources and Audio Description tests.
Timeline longTimeline() {
    return Timeline{"V1 tgt |A" + std::string(399, 'a') + "|\nA1 tgt |A" + std::string(399, 'a') + "|"};
}

} // namespace

TEST_CASE("Sources bridge: a claim at the playhead, over In-Out or the selected clips; the claims without a source stand out",
          "[app][sources][d6]") {
    Timeline tl = longTimeline();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    SourcesBridge sources;
    sources.setController(&controller);
    CHECK(sources.count() == 0);

    controller.setPlayhead(Ticks::fromSeconds(2));
    CHECK(sources.add(QStringLiteral("Everyone agrees")).isEmpty());
    CHECK(sources.count() == 1);
    CHECK(sources.neededCount() == 1); // no source given: a claim that needs one
    REQUIRE(sources.items().size() == 1);
    CHECK(sources.items().first().toMap().value("needed").toBool());
    CHECK(sources.items().first().toMap().value("clock").toString() == "0:02");

    controller.setPlayhead(Ticks::fromSeconds(5));
    const QVariantMap fields{{"url", "https://example.org/report"}, {"title", "Labour Market Report"}, {"author", "A. Author"},
                             {"date", "2024-02-01"}};
    CHECK(sources.add(QStringLiteral("Unemployment fell 4 percent"), fields).isEmpty());
    CHECK(sources.count() == 2);
    CHECK(sources.neededCount() == 1);
    const QVariantList all = sources.items();
    REQUIRE(all.size() == 2);
    CHECK(all[1].toMap().value("title").toString() == "Labour Market Report");
    CHECK_FALSE(all[1].toMap().value("needed").toBool());
    sources.setOnlyNeeded(true);
    REQUIRE(sources.items().size() == 1);
    CHECK(sources.items().first().toMap().value("claim").toString() == "Everyone agrees");
    sources.setOnlyNeeded(false);

    // Over the In-Out range.
    REQUIRE(CommandJob::runNow(&controller, nullptr, "marker.markIn", Json{{"time", Ticks::fromSeconds(8).value}}).ok());
    REQUIRE(CommandJob::runNow(&controller, nullptr, "marker.markOut", Json{{"time", Ticks::fromSeconds(10).value}}).ok());
    CHECK(sources.add(QStringLiteral("A ranged claim"), {{"url", "https://example.org/b"}}, QStringLiteral("marks")).isEmpty());
    const QVariantMap ranged = sources.items().last().toMap();
    CHECK(ranged.value("atSeconds").toDouble() == Approx(8.0));
    CHECK(ranged.value("duration").toDouble() > 1.9);

    // Over the selected clips; with none selected, a message. No claim, nothing.
    controller.setSelection({});
    CHECK_FALSE(sources.add(QStringLiteral("x"), {}, QStringLiteral("selection")).isEmpty());
    CHECK_FALSE(sources.add(QStringLiteral("  ")).isEmpty());
    CHECK_FALSE(sources.add(QStringLiteral("x"), {}, QStringLiteral("transcript")).isEmpty()); // no words selected

    // Editing is one undo step; removing; invalid input is refused with the reason.
    const std::size_t undo = controller.session().undoCount();
    CHECK(sources.update(sources.items().first().toMap().value("index").toInt(),
                         {{"title", "A study"}, {"url", "https://example.org/a"}}).isEmpty());
    CHECK(controller.session().undoCount() == undo + 1);
    CHECK(sources.neededCount() == 0);
    CHECK(sources.remove(sources.items().last().toMap().value("index").toInt()).isEmpty());
    CHECK(sources.count() == 2);
    CHECK_FALSE(sources.update(0, {{"url", "ftp://nope"}}).isEmpty());
    CHECK_FALSE(sources.message().isEmpty());
}

TEST_CASE("Sources bridge: export as a YouTube block, a bibliography, a text file and a PDF; lower thirds", "[app][sources][d6]") {
    Timeline tl = longTimeline();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    SourcesBridge sources;
    sources.setController(&controller);
    controller.setPlayhead(Ticks::fromSeconds(2));
    REQUIRE(sources.add(QStringLiteral("Wages rose too"), {{"url", "https://example.org/report"}, {"title", "Labour Market Report"},
                                                           {"author", "Author, A."}, {"date", "2024-02-01"}}).isEmpty());
    controller.setPlayhead(Ticks::fromSeconds(6));
    REQUIRE(sources.add(QStringLiteral("Still unsourced")).isEmpty());

    const QVariantMap youtube = sources.exportText(QStringLiteral("youtube"));
    CHECK(youtube.value("text").toString().startsWith(QStringLiteral("Sources\n0:02 Wages rose too - Labour Market Report")));
    CHECK(youtube.value("cited").toInt() == 1);
    CHECK(youtube.value("uncited").toInt() == 1);
    CHECK(youtube.value("warnings").toStringList().first().contains(QStringLiteral("still needs a source")));
    CHECK(sources.exportText(QStringLiteral("apa")).value("text").toString().contains(QStringLiteral("(2024, February 1)")));
    CHECK(sources.exportText(QStringLiteral("nonsense")).value("text").toString().isEmpty());

    const fs::path dir = workDir("sources");
    CHECK(sources.exportFile(url(dir / "sources.txt"), QStringLiteral("youtube")).isEmpty());
    std::string written = readAll(dir / "sources.txt");
    std::erase(written, '\r'); // a text file gets the platform's line ends
    CHECK(written.rfind("Sources\n0:02 Wages rose too", 0) == 0);
    CHECK(sources.exportFile(url(dir / "sources.pdf"), QStringLiteral("apa")).isEmpty());
    CHECK(readAll(dir / "sources.pdf").rfind("%PDF", 0) == 0);
    CHECK(fs::file_size(dir / "sources.pdf") > 500);

    const std::size_t undo = controller.session().undoCount();
    CHECK(sources.addLowerThirds(3.0).isEmpty());
    CHECK(controller.session().undoCount() == undo + 1);
    int graphics = 0;
    for (const TrackPtr& track : controller.currentSequence()->videoTracks)
        for (const Clip& clip : track->clips)
            graphics += clip.graphic() ? 1 : 0;
    CHECK(graphics == 1); // the claim without a source shows nothing
    REQUIRE(controller.undo());

    // Nothing cited, nothing to write.
    SourcesBridge empty;
    ProjectController other;
    other.resetProject(tl.state, tl.seq);
    empty.setController(&other);
    CHECK_FALSE(empty.exportFile(url(dir / "none.txt"), QStringLiteral("youtube")).isEmpty());
    CHECK_FALSE(fs::exists(dir / "none.txt"));
}

TEST_CASE("Window > Sources: the panel lists the claims by time with the unsourced ones flagged; screenshots",
          "[app][sources][d6][qml]") {
    Shell shell;
    REQUIRE(shell.window != nullptr);
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* sources = shell.engine.singletonInstance<SourcesBridge*>("Wabcut", "Sources");
    REQUIRE(controller != nullptr);
    REQUIRE(sources != nullptr);
    Timeline tl = longTimeline();
    controller->resetProject(tl.state, tl.seq);
    spin(200);
    controller->setPlayhead(Ticks::fromSeconds(2));
    REQUIRE(sources->add(QStringLiteral("Unemployment fell 4 percent in 2023"),
                         {{"url", "https://example.org/report"}, {"title", "Labour Market Report"}, {"author", "A. Author"},
                          {"publisher", "Example Statistics"}, {"date", "2024-02-01"}}).isEmpty());
    controller->setPlayhead(Ticks::fromSeconds(6));
    REQUIRE(sources->add(QStringLiteral("Most people agree")).isEmpty());
    controller->setPlayhead(Ticks::fromSeconds(11));
    REQUIRE(sources->add(QStringLiteral("Wages rose faster than prices"),
                         {{"url", "https://example.com/wages"}, {"title", "Wages and Prices"}, {"date", "2023"}}).isEmpty());

    runCommand(shell, "window.sources");
    spin(400);
    QQuickItem* panel = shell.item("sourcesPanel");
    REQUIRE(panel != nullptr);
    REQUIRE(panel->isVisible());
    QQuickItem* list = findNamed(panel, QStringLiteral("sourcesList"));
    REQUIRE(list != nullptr);
    CHECK(list->property("count").toInt() == 3);
    QQuickItem* summary = findNamed(panel, QStringLiteral("sourcesSummary"));
    REQUIRE(summary != nullptr);
    CHECK(summary->property("text").toString().contains(QStringLiteral("1 without a source")));

    const QRectF shot = sceneRect(panel).adjusted(-6, -6, 6, 6);
    saveCrop(shell, shot, QStringLiteral("sources-panel.png"));
    // The form for a new claim opens from the + button and takes the panel's room (the list is hidden meanwhile).
    QMetaObject::invokeMethod(panel, "openAdd");
    spin(200);
    QQuickItem* form = findNamed(panel, QStringLiteral("sourcesForm"));
    REQUIRE(form != nullptr);
    CHECK(form->isVisible());
    CHECK_FALSE(list->isVisible());
    saveCrop(shell, shot, QStringLiteral("sources-form.png"));
    QMetaObject::invokeMethod(panel, "closeForm");
    spin(100);
    CHECK_FALSE(form->isVisible());
    CHECK(list->isVisible());
    // Needs a source: only the unsourced claim.
    sources->setOnlyNeeded(true);
    spin(150);
    CHECK(list->property("count").toInt() == 1);
    sources->setOnlyNeeded(false);
    spin(100);

    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    spin(100);
    saveCrop(shell, shot, QStringLiteral("sources-panel-light.png"));
}

TEST_CASE("Audio description bridge: nothing without a project, no model, no track", "[app][audioDescription][d6]") {
    AudioDescriptionBridge ad; // no engine: no project, no AI
    CHECK_FALSE(ad.analyze({{"silent", true}}).isEmpty());
    CHECK(ad.lines().isEmpty());
    CHECK_FALSE(ad.apply().isEmpty());
    CHECK_FALSE(ad.voice().isEmpty()); // nothing is on a track yet
    CHECK_FALSE(ad.describe({}).isEmpty());
    CHECK_FALSE(ad.quote().value("available").toBool());
    CHECK_FALSE(ad.setLineText(0, QStringLiteral("x")).isEmpty()); // no such line
}

TEST_CASE("Sequence > Generate Audio Description: gaps, lines edited by hand, the track, the script; screenshots",
          "[app][audioDescription][d6][qml]") {
    Shell shell;
    REQUIRE(shell.window != nullptr);
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* ad = shell.engine.singletonInstance<AudioDescriptionBridge*>("Wabcut", "AudioDescription");
    REQUIRE(controller != nullptr);
    REQUIRE(ad != nullptr);
    Timeline tl = longTimeline();
    controller->resetProject(tl.state, tl.seq);
    spin(200);

    // A video without speech: the whole 16 s is silence, cut in two pieces of 8 s (20 words at 2.5 a second).
    CHECK(ad->analyze({{"silent", true}}).isEmpty());
    REQUIRE(ad->lines().size() == 2);
    const QVariantMap first = ad->lines().first().toMap();
    CHECK(first.value("maxWords").toInt() == 20);
    CHECK(first.value("text").toString().isEmpty());
    CHECK(ad->setLineText(0, QStringLiteral("  A woman opens a\n red door and steps inside.  ")).isEmpty());
    CHECK(ad->lines().first().toMap().value("text").toString() == "A woman opens a red door and steps inside.");
    CHECK(ad->lines().first().toMap().value("words").toInt() == 9);
    CHECK_FALSE(ad->lines().first().toMap().value("over").toBool());
    // A line longer than its gap fits is flagged, not cut: it is the person's to shorten.
    ad->setLineText(1, QStringLiteral("one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen "
                                      "seventeen eighteen nineteen twenty twenty-one"));
    CHECK(ad->lines().last().toMap().value("over").toBool());
    ad->removeLine(1);
    CHECK(ad->lines().last().toMap().value("text").toString().isEmpty());

    // The script for a narrator, and the track (switched off, one undo step).
    const QString script = ad->scriptText();
    CHECK(script.contains(QStringLiteral("A woman opens a red door and steps inside.")));
    CHECK(script.contains(QStringLiteral("00:00:00:00")));
    const fs::path dir = workDir("ad");
    CHECK(ad->exportScript(url(dir / "script.txt")).isEmpty());
    CHECK(readAll(dir / "script.txt").find("A woman opens a red door") != std::string::npos);
    const std::size_t undo = controller->session().undoCount();
    CHECK(ad->apply(QStringLiteral("en")).isEmpty());
    CHECK(controller->session().undoCount() == undo + 1);
    const Sequence& seq = *controller->currentSequence();
    REQUIRE(seq.captionTracks.size() == 1);
    CHECK(seq.captionTracks.front().name == "Audio Description");
    CHECK_FALSE(seq.captionTracks.front().enabled);
    REQUIRE(seq.captionTracks.front().cues.size() == 1);
    CHECK(seq.captionTracks.front().cues.front().text == "A woman opens a red door and steps inside.");
    CHECK(ad->captionTrack() == static_cast<qint64>(seq.captionTracks.front().id.value));

    // Voice…: the Dub dialog is prefilled for that track (the voice, cost and consent are asked there). The dialog takes the
    // proposal and clears it, so it is read as it is made.
    auto* dub = shell.engine.singletonInstance<DubBridge*>("Wabcut", "Dub");
    REQUIRE(dub != nullptr);
    QVariantMap proposed;
    QObject::connect(dub, &DubBridge::proposalChanged, dub, [&] {
        if (proposed.isEmpty())
            proposed = dub->proposal();
    });
    CHECK(ad->voice(QStringLiteral("en")).isEmpty());
    CHECK(proposed.value("kind").toString() == "dub");
    CHECK(proposed.value("name").toString() == "Audio Description");
    CHECK(proposed.value("captionTrack").toLongLong() == ad->captionTrack());
    CHECK(proposed.value("duck").toDouble() == Approx(-12.0));
    CHECK(proposed.value("language").toString() == "en");
    dub->clearProposal();

    // Without a model that sees pictures nothing is sent: a reason, never a quiet fallback.
    const QVariantMap quote = ad->quote({});
    if (!quote.value("available").toBool()) {
        CHECK_FALSE(quote.value("reason").toString().isEmpty());
        CHECK_FALSE(ad->describe({{"allowOnce", false}}).isEmpty());
    }

    // The dialog: open with the gaps, two lines, light and dark.
    REQUIRE(ad->analyze({{"silent", true}}).isEmpty());
    ad->setLineText(0, QStringLiteral("A woman opens a red door and steps inside."));
    ad->setLineText(1, QStringLiteral("Rain falls on an empty street at night."));
    runCommand(shell, "sequence.generateAudioDescription");
    spin(400);
    QObject* dialog = shell.window->findChild<QObject*>(QStringLiteral("audioDescriptionDialog"));
    REQUIRE(dialog != nullptr);
    CHECK(dialog->property("opened").toBool());
    QQuickItem* lines = findNamed(shell.window->contentItem(), QStringLiteral("audioDescriptionLines"));
    REQUIRE(lines != nullptr);
    CHECK(lines->property("count").toInt() >= 1);
    REQUIRE(findNamed(shell.window->contentItem(), QStringLiteral("audioDescriptionGapSummary")) != nullptr);
    const QRectF area = sceneRect(lines).adjusted(-80, -520, 80, 160);
    saveCrop(shell, area, QStringLiteral("audio-description-dialog.png"));
    shell.setAppearance(false, "ultraviolet");
    REQUIRE(shell.settle(false, shell.themeColor("accentFill")));
    spin(100);
    saveCrop(shell, area, QStringLiteral("audio-description-dialog-light.png"));
    QQuickItem* close = findNamed(shell.window->contentItem(), QStringLiteral("audioDescriptionClose"));
    REQUIRE(close != nullptr);
    QMetaObject::invokeMethod(close, "clicked");
    spin(300);
    CHECK_FALSE(dialog->property("opened").toBool());
}
