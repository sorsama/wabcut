// AI-8 smart search in the Library, on the real Main.qml with fixture embeddings (no model, no decoder): the Smart
// toggle indexes the demo project's media (the pill), a query shows result cards (thumbnail, range badge, snippet with
// the matched words), a hit dropped on the timeline inserts its source range, and plain typing filters by name.
// Screenshots: <build>/test-output/ui/smart-search.png and smart-search-library.png.
// 0.4: a hit opens in the Source Monitor with its range marked (double-click or Enter; Tab reaches the list, Up and
// Down move between the cards). Screenshot: search-hit-source-range.png.

#include "ShellFixture.h"

#include "BridgeIds.h"
#include "LibraryBridge.h"
#include "LibraryModel.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "SearchBridge.h"
#include "SourceMonitorBridge.h"
#include "TimelineFixture.h"
#include "search/SearchTestUtil.h"

#include <wabcut/ai/search/FootageSearch.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickWindow>

#include <cstdlib>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using namespace wabcut::ai::search;
using namespace wabcut::ai::search::test;

namespace {

/// Fixture frames with a little depth for the screenshot: the shot's colour, darker towards the bottom.
class ShadedAccess : public FixtureAccess {
public:
    std::expected<std::vector<Image>, Error> frames(const MediaItem& media, std::span<const Ticks> times, int width,
                                                    std::stop_token stop) override {
        auto out = FixtureAccess::frames(media, times, width, stop);
        if (out) {
            for (Image& image : *out) {
                for (int y = 0; y < image.height; ++y) {
                    const double shade = 1.0 - 0.45 * y / std::max(1, image.height - 1);
                    for (int x = 0; x < image.width; ++x) {
                        auto* px = &image.rgb[(static_cast<std::size_t>(y) * image.width + x) * 3];
                        for (int c = 0; c < 3; ++c) {
                            px[c] = static_cast<std::uint8_t>(px[c] * shade);
                        }
                    }
                }
            }
        }
        return out;
    }
};

const MediaItem* named(const ProjectState& state, std::string_view name) {
    for (const MediaPtr& m : state.media) {
        if (m->name == name) {
            return m.get();
        }
    }
    return nullptr;
}

/// The demo project's files as smart-search fixtures (what each shot shows, what is said), indexed under `dir`.
void useFixtureSearch(SearchBridge& search, ProjectController& project, const TempDir& dir) {
    SearchBridge* smart = &search;
    ProjectController* controller = &project;
    // The demo project's files, as fixtures: what each shot shows (a colour) and what is said.
    auto access = std::make_shared<ShadedAccess>();
    const Rgb sea{40, 90, 210};
    const Rgb coast{70, 160, 90};
    const Rgb neon{215, 50, 70};
    const Rgb studio{110, 105, 120};
    const auto state = controller->snapshot();
    const auto add = [&](std::string_view name, std::uint64_t fingerprint, std::vector<double> cuts, std::vector<Rgb> colours,
                         std::shared_ptr<const ai::Transcript> words = nullptr) {
        const MediaItem* m = named(*state, name);
        REQUIRE(m != nullptr);
        access->items[m->id.value] = FixtureAccess::Item{fingerprint, "2026-10-0" + std::to_string(fingerprint % 9 + 1),
                                                         shotsAt(std::max(1.0, m->info.duration.toSeconds()), std::move(cuts)),
                                                         std::move(colours), std::move(words), false};
    };
    add("Interview A.mov", 1, {}, {studio},
        spoken("en", {{"we", 20, 20.3}, {"filmed", 20.3, 20.8}, {"the", 20.8, 21}, {"coast", 21, 21.6}, {"at", 21.6, 21.8},
                      {"sunrise", 21.8, 22.6}}));
    add("Interview B.mov", 2, {}, {studio},
        spoken("th", {{"วันนี้", 3, 3.5}, {"เรา", 3.5, 3.8}, {"จะ", 3.8, 4}, {"ไป", 4, 4.3}, {"ทะเล", 4.3, 5}}));
    add("City B-roll.mp4", 3, {8}, {neon, studio});
    add("Drone Coast.mp4", 4, {12, 25}, {sea, coast, sea});
    add("Street Walk.mov", 5, {}, {coast},
        spoken("en", {{"the", 4, 4.2}, {"blue", 4.2, 4.6}, {"door", 4.6, 5.1}, {"on", 5.1, 5.3}, {"the", 5.3, 5.4},
                      {"left", 5.4, 5.9}}));
    add("Product Close-up.mp4", 6, {}, {neon});
    add("Music Bed - Lofi.wav", 7, {}, {studio});
    add("Whoosh SFX.wav", 8, {}, {studio});
    add("VO Take 3.wav", 9, {}, {studio},
        spoken("en", {{"a", 1, 1.1}, {"calm", 1.1, 1.5}, {"blue", 1.5, 1.9}, {"coast", 1.9, 2.5}, {"at", 2.5, 2.7}, {"dawn", 2.7, 3.2}}));
    add("Lower Third.png", 10, {}, {studio});
    add("Title Card.png", 11, {}, {neon});
    add("Logo Bug.png", 12, {}, {coast});
    add("Outro.mov", 13, {}, {sea});
    auto embedder = std::make_shared<FixtureEmbedder>();
    auto index = SearchIndex::open(dir.path / "index", embedder->modelVersion(), kFixtureDim);
    REQUIRE(index);
    FootageSearchOptions options;
    options.cacheRoot = dir.path / "analysis";
    smart->setSearch(std::make_shared<FootageSearch>(options, std::shared_ptr<SearchIndex>(index->release()), access, embedder));
}

} // namespace

