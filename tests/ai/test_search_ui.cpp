// AI-8 smart search in the Library, on the real Main.qml with fixture embeddings (no model, no decoder): the Smart
// toggle indexes the demo project's media (the pill), a query shows result cards (thumbnail, range badge, snippet with
// the matched words), a hit dropped on the timeline inserts its source range, and plain typing filters by name.
// Screenshots: <build>/test-output/ui/smart-search.png and smart-search-library.png.

#include "ShellFixture.h"

#include "BridgeIds.h"
#include "LibraryBridge.h"
#include "LibraryModel.h"
#include "ProjectController.h"
#include "SearchBridge.h"
#include "search/SearchTestUtil.h"

#include <wabcut/ai/search/FootageSearch.h>

#include <catch2/catch_test_macros.hpp>

#include <QQuickItem>

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

    // The demo project's files, as fixtures: what each shot shows (a colour) and what is said.
    TempDir dir("ui");
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