TEST_CASE("Smart search in the Library: index, result cards, drop a hit with its range; screenshots", "[ai][ui][search][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* smart = shell.engine.singletonInstance<SearchBridge*>("Wabcut", "SmartSearch");
    REQUIRE(controller != nullptr);
    REQUIRE(smart != nullptr);

    TempDir dir("ui");
    useFixtureSearch(*smart, *controller, dir);

    QQuickItem* bar = shell.item("librarySearchBar");
    QQuickItem* library = shell.item("libraryPanel");
    REQUIRE(bar != nullptr);
    REQUIRE(library != nullptr);
    CHECK_FALSE(smart->upToDate());

    // Smart on: what is not indexed gets indexed in the background; the pill counts it.
    bar->setProperty("smart", true);
    REQUIRE(smart->waitForIdle(20'000));
    CHECK(smart->upToDate());
    CHECK(smart->indexText() == QStringLiteral("Indexed 13/13"));

    // A query: frames that look like it and words that say it, best first; the grid gives way to the results.
    bar->setProperty("query", QStringLiteral("blue coast"));
    REQUIRE(waitFor([&] { return !smart->results().isEmpty() && !smart->searching(); }, 5000));
    const QVariantList results = smart->results();
    REQUIRE(results.size() >= 4);
    const QVariantMap first = results.front().toMap();
    INFO("first hit: " << first.value(QStringLiteral("mediaName")).toString().toStdString());
    CHECK(first.value(QStringLiteral("visual")).toBool());
    CHECK(first.value(QStringLiteral("transcript")).toBool()); // seen and heard ranks first
    bool sawWords = false;
    for (const QVariant& r : results) {
        const QVariantMap hit = r.toMap();
        CHECK_FALSE(hit.value(QStringLiteral("rangeText")).toString().isEmpty());
        CHECK(hit.value(QStringLiteral("thumbnail")).toString().startsWith(QStringLiteral("image://wabcut-search/m")));
        if (hit.value(QStringLiteral("snippetRich")).toString().contains(QStringLiteral("<b>coast</b>"))) {
            sawWords = true;
        }
    }
    CHECK(sawWords);
    QQuickItem* panel = shell.item("smartSearchPanel");
    REQUIRE(panel != nullptr);
    CHECK(panel->isVisible());
    spin(600); // the thumbnails decode off the GUI thread

    {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(outputDir() + QStringLiteral("/smart-search.png")));
        const double dpr = shell.window->effectiveDevicePixelRatio();
        const QRectF r = library->mapRectToScene(QRectF(0, 0, library->width(), library->height()));
        REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(outputDir() + QStringLiteral("/smart-search-library.png")));
    }

    // Drag a hit onto V1 at the start: its source range is inserted, one undo step.
    const auto sequence = controller->currentSequence();
    REQUIRE(sequence);
    REQUIRE_FALSE(sequence->videoTracks.empty());
    const QString v1 = trackKey(sequence->videoTracks[0]->id);
    QVariantMap drone;
    for (const QVariant& r : results) {
        if (r.toMap().value(QStringLiteral("mediaName")).toString() == QStringLiteral("Drone Coast.mp4")) {
            drone = r.toMap();
            break;
        }
    }
    REQUIRE_FALSE(drone.isEmpty());
    const std::size_t undo = controller->session().undoCount();
    REQUIRE(smart->dropHit(drone.value(QStringLiteral("hit")).toString(), QStringLiteral("00:00:00:00"), v1, false));
    CHECK(controller->session().undoCount() == undo + 1);
    const auto after = controller->currentSequence();
    const Clip& placed = after->videoTracks[0]->clips.front();
    CHECK(placed.media == named(*controller->snapshot(), "Drone Coast.mp4")->id);
    // The range lands on the sequence's frames (23.976 fps): within a frame of the hit.
    const std::int64_t frame = framesToTicks(1, after->frameRate).value;
    CHECK(std::llabs(placed.sourceIn.value - drone.value(QStringLiteral("sourceIn")).toString().toLongLong()) <= frame);
    CHECK(std::llabs(placed.sourceIn.value + placed.duration().value - drone.value(QStringLiteral("sourceOut")).toString().toLongLong()) <=
          frame);
    CHECK_FALSE(smart->dropHit(QStringLiteral("{\"media\":\"m999\",\"sourceIn\":\"0\",\"sourceOut\":\"10\"}"), QStringLiteral("00:00:00:00"), v1, false));
    CHECK_FALSE(smart->dropHit(QStringLiteral("not json"), QStringLiteral("00:00:00:00"), v1, false));

    // Smart off: the query filters the library by name, as before (M5k: the Library's own model, Library.findText).
    bar->setProperty("smart", false);
    bar->setProperty("query", QStringLiteral("drone"));
    spin(50);
    CHECK_FALSE(panel->isVisible());
    // Since M5k the Library's rows live in the Library bridge's model (Find text = the plain query).
    auto* libraryBridge = shell.engine.singletonInstance<wabcut::app::LibraryBridge*>("Wabcut", "Library");
    REQUIRE(libraryBridge != nullptr);
    CHECK(libraryBridge->findText() == QStringLiteral("drone"));
    wabcut::app::LibraryModel* rows = libraryBridge->items();
    REQUIRE(rows != nullptr);
    REQUIRE(rows->rowCount() > 0);
    for (int row = 0; row < rows->rowCount(); ++row) {
        const QString name = rows->data(rows->index(row, 0), wabcut::app::LibraryModel::NameRole).toString();
        CHECK(name.contains(QStringLiteral("Drone"), Qt::CaseInsensitive));
    }
    bar->setProperty("query", QString());

    const QStringList messages = takeMessages();
    for (const QString& m : messages) {
        INFO(m.toStdString());
        CHECK_FALSE((m.contains(QStringLiteral("qml")) || m.contains(QStringLiteral("Search"))));
    }
}

TEST_CASE("Smart search hit opens in the Source Monitor with its range marked", "[ai][ui][search]") {
    // 0.4: a result card's double-click (and Enter) call SourceMonitor.loadRange with the hit's media and its range as
    // tick strings: the Source Monitor gets the clip with that range marked, the Program Monitor keeps the sequence.
    resetSettings();
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )",
                {{'S', 250}});
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    PlaybackBridge playback(&controller);
    SourceMonitorBridge source;
    source.setController(&controller);
    source.setProgramPlayback(&playback);
    const MediaId media = tl.media('S');
    const qint64 in = Ticks::fromSeconds(2).value + 7;  // inside frame 50 (25 fps)
    const qint64 out = Ticks::fromSeconds(5).value - 3; // the range's last tick is inside frame 124
    const std::size_t undo = controller.session().undoCount();
    REQUIRE(source.loadRange(mediaKey(media), QString::number(in), QString::number(out)));
    CHECK(source.mediaId() == media);
    CHECK(source.hasIn());
    CHECK(source.hasOut());
    const MediaItem* item = controller.snapshot()->findMedia(media);
    REQUIRE(item != nullptr);
    REQUIRE(item->markIn);
    REQUIRE(item->markOut);
    CHECK(*item->markIn == f(50));   // the start of the frame holding the hit's start
    CHECK(*item->markOut == f(125)); // the end of the frame holding its last tick
    CHECK(source.positionTicks() == f(50));
    CHECK_FALSE(playback.sourceMode()); // no one-clip takeover of the Program Monitor
    CHECK(controller.session().undoCount() == undo + 1);
    CHECK(controller.undoText().contains(QStringLiteral("Source Marks")));
    // One undo clears the marks; the clip stays loaded.
    REQUIRE(controller.undo());
    CHECK_FALSE(source.hasIn());
    CHECK_FALSE(source.hasOut());
    CHECK(source.mediaId() == media);
    // Unknown media, and a range that is empty or backwards: false, with the Source Monitor's message.
    CHECK_FALSE(source.loadRange(QStringLiteral("m999"), QStringLiteral("0"), QStringLiteral("10")));
    CHECK_FALSE(source.message().isEmpty());
    CHECK_FALSE(source.loadRange(mediaKey(media), QString::number(out), QString::number(in)));
    CHECK_FALSE(source.loadRange(mediaKey(media), QStringLiteral("abc"), QString::number(out)));
    CHECK(controller.session().undoCount() == undo);
}

TEST_CASE("Smart search hit opens with Return and the list takes Up and Down", "[ai][ui][search][screenshots]") {
    // 0.4: Tab from the search field reaches the results; Up / Down move between the cards (not the keymap's); Enter
    // opens the focused hit in the Source Monitor with its range marked, as a double-click does.
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* smart = shell.engine.singletonInstance<SearchBridge*>("Wabcut", "SmartSearch");
    auto* source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    auto* playback = shell.engine.singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
    REQUIRE(controller != nullptr);
    REQUIRE(smart != nullptr);
    REQUIRE(source != nullptr);
    REQUIRE(playback != nullptr);
    TempDir dir("ui-open");
    useFixtureSearch(*smart, *controller, dir);
    QQuickItem* bar = shell.item("librarySearchBar");
    REQUIRE(bar != nullptr);
    bar->setProperty("smart", true);
    REQUIRE(smart->waitForIdle(20'000));
    bar->setProperty("query", QStringLiteral("blue coast"));
    REQUIRE(waitFor([&] { return smart->results().size() >= 3 && !smart->searching(); }, 5000));
    spin(100);
    const QVariantList results = smart->results();

    const auto key = [&](int code, const QString& text = {}) {
        QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier, text);
        QCoreApplication::sendEvent(shell.window.get(), &press);
        QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier, text);
        QCoreApplication::sendEvent(shell.window.get(), &release);
    };
    const auto focusedCard = [&]() -> QQuickItem* {
        QQuickItem* f = shell.window->activeFocusItem();
        return f && f->objectName() == QStringLiteral("searchHitCard") ? f : nullptr;
    };
    QQuickItem* list = shell.item("smartSearchResults");
    REQUIRE(list != nullptr);
    // Tab from the search field reaches the first card.
    QQuickItem* field = shell.item("librarySearchField");
    REQUIRE(field != nullptr);
    field->forceActiveFocus(Qt::TabFocusReason);
    REQUIRE(field->hasActiveFocus());
    for (int i = 0; i < 10 && focusedCard() == nullptr; ++i)
        key(Qt::Key_Tab);
    REQUIRE(focusedCard() != nullptr);
    CHECK(list->property("currentIndex").toInt() == 0);
    // Down and Up move between the cards (the playhead stays: the keymap's Up / Down do not run).
    const QString playhead = controller->playheadText();
    key(Qt::Key_Down);
    CHECK(list->property("currentIndex").toInt() == 1);
    REQUIRE(focusedCard() != nullptr);
    key(Qt::Key_Down);
    key(Qt::Key_Up);
    CHECK(list->property("currentIndex").toInt() == 1);
    CHECK(controller->playheadText() == playhead);
    // Enter opens the focused hit: its clip in the Source Monitor with the range marked; the Program keeps the sequence.
    const QVariantMap hit = results.at(1).toMap();
    const std::size_t undo = controller->session().undoCount();
    key(Qt::Key_Return, QStringLiteral("\r"));
    CHECK(waitFor([&] { return source->property("loaded").toBool(); }, 3000));
    CHECK(mediaKey(source->mediaId()) == hit.value(QStringLiteral("mediaId")).toString());
    CHECK(source->hasIn());
    CHECK(source->hasOut());
    CHECK_FALSE(playback->sourceMode());
    CHECK(controller->session().undoCount() == undo + 1); // the marks: one undo step
    QQuickItem* sourcePanel = shell.item("sourceMonitorPanel");
    REQUIRE(sourcePanel != nullptr);
    CHECK(sourcePanel->isVisible());
    spin(600); // the thumbnails decode off the GUI thread
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    CHECK(image.save(outputDir() + QStringLiteral("/search-hit-source-range.png")));

    bar->setProperty("query", QString());
    const QStringList messages = takeMessages();
    for (const QString& m : messages) {
        INFO(m.toStdString());
        CHECK_FALSE((m.contains(QStringLiteral("qml")) || m.contains(QStringLiteral("Search"))));
    }
}
