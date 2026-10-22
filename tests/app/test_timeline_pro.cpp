// M5i: the pro-grade timeline, offscreen.
//   * Video filmstrips: density per zoom and mode, placeholders while the atlas loads, the LRU byte budget, proxies
//     first.
//   * Sequence tabs: open / focus / close / reorder / cycle, each tab's playhead, zoom, scroll and targets, the
//     Library's double-click, Ctrl+Tab and a middle click in the shell.
//   * Track heights: resize, per kind and all, minimise, presets, the headers follow the lanes, the edge drag.
//   * Trim mode: Trim Edit's edit points, type cycling, keyboard trims, dynamic J/K/L trims, the Trim view's offsets,
//     edge clicks.
//   * Markers 2.0: colours, durations, moves, inline edits, navigation, clearing, filters, clip markers, export.
//   * Duplicate frame ranges and through edits.
// Screenshots: the timeline with filmstrips, the sequence tabs, the Trim view and the Markers panel.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "ClipAccessModel.h"
#include "DemoProject.h"
#include "FilmstripSource.h"
#include "KeymapDispatcher.h"
#include "MarkersBridge.h"
#include "NestedPictures.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "SequenceTabsBridge.h"
#include "TimelineAnalysis.h"
#include "TimelineFixture.h"
#include "TimelineView.h"
#include "TrimBridge.h"

#include <catch2/catch_test_macros.hpp>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequenceSettings.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QFile>
#include <QFileInfo>
#include <QJSValue>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTemporaryDir>
#include <QUrl>

#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// `count` frames of 128x72 (16:9) and their times, one second apart.
MediaFilmstripSource::Frames frames(int count, int hue = 200) {
    MediaFilmstripSource::Frames out;
    for (int i = 0; i < count; ++i) {
        QImage image(128, 72, QImage::Format_RGBA8888);
        image.fill(QColor::fromHsv((hue + i * 9) % 360, 150, 200));
        out.images.push_back(image);
        out.times.push_back(Ticks::fromSeconds(i));
    }
    return out;
}

/// Pending until `ready`, then one atlas for every media item.
class ControlledFilmstrips final : public FilmstripSource {
public:
    ControlledFilmstrips() {
        MediaFilmstripSource::Frames f = frames(16);
        m_atlas = buildFilmstripAtlas(f.images, f.times, 72);
        m_atlas->serial = 1;
    }
    Lookup lookup(MediaId) const override {
        return ready ? Lookup{State::Ready, m_atlas} : Lookup{State::Pending, nullptr};
    }
    std::atomic<bool> ready{false};

private:
    std::shared_ptr<FilmstripAtlas> m_atlas;
};

/// Video-like thumbnails for screenshots: per media item a sweep of a lit gradient and a moving disc.
class PictureFilmstrips final : public FilmstripSource {
public:
    Lookup lookup(MediaId media) const override {
        std::lock_guard lock(m_mutex);
        auto& atlas = m_atlases[media.value];
        if (!atlas) {
            std::vector<QImage> images;
            std::vector<Ticks> times;
            for (int i = 0; i < 24; ++i) {
                QImage image(128, 72, QImage::Format_RGBA8888_Premultiplied);
                QPainter p(&image);
                const int hue = static_cast<int>((media.value * 47) % 360);
                QLinearGradient g(0, 0, 128, 72);
                g.setColorAt(0, QColor::fromHsv(hue, 120, 70));
                g.setColorAt(1, QColor::fromHsv((hue + 40) % 360, 160, 190));
                p.fillRect(image.rect(), g);
                p.setRenderHint(QPainter::Antialiasing);
                p.setBrush(QColor::fromHsv((hue + 180) % 360, 90, 235));
                p.setPen(Qt::NoPen);
                p.drawEllipse(QPointF(20 + i * 4.0, 40 - (i % 6) * 3.0), 13, 13);
                p.fillRect(QRectF(0, 56, 128, 16), QColor(0, 0, 0, 90));
                p.end();
                images.push_back(image);
                times.push_back(Ticks::fromSeconds(i));
            }
            auto built = buildFilmstripAtlas(images, times, 72);
            built->serial = media.value;
            atlas = built;
        }
        return {State::Ready, atlas};
    }

private:
    mutable std::mutex m_mutex;
    mutable std::map<std::uint64_t, std::shared_ptr<const FilmstripAtlas>> m_atlases;
};

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

void findAllNamed(QQuickItem* root, const QString& name, QList<QQuickItem*>& out) {
    if (!root)
        return;
    if (root->objectName() == name)
        out << root;
    for (QQuickItem* child : root->childItems())
        findAllNamed(child, name, out);
}

void sendMouse(QQuickWindow* window, QEvent::Type type, QPointF scene, Qt::MouseButton button, Qt::MouseButtons buttons,
               Qt::KeyboardModifiers mods = {}) {
    QMouseEvent event(type, scene, scene, button, buttons, mods);
    QCoreApplication::sendEvent(window, &event);
}

void clickItem(QQuickWindow* window, QQuickItem* item, Qt::MouseButton button = Qt::LeftButton) {
    const QPointF c = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    sendMouse(window, QEvent::MouseButtonPress, c, button, button);
    sendMouse(window, QEvent::MouseButtonRelease, c, button, Qt::NoButton);
}

void sendKey(QQuickWindow* window, int key, Qt::KeyboardModifiers mods = {}) {
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods);
    QCoreApplication::sendEvent(window, &release);
}

void saveCrop(const QImage& image, QQuickItem* item, const QString& name, double extraBottom = 0) {
    const double dpr = image.devicePixelRatio();
    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height() + extraBottom));
    image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
        .save(QDir(outputDir()).filePath(name));
}

int transportKind(KeymapDispatcher::TransportKind kind) {
    return static_cast<int>(kind);
}

// Metadata-only fixture edit: tracks stay pointer-identical. No provider, file or paid generation is involved.
edit::EditOutcome setGeneratedFixture(const ProjectState& before, MediaId media,
                                     std::optional<GeneratedProvenance> provenance) {
    edit::EditResult result;
    result.state = before;
    for (MediaPtr& item : result.state.media) {
        if (item->id == media) {
            auto next = std::make_shared<MediaItem>(*item);
            next->generated = std::move(provenance);
            item = std::move(next);
            break;
        }
    }
    result.changes = diff(before, result.state);
    return result;
}

GeneratedProvenance extensionProvenance(MediaId source) {
    GeneratedProvenance provenance;
    provenance.provider = "synthetic-fixture";
    provenance.model = "offline-extension";
    provenance.promptHash = std::string(64, '0');
    provenance.createdAt = "2026-10-11T00:00:00Z";
    provenance.kind = "video";
    provenance.operation = "extend-tail";
    provenance.source = source;
    return provenance;
}

} // namespace

TEST_CASE("AI badges follow media provenance, viewport changes and undo", "[app][timeline][ai-badge]") {
    Rig rig("V1 |AaaaaaaaaaaaaaaaaaaaBbbbbbbbbbbbbbbbbbbb|", 10.0, false);
    const auto sequence = rig.controller.currentSequence();
    const MediaId generated = rig.tl.media('B');
    auto* access = rig.controller.clipList();
    const auto spoken = [&] {
        return access->data(access->index(1), ClipAccessModel::AccessibleNameRole).toString();
    };
    int accessibleUpdates = 0;
    QObject::connect(access, &QAbstractItemModel::dataChanged, access,
                     [&](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                         if (roles.contains(ClipAccessModel::AccessibleNameRole))
                             ++accessibleUpdates;
                     });
    CHECK(rig.view->stats().generatedBadges == 0);
    REQUIRE(rig.controller.applyEditFn("Synthetic extension provenance", [&](const ProjectState& state) {
        return setGeneratedFixture(state, generated, extensionProvenance(rig.tl.media('A')));
    }));
    REQUIRE(rig.controller.currentSequence() == sequence); // exercises metadata invalidation, not track edits
    rig.harness.frame();
    CHECK(rig.view->stats().generatedBadges == 1);
    CHECK(rig.view->stats().labels == 2);
    CHECK(spoken().contains(QStringLiteral("AI-generated")));
    CHECK(accessibleUpdates > 0);
    CHECK_FALSE(access->data(access->index(0), ClipAccessModel::AccessibleNameRole)
                    .toString().contains(QStringLiteral("AI-generated")));

    SECTION("Undo and redo metadata without replacing tracks") {
        rig.controller.undo();
        rig.harness.frame();
        CHECK(rig.view->stats().generatedBadges == 0);
        CHECK_FALSE(spoken().contains(QStringLiteral("AI-generated")));
        rig.controller.redo();
        rig.harness.frame();
        CHECK(rig.view->stats().generatedBadges == 1);
        REQUIRE(rig.controller.applyEditFn("Remove provenance", [&](const ProjectState& state) {
            return setGeneratedFixture(state, generated, std::nullopt);
        }));
        rig.harness.frame();
        CHECK(rig.view->stats().generatedBadges == 0);
        CHECK(rig.view->stats().labels == 2);
    }
    SECTION("Narrow clips keep labels and regain the badge when zoomed in") {
        rig.view->setPixelsPerSecond(50); // each clip 40 px; the label fits, the tag does not
        rig.harness.frame();
        CHECK(rig.view->stats().generatedBadges == 0);
        CHECK(rig.view->stats().labels == 2);
        CHECK(spoken().contains(QStringLiteral("AI-generated")));
        rig.view->setPixelsPerSecond(250);
        rig.harness.frame();
        CHECK(rig.view->stats().generatedBadges == 1);
    }
    SECTION("Sticky label scrolls, hides near the tail and returns") {
        rig.view->setScrollX(230);
        rig.harness.frame();
        CHECK_FALSE(rig.view->stats().fullRebuild); // move inside the existing built range
        CHECK(rig.view->stats().generatedBadges == 1);
        rig.view->setScrollX(360); // 40 px of the extension remain
        rig.harness.frame();
        CHECK(rig.view->stats().generatedBadges == 0);
        CHECK(rig.view->stats().labels >= 1);
        rig.view->setScrollX(1500); // leave the built range entirely
        rig.harness.frame();
        CHECK(rig.view->stats().generatedBadges == 0);
        rig.view->setScrollX(0);
        rig.harness.frame();
        CHECK(rig.view->stats().generatedBadges == 1);
    }
    SECTION("Deleting the extension removes the badge; undo restores it") {
        rig.controller.selectClipById(rig.clip("V1@20"), ProjectController::Replace, false);
        rig.controller.deleteSelection();
        rig.harness.frame();
        CHECK(rig.view->stats().generatedBadges == 0);
        rig.controller.undo();
        rig.harness.frame();
        CHECK(rig.view->stats().generatedBadges == 1);
    }
    SECTION("Presence, including empty provenance, marks generated media") {
        // Hand-built snapshot: the visual contract is presence, independent of provenance validation.
        auto result = setGeneratedFixture(*rig.controller.snapshot(), generated, GeneratedProvenance{});
        REQUIRE(result);
        rig.controller.resetProject(std::move(result->state), rig.tl.seq);
        rig.harness.frame();
        CHECK(rig.view->stats().generatedBadges == 1);
    }
    SECTION("A long name keeps its label and hides the tag") {
        rig.controller.renameClip(clipKey(rig.clip("V1@20")),
                                  QStringLiteral("A very long extension label that must be elided"));
        rig.harness.frame();
        CHECK(rig.view->stats().generatedBadges == 0);
        CHECK(rig.view->stats().labels == 2);
    }
}

TEST_CASE("AI badges follow provenance on audio clips too", "[app][timeline][ai-badge]") {
    Rig rig("A1 |Aaaaaaaaaaaaaaaaaaaa|", 10.0, false);
    REQUIRE(rig.controller.applyEditFn("Synthetic provenance", [&](const ProjectState& state) {
        return setGeneratedFixture(state, rig.tl.media('A'), extensionProvenance({}));
    }));
    rig.harness.frame();
    CHECK(rig.view->stats().generatedBadges == 1);
    CHECK(rig.view->stats().labels == 1);
}

TEST_CASE("AI badges respect deferred label layout without per-clip QML items", "[app][timeline][ai-badge]") {
    Rig rig("V1 |AaaaaAaaaaAaaaaAaaaaAaaaaAaaaaAaaaaAaaaaAaaaaAaaaaAaaaaAaaaa|", 15.0, false);
    REQUIRE(rig.controller.applyEditFn("Synthetic provenance", [&](const ProjectState& state) {
        return setGeneratedFixture(state, rig.tl.media('A'), extensionProvenance({}));
    }));
    for (int frame = 0; frame < 4; ++frame) {
        QCoreApplication::processEvents(); // deliver the budget's queued update
        rig.harness.frame();
    }
    CHECK_FALSE(rig.view->stats().labelsPending);
    CHECK(rig.view->stats().labels == 12);
    CHECK(rig.view->stats().generatedBadges == 12);
    CHECK(rig.view->childItems().isEmpty());
    REQUIRE(rig.controller.applyEditFn("Clear provenance", [&](const ProjectState& state) {
        return setGeneratedFixture(state, rig.tl.media('A'), std::nullopt);
    }));
    rig.harness.frame();
    CHECK(rig.view->stats().generatedBadges == 0);
    CHECK(rig.view->stats().labels == 12);
}

TEST_CASE("Screenshot: synthetic generated extension has an AI slice tag", "[app][timeline][ai-badge][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    bool dark = true;
    SECTION("Dark") { dark = true; }
    SECTION("Light") { dark = false; }
    shell.setAppearance(dark, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    REQUIRE(controller);
    REQUIRE(view);
    Timeline fixture("V1 |AaaaaaaaaaaaaaaaaaaaaaaaaBbbbbbbbbbbbbbbbbbbbbbbbb|");
    auto result = setGeneratedFixture(fixture.state, fixture.media('B'), extensionProvenance(fixture.media('A')));
    REQUIRE(result);
    controller->resetProject(std::move(result->state), fixture.seq);
    controller->renameClip(clipKey(fixture.clip("V1@0")), QStringLiteral("Original shot"));
    controller->renameClip(clipKey(fixture.clip("V1@25")), QStringLiteral("Tail extension"));
    view->setPixelsPerSecond(360);
    view->setScrollX(0);
    view->setTrackHeight(trackKey(fixture.track("V1")), 72);
    view->setFilmstripMode(QStringLiteral("continuous"));
    view->setFilmstripSource(std::make_shared<PictureFilmstrips>());
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    CHECK(view->stats().generatedBadges == 1);
    CHECK(view->stats().labels == 2);
    const QQuickItem* panel = shell.item("timelinePanel");
    REQUIRE(panel);
    const qreal dpr = image.devicePixelRatio();
    const QRectF rect = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
    const QImage crop = image.copy(QRectF(rect.x() * dpr, rect.y() * dpr, rect.width() * dpr,
                                         rect.height() * dpr).toRect());
    REQUIRE(crop.save(QDir(outputDir()).filePath(dark ? QStringLiteral("timeline-ai-extension-dark.png")
                                                    : QStringLiteral("timeline-ai-extension-light.png"))));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// =====================================================================================================================
// Filmstrips

TEST_CASE("Filmstrips: placeholders while the atlas loads, then thumbnails; density follows the zoom",
          "[app][timeline][filmstrip]") {
    Rig rig("V1 |Aaaaaaaaaaaaaaaaaaaa|", 10.0, false); // 20 frames: 200 px
    auto film = std::make_shared<ControlledFilmstrips>();
    rig.view->setFilmstripMode(QStringLiteral("continuous"));
    rig.view->setFilmstripSource(film);
    rig.harness.frame();
    // The body is 36 px high, so a 16:9 tile is 64 px wide: 197 px of body is a full strip of four tiles.
    CHECK(rig.view->stats().filmstripTiles == 0);
    CHECK(rig.view->stats().filmstripPlaceholders == 4);
    CHECK(rig.view->stats().filmstripNodes == 0);

    film->ready = true;
    rig.view->filmstripsChanged(); // what MediaFilmstripSource::stripsReady triggers
    rig.harness.frame();
    CHECK(rig.view->stats().filmstripTiles == 4);
    CHECK(rig.view->stats().filmstripPlaceholders == 0);
    CHECK(rig.view->stats().filmstripNodes == 1); // one textured node for the track's one media atlas
    CHECK(rig.view->stats().textures == 1);

    SECTION("Continuous adapts: the head frame when narrow, head and tail, then a full strip") {
        rig.view->setPixelsPerSecond(5 * 25.0); // 100 px: under two tiles
        rig.harness.frame();
        CHECK(rig.view->stats().filmstripTiles == 1);
        rig.view->setPixelsPerSecond(8 * 25.0); // 160 px: two to three tiles
        rig.harness.frame();
        CHECK(rig.view->stats().filmstripTiles == 2);
        rig.view->setPixelsPerSecond(20 * 25.0); // 400 px: a full strip
        rig.harness.frame();
        CHECK(rig.view->stats().filmstripTiles == 7);
    }
    SECTION("Video Head, Head and Tail, and off") {
        rig.view->setFilmstripMode(QStringLiteral("head"));
        rig.harness.frame();
        CHECK(rig.view->stats().filmstripTiles == 1);
        rig.view->setFilmstripMode(QStringLiteral("headTail"));
        rig.harness.frame();
        CHECK(rig.view->stats().filmstripTiles == 2);
        rig.view->setFilmstripMode(QStringLiteral("off"));
        rig.harness.frame();
        CHECK(rig.view->stats().filmstripTiles == 0);
        CHECK(rig.view->stats().filmstripNodes == 0);
        CHECK(rig.view->stats().textures == 0); // swept after the full rebuild
    }
}

TEST_CASE("Filmstrips: audio tracks and media without files draw no thumbnails", "[app][timeline][filmstrip]") {
    Rig rig(R"(
        V1 |Aaaaaaaaaaaaaaaaaaaa|
        A1 |Aaaaaaaaaaaaaaaaaaaa|
    )",
            10.0);
    auto film = std::make_shared<ControlledFilmstrips>();
    film->ready = true;
    rig.view->setFilmstripSource(film);
    rig.harness.frame();
    CHECK(rig.view->stats().filmstripTiles == 4); // V1 only
    rig.view->setFilmstripSource(std::make_shared<NoFilmstripSource>());
    rig.harness.frame();
    CHECK(rig.view->stats().filmstripTiles == 0);
    CHECK(rig.view->stats().filmstripPlaceholders == 0);
}

TEST_CASE("MediaFilmstripSource: asynchronous, one request per media, an LRU byte budget", "[app][filmstrip]") {
    std::vector<std::pair<std::string, MediaFilmstripSource::Done>> pending;
    MediaFilmstripSource source([&](const std::string& path, int count, int height, MediaFilmstripSource::Done done) {
        CHECK(count == MediaFilmstripSource::thumbnailCount(60));
        CHECK(height == MediaFilmstripSource::kThumbnailHeight);
        pending.emplace_back(path, std::move(done));
    });
    int readySignals = 0;
    QObject::connect(&source, &MediaFilmstripSource::stripsReady, [&] { ++readySignals; });
    source.setMediaFiles({{1, {"a.mov", 60}}, {2, {"b.mov", 60}}, {3, {"c.mov", 60}}});

    // A miss starts one request and answers Pending; asking again does not request again.
    CHECK(source.lookup(MediaId{1}).state == FilmstripSource::State::Pending);
    CHECK(source.lookup(MediaId{1}).state == FilmstripSource::State::Pending);
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].first == "a.mov");
    CHECK(source.lookup(MediaId{9}).state == FilmstripSource::State::Unavailable); // no file

    pending[0].second(frames(16)); // on the extraction thread, in the app
    QCoreApplication::processEvents();
    CHECK(readySignals == 1);
    const auto first = source.lookup(MediaId{1});
    REQUIRE(first.state == FilmstripSource::State::Ready);
    CHECK(first.atlas->count() == 16);
    CHECK(first.atlas->tileWidth == 128);
    CHECK(first.atlas->tileHeight == 72);
    CHECK(first.atlas->tileAt(Ticks::fromSeconds(3)) == 3);
    CHECK(first.atlas->tileAt(Ticks::fromSeconds(100)) == 15);
    const std::size_t one = source.bytesInMemory();
    CHECK(one == first.atlas->bytes());

    // Room for two and a half atlases: the third pushes out the least recently used.
    source.setBudget(one * 5 / 2);
    source.lookup(MediaId{2});
    pending.back().second(frames(16));
    source.lookup(MediaId{3});
    pending.back().second(frames(16));
    CHECK(source.residentMedia() == std::vector<MediaId>{MediaId{3}, MediaId{2}});
    CHECK(source.bytesInMemory() <= source.budget());
    // Using 2 makes 3 the oldest; the evicted 1 is requested again (from the disk cache) and evicts 3.
    CHECK(source.lookup(MediaId{2}).state == FilmstripSource::State::Ready);
    const std::size_t requests = pending.size();
    CHECK(source.lookup(MediaId{1}).state == FilmstripSource::State::Pending);
    CHECK(pending.size() == requests + 1);
    pending.back().second(frames(16));
    CHECK(source.residentMedia() == std::vector<MediaId>{MediaId{1}, MediaId{2}});

    // A failed extraction is Unavailable; a changed file drops the atlas and ignores the old request.
    source.setMediaFiles({{1, {"a.mov", 60}}, {2, {"b2.mov", 60}}});
    CHECK(source.residentMedia() == std::vector<MediaId>{MediaId{1}});
    CHECK(source.lookup(MediaId{2}).state == FilmstripSource::State::Pending);
    pending.back().second(std::nullopt);
    CHECK(source.lookup(MediaId{2}).state == FilmstripSource::State::Unavailable);
}

TEST_CASE("Nested and multicam clips get filmstrips of the nested sequence, rendered again after an edit inside (I.9)",
          "[app][filmstrip][nested]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb....|
    )");
    auto nested = edit::nestClips(tl.state, tl.seq, std::vector<ClipId>{tl.clip("V1@0"), tl.clip("V1@4")}, "Nest");
    REQUIRE(nested);
    MediaFilmstripSource source([](const std::string&, int, int, MediaFilmstripSource::Done) {});
    std::atomic<int> renders{0};
    std::mutex shownMutex;
    std::vector<std::size_t> shownTracks;
    source.setNestedRenderer([&](const ProjectState&, const Sequence& shown, int count, int height, const std::stop_token&) {
        CHECK(height == MediaFilmstripSource::kThumbnailHeight);
        {
            std::lock_guard lock(shownMutex);
            shownTracks.push_back(static_cast<std::size_t>(
                std::count_if(shown.videoTracks.begin(), shown.videoTracks.end(), [](const TrackPtr& t) { return t->enabled; })));
        }
        ++renders;
        return std::optional<MediaFilmstripSource::Frames>(frames(count, 30 * renders));
    });
    int ready = 0;
    QObject::connect(&source, &MediaFilmstripSource::stripsReady, [&] { ++ready; });
    const auto waitReady = [&](int count) {
        QElapsedTimer timer;
        timer.start();
        while (ready < count && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(2);
        }
        return ready >= count;
    };
    ProjectState s = nested->edit.state;
    source.setProject(std::make_shared<const ProjectState>(s), {});
    const auto clipOf = [&](const ProjectState& state) { return state.findSequence(tl.seq)->findClip(nested->clips.front())->clip(); };

    // A miss renders the nested sequence's pictures on the worker (it drew nothing before).
    const auto first = source.lookupNested(clipOf(s));
    CHECK(first.state == FilmstripSource::State::Pending);
    CHECK((first.key >> 63) == 1u); // never a media id
    REQUIRE(waitReady(1));
    const auto done = source.lookupNested(clipOf(s));
    REQUIRE(done.state == FilmstripSource::State::Ready);
    CHECK(done.atlas->count() == MediaFilmstripSource::thumbnailCount(s.findSequence(nested->nested)->duration.toSeconds()));
    CHECK(source.lookupNested(clipOf(s)).atlas == done.atlas);
    CHECK(renders == 1);

    // An edit inside the nested sequence (the clip itself unchanged): rendered again, the old strip shown meanwhile.
    const ClipId inside = clipAt(s, nested->nested, "V1@0");
    s = ok(edit::trimEdge(s, nested->nested, inside, edit::Edge::Tail, -f(1))).state;
    source.setProject(std::make_shared<const ProjectState>(s), {});
    CHECK(source.lookupNested(clipOf(s)).atlas == done.atlas);
    REQUIRE(waitReady(2));
    const auto again = source.lookupNested(clipOf(s));
    REQUIRE(again.state == FilmstripSource::State::Ready);
    CHECK(again.atlas != done.atlas);
    CHECK(renders == 2);
    // An unrelated edit at the top level renders nothing.
    s = ok(edit::addMarker(s, tl.seq, Marker{f(2), Ticks{}, "Note"})).state;
    source.setProject(std::make_shared<const ProjectState>(s), {});
    QCoreApplication::processEvents();
    CHECK(source.lookupNested(clipOf(s)).atlas == again.atlas);
    CHECK(renders == 2);
    // A media clip's strip is not a nested one.
    CHECK(source.lookupNested(Clip{}).state == FilmstripSource::State::Unavailable);
}

TEST_CASE("Library: a sequence's thumbnail URL follows what its picture depends on (I.9)", "[app][library][nested]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb....|
    )");
    auto nested = edit::nestClips(tl.state, tl.seq, std::vector<ClipId>{tl.clip("V1@4")}, "Nest");
    REQUIRE(nested);
    const ProjectState s = nested->edit.state;
    const QString url = SequenceThumbnailProvider::urlFor(s, tl.seq);
    CHECK(url.startsWith(QStringLiteral("image://wabcut-sequences/%1/").arg(tl.seq.value)));
    CHECK(SequenceThumbnailProvider::urlFor(s, SequenceId{9999}).isEmpty());
    // An edit inside the nested sequence changes the outer sequence's picture: a new URL.
    const ClipId inside = clipAt(s, nested->nested, "V1@0");
    const ProjectState trimmed = ok(edit::trimEdge(s, nested->nested, inside, edit::Edge::Tail, -f(1))).state;
    CHECK(SequenceThumbnailProvider::urlFor(trimmed, tl.seq) != url);
    // A redaction anywhere changes it too (it may cover what the picture shows); a marker does not.
    Redaction r;
    r.media = tl.media('A');
    r.source = {f(0), f(4)};
    r.region.shape = MaskShape::Rectangle;
    const auto redacted = edit::addRedaction(s, r);
    REQUIRE(redacted);
    CHECK(SequenceThumbnailProvider::urlFor(redacted->state, tl.seq) != url);
    const ProjectState marked = ok(edit::addMarker(s, tl.seq, Marker{f(2), Ticks{}, "Note"})).state;
    CHECK(sequencePictureStamp(marked, nested->nested) == sequencePictureStamp(s, nested->nested));
}

TEST_CASE("Filmstrips use a media item's proxy when it has one", "[app][filmstrip]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString original = dir.filePath(QStringLiteral("original.mov"));
    const QString proxy = dir.filePath(QStringLiteral("proxy.mov"));
    for (const QString& path : {original, proxy}) {
        QFile f(path);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    Timeline tl("V1 |Aaaa|");
    const MediaId a = tl.media('A');
    ProjectState state = tl.state;
    for (MediaPtr& m : state.media) {
        if (m->id != a)
            continue;
        MediaItem copy = *m;
        copy.locator.path = pathToUtf8(std::filesystem::path(original.toStdU16String()));
        copy.locator.offline = false;
        copy.locator.proxy = ProxyMedia{pathToUtf8(std::filesystem::path(proxy.toStdU16String())), {}};
        m = std::make_shared<const MediaItem>(std::move(copy));
    }
    ProjectController controller;
    controller.resetProject(state, tl.seq);
    REQUIRE(controller.mediaFilmstripSource());
    const auto canonical = [](const std::string& path) { return QFileInfo(QString::fromStdString(path)).canonicalFilePath(); };
    CHECK(canonical(controller.mediaFilmstripSource()->fileOf(a)) == QFileInfo(proxy).canonicalFilePath());

    QFile::remove(proxy); // the proxy is gone: the original
    controller.resetProject(state, tl.seq);
    CHECK(canonical(controller.mediaFilmstripSource()->fileOf(a)) == QFileInfo(original).canonicalFilePath());
}

// =====================================================================================================================
// Sequence tabs

TEST_CASE("Sequence tabs: open, focus, close, reorder and cycle; each tab keeps its playhead, zoom and scroll",
          "[app][timeline][tabs]") {
    ProjectController controller; // the demo: three sequences
    SequenceTabsBridge tabs;
    tabs.setController(&controller);
    const QVariantList all = tabs.sequences();
    REQUIRE(all.size() == 3);
    const QString s1 = all[0].toMap().value(QStringLiteral("id")).toString();
    const QString s2 = all[1].toMap().value(QStringLiteral("id")).toString();
    const QString s3 = all[2].toMap().value(QStringLiteral("id")).toString();
    CHECK(tabs.count() == 1);
    CHECK(tabs.current() == s1);
    CHECK(all[0].toMap().value(QStringLiteral("open")).toBool());
    CHECK_FALSE(all[1].toMap().value(QStringLiteral("open")).toBool());

    const QSize size(1000, 300);
    RenderHarness harness(size);
    REQUIRE(harness.initialize());
    TimelineView* view = addView(harness, controller, size);
    harness.frame();

    // Sequence 01: its playhead, zoom and scroll.
    controller.setPlayhead(Ticks::fromSeconds(7));
    const Ticks playhead1 = controller.playhead();
    view->setPixelsPerSecond(180);
    view->setScrollX(260);
    const double pps1 = view->pixelsPerSecond();
    const double scroll1 = view->scrollX();

    REQUIRE(tabs.open(s2));
    CHECK(tabs.count() == 2);
    CHECK(tabs.current() == s2);
    CHECK(controller.sequenceIdKey() == s2);
    controller.setPlayhead(Ticks::fromSeconds(3));
    const Ticks playhead2 = controller.playhead();
    view->setPixelsPerSecond(60);
    const double pps2 = view->pixelsPerSecond();

    REQUIRE(tabs.activate(s1));
    CHECK(controller.playhead() == playhead1);
    CHECK(view->pixelsPerSecond() == pps1);
    CHECK(view->scrollX() == scroll1);
    REQUIRE(tabs.activate(s2));
    CHECK(controller.playhead() == playhead2);
    CHECK(view->pixelsPerSecond() == pps2);

    // Targets are per sequence already (the model): toggling V1 in Sequence 01 leaves Sequence 02 alone.
    const bool targeted2 = controller.currentSequence()->videoTracks.front()->targeted;
    REQUIRE(tabs.activate(s1));
    const TrackId v1 = controller.currentSequence()->videoTracks.front()->id;
    const bool targeted1 = controller.currentSequence()->videoTracks.front()->targeted;
    controller.setTrackFlag(trackKey(v1), QStringLiteral("targeted"), !targeted1);
    REQUIRE(tabs.activate(s2));
    CHECK(controller.currentSequence()->videoTracks.front()->targeted == targeted2);
    REQUIRE(tabs.activate(s1));
    CHECK(controller.currentSequence()->videoTracks.front()->targeted == !targeted1);

    // A third tab opens after the current one; move and cycle.
    REQUIRE(tabs.open(s3));
    const auto order = [&] {
        QStringList ids;
        for (const QVariant& t : tabs.tabs())
            ids << t.toMap().value(QStringLiteral("id")).toString();
        return ids;
    };
    CHECK(order() == QStringList{s1, s3, s2});
    tabs.move(2, 0);
    CHECK(order() == QStringList{s2, s1, s3});
    tabs.cycle(1); // s3 -> wraps to s2
    CHECK(tabs.current() == s2);
    tabs.cycle(-1);
    CHECK(tabs.current() == s3);

    // Closing the current tab focuses its neighbour; the last tab stays.
    REQUIRE(tabs.close(s3));
    CHECK(order() == QStringList{s2, s1});
    CHECK(tabs.current() == s1);
    REQUIRE(tabs.close(s2));
    CHECK_FALSE(tabs.close(s1));
    CHECK(tabs.count() == 1);
    CHECK_FALSE(tabs.open(QStringLiteral("s999999")));

    // Switching sequences elsewhere (the sequence menu) opens a tab for it.
    controller.setCurrentSequenceIndex(2);
    CHECK(tabs.count() == 2);
    CHECK(tabs.current() == s3);
}

TEST_CASE("Sequence tabs in the shell: the Library opens them, Ctrl+Tab cycles, a middle click closes",
          "[app][timeline][tabs][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* tabs = shell.engine.singletonInstance<SequenceTabsBridge*>("Wabcut", "SequenceTabs");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(tabs);
    REQUIRE(controller);
    QQuickItem* root = shell.window->contentItem();
    REQUIRE_FALSE(shell.grab().isNull()); // lay the scene out
    const QVariantList all = tabs->sequences();
    REQUIRE(all.size() == 3);
    const QString s2 = all[1].toMap().value(QStringLiteral("id")).toString();
    const QString s3 = all[2].toMap().value(QStringLiteral("id")).toString();

    // Double-clicking a sequence in the Library (the chip's handler) opens or focuses its tab.
    QQuickItem* chip = findNamed(root, QStringLiteral("librarySequence_") + s2);
    REQUIRE(chip);
    REQUIRE(QMetaObject::invokeMethod(chip, "doubleClicked"));
    CHECK(tabs->current() == s2);
    CHECK(tabs->count() == 2);
    REQUIRE(tabs->open(s3));
    CHECK(tabs->count() == 3);

    // Ctrl+Tab in the Timeline: the next tab (wrapping).
    QQuickItem* lanes = findNamed(root, QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);
    lanes->forceActiveFocus();
    const QString before = tabs->current();
    sendKey(shell.window.get(), Qt::Key_Tab, Qt::ControlModifier);
    CHECK(tabs->current() != before);
    sendKey(shell.window.get(), Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier);
    CHECK(tabs->current() == before);

    // Clicking a tab focuses it; a middle click closes it.
    QCoreApplication::processEvents();
    QQuickItem* tab3 = findNamed(root, QStringLiteral("sequenceTab_") + s3);
    REQUIRE(tab3);
    clickItem(shell.window.get(), tab3);
    CHECK(tabs->current() == s3);
    clickItem(shell.window.get(), tab3, Qt::MiddleButton);
    CHECK(tabs->count() == 2);
    CHECK(tabs->current() != s3);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// =====================================================================================================================
// Track heights

TEST_CASE("Track heights: one track, per kind, all, minimise, presets", "[app][timeline][heights]") {
    Rig rig(R"(
        V2 |Bbbb|
        V1 |Aaaa|
        A1 |Aaaa|
    )");
    TimelineView* v = rig.view;
    const QString v1 = trackKey(rig.tl.track("V1"));
    const QString v2 = trackKey(rig.tl.track("V2"));
    const QString a1 = trackKey(rig.tl.track("A1"));
    const TimelineStyle& st = v->parsedStyle();
    CHECK(v->trackHeightOf(v1) == st.videoTrackHeight);
    CHECK(v->trackHeightOf(a1) == st.audioTrackHeight);

    const int revision = v->layoutRevision();
    v->setTrackHeight(v1, 100);
    CHECK(v->trackHeightOf(v1) == 100);
    CHECK(v->layoutRevision() > revision);
    // Rows stack: V2, V1, A1.
    CHECK(v->rows()[2].y == v->rows()[1].y + 100 + st.trackGap);
    v->setTrackHeight(v1, 3);
    CHECK(v->trackHeightOf(v1) == st.minTrackHeight);
    v->setTrackHeight(v1, 5000);
    CHECK(v->trackHeightOf(v1) == st.maxTrackHeight);

    v->resetTrackHeights();
    v->changeTrackHeights(QStringLiteral("video"), 2); // Ctrl+= twice
    CHECK(v->trackHeightOf(v1) == st.videoTrackHeight + 2 * st.trackHeightStep);
    CHECK(v->trackHeightOf(v2) == st.videoTrackHeight + 2 * st.trackHeightStep);
    CHECK(v->trackHeightOf(a1) == st.audioTrackHeight);
    v->changeTrackHeights(QStringLiteral("audio"), -1); // Alt+-
    CHECK(v->trackHeightOf(a1) == st.audioTrackHeight - st.trackHeightStep);
    v->changeTrackHeights(QStringLiteral("all"), 1); // Shift+=
    CHECK(v->trackHeightOf(a1) == st.audioTrackHeight);
    CHECK(v->trackHeightOf(v1) == st.videoTrackHeight + 3 * st.trackHeightStep);

    // Presets: the heights as lists per kind, recalled later.
    v->setTrackHeight(v2, 90);
    const QVariantMap preset = v->trackHeightPreset();
    CHECK(preset.value(QStringLiteral("video")).toList() ==
          QVariantList{st.videoTrackHeight + 3 * st.trackHeightStep, 90.0});
    v->minimizeAllTracks();
    CHECK(v->trackHeightOf(v1) == st.minTrackHeight);
    CHECK(v->trackHeightOf(a1) == st.minTrackHeight);
    v->applyTrackHeightPreset(preset);
    CHECK(v->trackHeightOf(v2) == 90);
    CHECK(v->trackHeightOf(a1) == st.audioTrackHeight);

    // Taller audio tracks draw taller waveforms (the envelope follows the row).
    rig.harness.frame();
    CHECK(v->rows()[2].height == st.audioTrackHeight);
}

TEST_CASE("Track heights in the shell: headers follow, the bottom edge drags, the keys work", "[app][timeline][heights][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    REQUIRE_FALSE(shell.grab().isNull());
    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    REQUIRE(view);
    QList<QQuickItem*> handles;
    findAllNamed(shell.window->contentItem(), QStringLiteral("trackResizeHandle"), handles);
    REQUIRE(handles.size() >= 2);
    QQuickItem* header = handles.front()->parentItem();
    const QString trackId = header->property("trackId").toString();
    REQUIRE_FALSE(trackId.isEmpty());
    const double before = view->trackHeightOf(trackId);
    CHECK(header->height() == before);

    // Drag the bottom edge down 30 px.
    const QPointF start = handles.front()->mapToScene(QPointF(handles.front()->width() / 2, handles.front()->height() / 2));
    sendMouse(shell.window.get(), QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 3; ++i)
        sendMouse(shell.window.get(), QEvent::MouseMove, start + QPointF(0, 10.0 * i), Qt::NoButton, Qt::LeftButton);
    sendMouse(shell.window.get(), QEvent::MouseButtonRelease, start + QPointF(0, 30), Qt::LeftButton, Qt::NoButton);
    CHECK(view->trackHeightOf(trackId) == before + 30);
    CHECK(header->height() == before + 30);

    // Keymap commands through Main.
    const double videoBefore = view->trackHeightOf(trackId);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand",
                                      Q_ARG(QVariant, QStringLiteral("timeline.increaseVideoTrackHeight"))));
    CHECK(view->trackHeightOf(trackId) == videoBefore + view->parsedStyle().trackHeightStep);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand",
                                      Q_ARG(QVariant, QStringLiteral("timeline.minimizeAllTracks"))));
    CHECK(view->trackHeightOf(trackId) == view->parsedStyle().minTrackHeight);
    CHECK(header->height() == view->parsedStyle().minTrackHeight);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// =====================================================================================================================
// View tools (0.1.1): Premiere's Hand (H) and Zoom (Z)

TEST_CASE("Hand and Zoom tools: H drags the view without editing, Z zooms in at the click and Alt+click zooms out",
          "[app][timeline][tools][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    REQUIRE_FALSE(shell.grab().isNull());
    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    QQuickItem* panel = shell.item("timelinePanel");
    QQuickItem* area = shell.item("timelineViewToolArea");
    QQuickItem* lanes = shell.item("timelineLanesArea");
    REQUIRE(view);
    REQUIRE(controller);
    REQUIRE(panel);
    REQUIRE(area);
    REQUIRE(lanes);
    CHECK_FALSE(area->property("enabled").toBool()); // the selection tool: presses go to the view

    // Z through the keymap (Main routes tool.zoom to the panel); the view keeps the selection tool.
    lanes->forceActiveFocus();
    sendKey(shell.window.get(), Qt::Key_Z);
    CHECK(panel->property("tool").toString() == QStringLiteral("zoom"));
    CHECK(view->tool() == QStringLiteral("select"));
    REQUIRE(area->property("enabled").toBool());
    // 0.1.3: the tools' cursors are EditCursors (Boxicons): a magnifier, an open hand, a holding hand.
    QQuickItem* toolCursor = shell.item("timelineViewToolCursor");
    REQUIRE(toolCursor);
    CHECK(toolCursor->property("shape").toString() == QStringLiteral("zoomIn"));
    CHECK(toolCursor->cursor().shape() == Qt::BitmapCursor);

    // A click zooms in, keeping the clicked time under the pointer; Alt+click zooms back out.
    const ProjectSnapshot before = controller->snapshot();
    const double x = area->width() * 0.6;
    const QPointF at = area->mapToScene(QPointF(x, area->height() / 2));
    const double level = view->zoomLevel();
    const double seconds = (x + view->scrollX()) / view->pixelsPerSecond();
    sendMouse(shell.window.get(), QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    sendMouse(shell.window.get(), QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    CHECK(view->zoomLevel() > level + 0.05);
    CHECK(std::abs((x + view->scrollX()) / view->pixelsPerSecond() - seconds) < 1e-6);
    const double zoomedIn = view->zoomLevel();
    sendMouse(shell.window.get(), QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
    sendMouse(shell.window.get(), QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
    CHECK(view->zoomLevel() < zoomedIn - 0.05);
    sendMouse(shell.window.get(), QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    sendMouse(shell.window.get(), QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    REQUIRE(view->zoomLevel() > level + 0.05); // zoomed in, so there is room to scroll

    // H: a drag across the clips scrolls the timeline with the pointer and edits nothing.
    sendKey(shell.window.get(), Qt::Key_H);
    CHECK(panel->property("tool").toString() == QStringLiteral("hand"));
    CHECK(view->tool() == QStringLiteral("select"));
    CHECK(toolCursor->property("shape").toString() == QStringLiteral("hand"));
    const double scroll = view->scrollX();
    REQUIRE(scroll > 40);
    sendMouse(shell.window.get(), QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    CHECK(toolCursor->property("shape").toString() == QStringLiteral("grab"));
    for (int i = 1; i <= 4; ++i)
        sendMouse(shell.window.get(), QEvent::MouseMove, at + QPointF(10.0 * i, 0), Qt::NoButton, Qt::LeftButton);
    sendMouse(shell.window.get(), QEvent::MouseButtonRelease, at + QPointF(40, 0), Qt::LeftButton, Qt::NoButton);
    CHECK(std::abs(view->scrollX() - (scroll - 40)) < 0.5);
    CHECK(controller->snapshot() == before); // no edit, no selection change in the project

    // V returns to the selection tool; the overlay steps aside.
    sendKey(shell.window.get(), Qt::Key_V);
    CHECK(panel->property("tool").toString() == QStringLiteral("select"));
    CHECK_FALSE(area->property("enabled").toBool());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// =====================================================================================================================
// File > Export Timeline (0.1.1)

TEST_CASE("File > Export Timeline: the menu opens the dialog, interchange.export writes the file, the report shows",
          "[app][interchange][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    REQUIRE_FALSE(shell.grab().isNull());
    auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(project);

    QObject* menuItem = shell.object("exportTimelineMenuItem");
    QObject* dialog = shell.object("exportTimelineDialog");
    QObject* reportDialog = shell.object("importReportDialog");
    REQUIRE(menuItem);
    REQUIRE(dialog);
    REQUIRE(reportDialog);
    CHECK(menuItem->property("enabled").toBool());
    REQUIRE(QMetaObject::invokeMethod(menuItem, "triggered"));
    QCoreApplication::processEvents();
    CHECK(dialog->property("visible").toBool());

    // Premiere / Resolve XML (the first format) to a file in a temporary folder.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("cut.xml"));
    dialog->setProperty("file", QUrl::fromLocalFile(path));
    QObject* ok = shell.object("exportTimelineOk");
    REQUIRE(ok);
    CHECK(ok->property("enabled").toBool());
    QVariantMap report;
    bool done = false;
    const QMetaObject::Connection connection = QObject::connect(
        project, &ProjectLifecycle::timelineExported, project, [&](const QVariantMap& r) {
            report = r;
            done = true;
        });
    REQUIRE(QMetaObject::invokeMethod(ok, "clicked"));
    CHECK(dialog->property("running").toBool()); // the command runs on a worker
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 20000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(5);
    }
    QObject::disconnect(connection);
    REQUIRE(done);
    INFO(report.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(report.value(QStringLiteral("ok")).toBool());
    CHECK(report.value(QStringLiteral("format")).toString() == QStringLiteral("Final Cut Pro 7 XML"));
    CHECK(report.value(QStringLiteral("carried")).toInt() > 0);
    CHECK(report.value(QStringLiteral("sequences")).toInt() >= 1);
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll().contains("<xmeml"));

    // The dialog closes and the export report shows in the import report's component.
    QCoreApplication::processEvents();
    CHECK_FALSE(dialog->property("opened").toBool()); // closing (its exit transition may still run)
    CHECK(reportDialog->property("visible").toBool());
    CHECK(reportDialog->property("title").toString() == QStringLiteral("Exported cut.xml"));
    CHECK(project->lastError().isEmpty());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("File > Export Timeline: several sequences, one file each and one combined report (0.2)",
          "[app][interchange][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    REQUIRE_FALSE(shell.grab().isNull());
    auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(project);
    REQUIRE(controller);
    controller->newSequence(); // a second sequence
    const QStringList names = controller->sequenceNames();
    REQUIRE(names.size() >= 2);

    QObject* dialog = shell.object("exportTimelineDialog");
    REQUIRE(dialog);
    REQUIRE(QMetaObject::invokeMethod(shell.object("exportTimelineMenuItem"), "triggered"));
    QCoreApplication::processEvents();
    CHECK_FALSE(dialog->property("several").toBool());
    REQUIRE(QMetaObject::invokeMethod(dialog, "selectAllSequences"));
    CHECK(dialog->property("several").toBool());
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    dialog->setProperty("folder", QUrl::fromLocalFile(dir.path()));
    QObject* ok = shell.object("exportTimelineOk");
    REQUIRE(ok);
    CHECK(ok->property("enabled").toBool());

    QVariantMap report;
    bool done = false;
    const auto wait = [&] {
        QElapsedTimer timer;
        timer.start();
        while (!done && timer.elapsed() < 30000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(5);
        }
        return done;
    };
    const QMetaObject::Connection connection = QObject::connect(
        project, &ProjectLifecycle::timelineExported, project, [&](const QVariantMap& r) {
            report = r;
            done = true;
        });
    REQUIRE(QMetaObject::invokeMethod(ok, "clicked"));
    REQUIRE(wait());
    INFO(report.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(report.value(QStringLiteral("ok")).toBool());
    const QVariantList files = report.value(QStringLiteral("files")).toList();
    REQUIRE(files.size() == names.size());
    for (int i = 0; i < files.size(); ++i) {
        const QVariantMap file = files[i].toMap();
        CHECK(file.value(QStringLiteral("sequence")).toString() == names[i]);
        CHECK(file.value(QStringLiteral("file")).toString() == names[i] + QStringLiteral(".xml"));
        QFile written(file.value(QStringLiteral("path")).toString());
        REQUIRE(written.open(QIODevice::ReadOnly));
        CHECK(written.readAll().contains("<xmeml"));
    }
    CHECK(report.value(QStringLiteral("sequences")).toInt() == names.size());
    CHECK(report.value(QStringLiteral("summary")).toString().startsWith(QStringLiteral("Final Cut Pro 7 XML")));
    QCoreApplication::processEvents();
    QObject* reportDialog = shell.object("importReportDialog");
    REQUIRE(reportDialog);
    CHECK(reportDialog->property("visible").toBool());
    // "Exported 2 timelines" (the plural is a translation: untranslated it reads "2 timeline(s)").
    CHECK(reportDialog->property("title").toString().startsWith(QStringLiteral("Exported %1 timeline").arg(names.size())));

    // Two sequences of one name get "Name.otio" and "Name (2).otio"; a missing folder is refused at once.
    for (int i = 0; i < 2; ++i) {
        const SequenceId id = controller->snapshot()->sequences[static_cast<std::size_t>(i)]->id;
        REQUIRE(controller->applyEditFn("Rename", [&](const ProjectState& s) {
            edit::SequenceSettingsChange rename;
            rename.name = "Cut";
            return edit::setSequenceSettings(s, id, rename);
        }));
    }
    CHECK_FALSE(project->exportTimelines(QStringLiteral("otio"), {0, 1}, QUrl::fromLocalFile(dir.filePath(QStringLiteral("nope")))));
    CHECK_FALSE(project->lastError().isEmpty());
    done = false;
    REQUIRE(project->exportTimelines(QStringLiteral("otio"), {0, 1}, QUrl::fromLocalFile(dir.path())));
    REQUIRE(wait());
    QObject::disconnect(connection);
    REQUIRE(report.value(QStringLiteral("ok")).toBool());
    const QVariantList named = report.value(QStringLiteral("files")).toList();
    REQUIRE(named.size() == 2);
    CHECK(named[0].toMap().value(QStringLiteral("file")).toString() == QStringLiteral("Cut.otio"));
    CHECK(named[1].toMap().value(QStringLiteral("file")).toString() == QStringLiteral("Cut (2).otio"));
    CHECK(QFileInfo::exists(dir.filePath(QStringLiteral("Cut (2).otio"))));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

namespace {
/// A QML object value as a map (JS objects come back as QJSValue).
QVariantMap jsMap(const QVariant& value) {
    if (value.metaType() == QMetaType::fromType<QJSValue>())
        return value.value<QJSValue>().toVariant().toMap();
    return value.toMap();
}
} // namespace

TEST_CASE("File > Export Timeline: AAF with the audio post settings; a caption file imports as a caption track (P.2)",
          "[app][interchange][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    REQUIRE_FALSE(shell.grab().isNull());
    auto* project = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    REQUIRE(project);
    QObject* dialog = shell.object("exportTimelineDialog");
    REQUIRE(dialog);
    REQUIRE(QMetaObject::invokeMethod(shell.object("exportTimelineMenuItem"), "triggered"));
    QCoreApplication::processEvents();

    // AAF (the fifth format): its settings show, OMF's title does not.
    QObject* formats = shell.object("exportTimelineFormat");
    QObject* options = shell.object("audioPostOptions");
    REQUIRE(formats);
    REQUIRE(options);
    CHECK_FALSE(options->property("visible").toBool());
    formats->setProperty("currentIndex", 4);
    QCoreApplication::processEvents();
    CHECK(jsMap(dialog->property("format")).value(QStringLiteral("id")).toString() == QStringLiteral("aaf"));
    CHECK(options->property("visible").toBool());
    CHECK_FALSE(options->property("omf").toBool());
    QVariant values;
    REQUIRE(QMetaObject::invokeMethod(options, "values", Q_RETURN_ARG(QVariant, values)));
    const QVariantMap v = jsMap(values);
    CHECK(v.value(QStringLiteral("sampleRate")).toInt() == 48000);
    CHECK(v.value(QStringLiteral("bitsPerSample")).toInt() == 24);
    CHECK(v.value(QStringLiteral("files")).toString() == QStringLiteral("embed"));
    CHECK(v.value(QStringLiteral("handleFrames")).toInt() == 24);
    CHECK(v.value(QStringLiteral("breakoutToMono")).toBool());
    CHECK_FALSE(v.contains(QStringLiteral("omfTitle")));

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("mix.aaf"));
    dialog->setProperty("file", QUrl::fromLocalFile(path));
    QVariantMap report;
    bool done = false;
    const QMetaObject::Connection connection = QObject::connect(
        project, &ProjectLifecycle::timelineExported, project, [&](const QVariantMap& r) {
            report = r;
            done = true;
        });
    REQUIRE(QMetaObject::invokeMethod(shell.object("exportTimelineOk"), "clicked"));
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < 60000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(5);
    }
    QObject::disconnect(connection);
    REQUIRE(done);
    INFO(report.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(report.value(QStringLiteral("ok")).toBool());
    CHECK(report.value(QStringLiteral("format")).toString() == QStringLiteral("AAF"));
    CHECK(report.value(QStringLiteral("summary")).toString().startsWith(QStringLiteral("AAF")));
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(file.read(8) == QByteArray("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8)); // a compound file

    // OMF: the title field and Include Pan.
    formats->setProperty("currentIndex", 5);
    QCoreApplication::processEvents();
    CHECK(options->property("omf").toBool());
    REQUIRE(QMetaObject::invokeMethod(options, "values", Q_RETURN_ARG(QVariant, values)));
    CHECK(jsMap(values).value(QStringLiteral("includePan")).toBool());
    CHECK_FALSE(jsMap(values).contains(QStringLiteral("breakoutToMono")));
    QMetaObject::invokeMethod(dialog, "close");

    // File > Import of an SRT: a new caption track of the current sequence, and its report.
    const QString srt = dir.filePath(QStringLiteral("Subtitles.srt"));
    QFile out(srt);
    REQUIRE(out.open(QIODevice::WriteOnly));
    out.write("1\n00:00:00,000 --> 00:00:01,000\nHello\n\n2\n00:00:01,500 --> 00:00:02,000\n<b>World</b>\n\n");
    out.close();
    QVariantMap imported;
    const QMetaObject::Connection importConnection =
        QObject::connect(project, &ProjectLifecycle::importReport, project, [&](const QVariantMap& r) { imported = r; });
    const QVariantMap summary = project->importFiles({QUrl::fromLocalFile(srt)});
    QObject::disconnect(importConnection);
    CHECK(summary.value(QStringLiteral("captionTracks")).toInt() == 1);
    CHECK(summary.value(QStringLiteral("failed")).toList().isEmpty());
    CHECK(imported.value(QStringLiteral("file")).toString() == QStringLiteral("Subtitles.srt"));
    CHECK(imported.value(QStringLiteral("approximated")).toInt() >= 1); // the bold
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    const SequencePtr current = controller->currentSequence();
    REQUIRE(current);
    REQUIRE_FALSE(current->captionTracks.empty());
    CHECK(current->captionTracks.back().name == "Subtitles");
    CHECK(current->captionTracks.back().cues.size() == 2);
    CHECK(project->detectImport(QUrl::fromLocalFile(srt)).value(QStringLiteral("kind")).toString() == QStringLiteral("captions"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// =====================================================================================================================
// Trim mode

TEST_CASE("Trim Edit selects the edit points nearest the playhead; the type cycles; keyboard trims",
          "[app][timeline][trim]") {
    Rig rig(R"(
        V1 |AaaaaaaaBbbbbbbb|
        A1 |AaaaaaaaBbbbbbbb|
    )");
    TrimBridge trim;
    trim.setController(&rig.controller);
    rig.controller.setPlayhead(f(6));
    REQUIRE(trim.trimEdit());
    CHECK(trim.active());
    CHECK(trim.viewOpen());
    CHECK(trim.type() == QStringLiteral("roll"));
    CHECK(trim.points().size() == 2); // V1 and A1: A's tail
    CHECK(trim.editTime() == f(8));
    CHECK(trim.outgoingName() == QStringLiteral("A"));
    CHECK(trim.incomingName() == QStringLiteral("B"));

    // Roll +2: the cut moves on both tracks, one undo step.
    REQUIRE(trim.trim(2));
    CHECK(rig.picture() == normalize(R"(
        V1 |AaaaaaaaaaB2bbbb|
        A1 |AaaaaaaaaaB2bbbb|
    )"));
    CHECK(rig.controller.undoText() == QStringLiteral("Undo Roll Edit"));
    CHECK(trim.outgoingOffset() == 2);
    CHECK(trim.incomingOffset() == 2);
    CHECK(trim.editTime() == f(10));

    // Toggle: ripple. Trim backward 1: A shortens and B follows.
    trim.toggleType();
    CHECK(trim.type() == QStringLiteral("ripple"));
    REQUIRE(trim.runCommand(QStringLiteral("sequence.trimBackward")));
    CHECK(rig.picture() == normalize(R"(
        V1 |AaaaaaaaaB2bbbb.|
        A1 |AaaaaaaaaB2bbbb.|
    )"));
    CHECK(trim.outgoingOffset() == 1);
    CHECK(trim.incomingOffset() == 2);

    // Toggle: regular. Trim backward 2 (Many, Large trim offset 2): a gap opens.
    trim.toggleType();
    CHECK(trim.type() == QStringLiteral("regular"));
    trim.setLargeOffset(2);
    REQUIRE(trim.runCommand(QStringLiteral("sequence.trimBackwardMany")));
    CHECK(rig.picture() == normalize(R"(
        V1 |Aaaaaaa..B2bbbb.|
        A1 |Aaaaaaa..B2bbbb.|
    )"));
    CHECK(trim.outgoingOffset() == -1);

    trim.toggleType();
    CHECK(trim.type() == QStringLiteral("roll"));
    // Each trim was one undo step.
    REQUIRE(rig.controller.undo());
    REQUIRE(rig.controller.undo());
    REQUIRE(rig.controller.undo());
    CHECK(rig.picture() == normalize(rig.original));

    trim.clear();
    CHECK_FALSE(trim.active());
    CHECK_FALSE(trim.viewOpen());
    CHECK(trim.runCommand(QStringLiteral("sequence.trimForward"))); // handled, nothing selected: nothing happens
    CHECK(rig.picture() == normalize(rig.original));
}

TEST_CASE("Dynamic trimming: J/K/L play from the edit point and the trim follows playback", "[app][timeline][trim]") {
    Rig rig(R"(
        V1 |AaaaaaaaBbbbbbbb|
    )");
    TrimBridge trim;
    trim.setController(&rig.controller);
    rig.controller.setPlayhead(f(8));
    REQUIRE(trim.trimEdit());

    // L: playback starts at the edit point (the program player is not attached here).
    trim.transportIntent(transportKind(KeymapDispatcher::TransportKind::ShuttleStep), 1, 1, 0);
    CHECK(trim.dynamic());
    CHECK(rig.controller.playhead() == f(8));
    rig.controller.setPlayhead(f(11)); // playback ran 3 frames
    // K: stop, trim by how far it played, the playhead on the new edit.
    trim.transportIntent(transportKind(KeymapDispatcher::TransportKind::Stop), 0, 1, 0);
    CHECK_FALSE(trim.dynamic());
    CHECK(rig.picture() == normalize("V1 |AaaaaaaaaaaB3bbb|"));
    CHECK(rig.controller.playhead() == f(11));
    CHECK(trim.outgoingOffset() == 3);

    // J backwards 2 frames.
    trim.transportIntent(transportKind(KeymapDispatcher::TransportKind::ShuttleStep), -1, 1, 0);
    rig.controller.setPlayhead(f(9));
    trim.transportIntent(transportKind(KeymapDispatcher::TransportKind::Stop), 0, 1, 0);
    CHECK(rig.picture() == normalize("V1 |AaaaaaaaaB1bbbbb|"));
    // K held + L tapped: one frame at a time.
    trim.transportIntent(transportKind(KeymapDispatcher::TransportKind::StepFrames), 0, 1, 1);
    CHECK(rig.picture() == normalize("V1 |AaaaaaaaaaB2bbbb|"));
    CHECK(trim.outgoingOffset() == 2);
    CHECK(trim.incomingOffset() == 2);
}

TEST_CASE("Clicking a clip edge selects that edit point; clicking a clip leaves trim mode", "[app][timeline][trim]") {
    Rig rig(R"(
        V1 |AaaaaaaaBbbbbbbb|
        A1 |AaaaaaaaBbbbbbbb|
    )");
    TrimBridge trim;
    trim.setController(&rig.controller);
    QObject::connect(rig.view, &TimelineView::editPointClicked, &trim, &TrimBridge::selectEditPoint);
    QObject::connect(&trim, &TrimBridge::pointsChanged, rig.view, [&] {
        rig.view->setTrimPoints(trim.points());
        rig.view->setTrimType(trim.type());
    });

    rig.view->setTool(QStringLiteral("ripple"));
    rig.press({rig.x(8) + 2, rig.y("V1")}); // B's head zone
    rig.release({rig.x(8) + 2, rig.y("V1")});
    REQUIRE(trim.active());
    CHECK(trim.type() == QStringLiteral("ripple"));
    CHECK(trim.points().size() == 2); // with its linked audio
    CHECK(trim.points()[0].toMap().value(QStringLiteral("edge")) == QStringLiteral("head"));
    CHECK_FALSE(trim.viewOpen()); // a click selects; Trim Edit opens the Trim view
    CHECK(rig.view->trimPoints().size() == 2);

    // Ripple the incoming head forward 2: B loses its first two frames and closes up.
    REQUIRE(trim.trim(2));
    CHECK(rig.picture() == normalize(R"(
        V1 |AaaaaaaaB2bbbb..|
        A1 |AaaaaaaaB2bbbb..|
    )"));
    CHECK(trim.incomingOffset() == 2);

    // A click on a clip body selects the clip and leaves trim mode.
    rig.view->setTool(QStringLiteral("select"));
    rig.press({rig.x(3), rig.y("V1")});
    rig.release({rig.x(3), rig.y("V1")});
    CHECK_FALSE(trim.active());
}

TEST_CASE("The Trim view opens with Trim Edit and shows the offsets", "[app][timeline][trim][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* trim = shell.engine.singletonInstance<TrimBridge*>("Wabcut", "Trim");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(trim);
    REQUIRE(controller);
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* view = findNamed(shell.window->contentItem(), QStringLiteral("trimView"));
    REQUIRE(view);
    CHECK_FALSE(view->isVisible());

    controller->setPlayhead(Ticks::fromSeconds(3));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("sequence.trimEdit"))));
    REQUIRE(trim->viewOpen());
    CHECK(view->isVisible());
    CHECK_FALSE(findNamed(shell.window->contentItem(), QStringLiteral("programFrameArea"))->isVisible());
    REQUIRE((trim->trim(3) || trim->trim(-3)));
    const QString expected = trim->outgoingOffset() > 0 ? QStringLiteral("+3") : QStringLiteral("-3");
    QQuickItem* out = findNamed(view, QStringLiteral("trimOutgoingOffset"));
    REQUIRE(out);
    bool found = false;
    for (QQuickItem* t : out->childItems())
        found = found || t->property("text").toString() == expected;
    CHECK(found);

    // Esc in the timeline leaves trim mode.
    QQuickItem* lanes = findNamed(shell.window->contentItem(), QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);
    lanes->forceActiveFocus();
    sendKey(shell.window.get(), Qt::Key_Escape);
    CHECK_FALSE(trim->active());
    CHECK_FALSE(view->isVisible());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// =====================================================================================================================
// Markers 2.0

TEST_CASE("Markers: add, colour, type, inline edit, duration, move, filter, navigate, clear", "[app][markers]") {
    Timeline tl("V1 |AaaaaaaaaaaaaaaaaaaaBbbb|");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    MarkersBridge markers;
    markers.setController(&controller);
    const auto seqMarkers = [&] { return controller.currentSequence()->markers; };

    controller.setPlayhead(f(5));
    CHECK(markers.add(false) == QStringLiteral("sequence"));
    REQUIRE(seqMarkers().size() == 1);
    CHECK(seqMarkers()[0].start == f(5));

    REQUIRE(markers.update(0, {{QStringLiteral("name"), QStringLiteral("Intro")},
                               {QStringLiteral("comment"), QStringLiteral("cold open")},
                               {QStringLiteral("colorIndex"), 1},
                               {QStringLiteral("type"), QStringLiteral("chapter")}}));
    CHECK(seqMarkers()[0].name == "Intro");
    CHECK(seqMarkers()[0].colorIndex == 1);
    CHECK(seqMarkers()[0].type == MarkerType::Chapter);
    CHECK(controller.undoText() == QStringLiteral("Undo Edit Marker"));

    // Alt-drag in the ruler: the end; a drag: the start (the duration is kept).
    REQUIRE(markers.setEnd(0, QString::number(f(9).value)));
    CHECK(seqMarkers()[0].duration == f(4));
    REQUIRE(markers.moveTo(0, QString::number(f(2).value)));
    CHECK(seqMarkers()[0].start == f(2));
    CHECK(seqMarkers()[0].duration == f(4));
    const QVariantMap item = markers.items()[0].toMap();
    CHECK(item.value(QStringLiteral("ranged")).toBool());
    CHECK(item.value(QStringLiteral("durationText")) == controller.formatTimecode(f(4)));

    // Timecode fields: In and duration typed in the sequence's timecode.
    REQUIRE(markers.update(0, {{QStringLiteral("duration"), controller.formatTimecode(f(6))}}));
    CHECK(seqMarkers()[0].duration == f(6));
    CHECK_FALSE(markers.update(0, {{QStringLiteral("start"), QStringLiteral("not a time")}}));

    // A web link must be http or https.
    CHECK_FALSE(markers.update(0, {{QStringLiteral("type"), QStringLiteral("webLink")},
                                   {QStringLiteral("url"), QStringLiteral("javascript:alert(1)")}}));
    CHECK_FALSE(markers.message().isEmpty());
    REQUIRE(markers.update(0, {{QStringLiteral("url"), QStringLiteral("https://wabcut.example")}}));

    controller.setPlayhead(f(12));
    markers.add(false);
    REQUIRE(markers.update(1, {{QStringLiteral("colorIndex"), 6}, {QStringLiteral("name"), QStringLiteral("Outro")}}));
    // Filters: colours and search.
    CHECK(markers.items().size() == 2);
    markers.setColorMask(1 << 6);
    CHECK(markers.items().size() == 1);
    CHECK(markers.items()[0].toMap().value(QStringLiteral("name")) == QStringLiteral("Outro"));
    markers.setColorMask(0xFF);
    markers.setSearch(QStringLiteral("COLD"));
    REQUIRE(markers.items().size() == 1);
    CHECK(markers.items()[0].toMap().value(QStringLiteral("index")) == 0);
    markers.setSearch({});
    CHECK(markers.marker(1).value(QStringLiteral("name")) == QStringLiteral("Outro"));

    // Navigation: Shift+M / Ctrl+Shift+M.
    controller.setPlayhead(f(0));
    REQUIRE(markers.runCommand(QStringLiteral("marker.goToNext")));
    CHECK(controller.playhead() == f(2));
    markers.goToNext();
    CHECK(controller.playhead() == f(12));
    CHECK_FALSE(markers.goToNext());
    markers.runCommand(QStringLiteral("marker.goToPrevious"));
    CHECK(controller.playhead() == f(2));

    // Ctrl+Alt+M clears the marker at the playhead; Ctrl+Alt+Shift+M clears them all.
    markers.runCommand(QStringLiteral("marker.clearSelected"));
    CHECK(seqMarkers().size() == 1);
    CHECK(seqMarkers()[0].name == "Outro");
    markers.runCommand(QStringLiteral("marker.clearAll"));
    CHECK(seqMarkers().empty());
    REQUIRE(controller.undo());
    CHECK(seqMarkers().size() == 1);
}

TEST_CASE("Clip markers: M on a selected clip marks its media, shown in the clip scope", "[app][markers]") {
    Timeline tl(R"(
        V1 |....Aaaaaaaa|
    )");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    MarkersBridge markers;
    markers.setController(&controller);
    const ClipId a = clipAt(*controller.snapshot(), tl.seq, "V1@4");
    controller.setSelection({a}, a);
    controller.setPlayhead(f(7)); // 3 frames into A: source frame 3
    CHECK(markers.add(true) == QStringLiteral("clip"));
    const MediaItem* media = controller.snapshot()->findMedia(tl.media('A'));
    REQUIRE(media);
    REQUIRE(media->markers.size() == 1);
    CHECK(media->markers[0].start == f(3));
    CHECK(controller.currentSequence()->markers.empty());

    markers.setScope(QStringLiteral("clip"));
    CHECK(markers.clipName() == QStringLiteral("A"));
    REQUIRE(markers.items().size() == 1);
    REQUIRE(markers.update(0, {{QStringLiteral("name"), QStringLiteral("Good take")}, {QStringLiteral("colorIndex"), 3}}));
    CHECK(controller.snapshot()->findMedia(tl.media('A'))->markers[0].name == "Good take");
    controller.setPlayhead(f(0));
    REQUIRE(markers.seek(0));
    CHECK(controller.playhead() == f(7)); // where this clip shows the marked source frame

    // Not focused on the Timeline (or no clip under the playhead): a sequence marker.
    markers.setScope(QStringLiteral("sequence"));
    CHECK(markers.add(false) == QStringLiteral("sequence"));
    CHECK(controller.currentSequence()->markers.size() == 1);
}

TEST_CASE("Markers export as CSV and YouTube chapters, to text and to a file", "[app][markers]") {
    Timeline tl("V1 |Aaaa|");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    MarkersBridge markers;
    markers.setController(&controller);
    controller.setPlayhead(f(0));
    markers.add(false);
    markers.update(0, {{QStringLiteral("name"), QStringLiteral("Intro")}, {QStringLiteral("type"), QStringLiteral("chapter")}});
    controller.setPlayhead(f(2));
    markers.add(false);
    markers.update(1, {{QStringLiteral("name"), QStringLiteral("Body, part 1")}});

    const QVariantMap csv = markers.exportText(QStringLiteral("csv"));
    CHECK(csv.value(QStringLiteral("count")).toInt() == 2);
    const QString text = csv.value(QStringLiteral("text")).toString();
    CHECK(text.startsWith(QStringLiteral("Marker Name,Description,In,Out,Duration,Marker Type,Color\n")));
    CHECK(text.contains(QStringLiteral("\"Body, part 1\"")));

    const QVariantMap youtube = markers.exportText(QStringLiteral("youtube"));
    CHECK(youtube.value(QStringLiteral("text")).toString() == QStringLiteral("0:00 Intro\n"));
    CHECK(youtube.value(QStringLiteral("warnings")).toStringList().contains(QStringLiteral("YouTube needs at least 3 chapters")));

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("markers.csv"));
    CHECK(markers.exportToFile(QStringLiteral("csv"), QUrl::fromLocalFile(path)) == path);
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(QString::fromUtf8(file.readAll()) == text);
}

// =====================================================================================================================
// Indicators

TEST_CASE("Duplicate frames: video that reuses source frames, one colour per source", "[app][timeline][duplicates]") {
    Timeline tl(R"(
        V2 |....Aaa....Cc|
        V1 |AaaaaaBbbbbbB|
    )");
    const Sequence& seq = tl.sequence();
    const ClipId v1a = clipAt(tl.state, tl.seq, "V1@0");
    const ClipId v2a = clipAt(tl.state, tl.seq, "V2@4");
    const ClipId b1 = clipAt(tl.state, tl.seq, "V1@6");
    const ClipId b2 = clipAt(tl.state, tl.seq, "V1@12");
    const auto dup = duplicateFrameRanges(seq);
    // A's source 0-3 is used by both A clips; B's frame 0 twice; C once.
    REQUIRE(dup.contains(v1a.value));
    REQUIRE(dup.contains(v2a.value));
    CHECK(dup.at(v1a.value).size() == 1);
    CHECK(dup.at(v1a.value)[0].range.start == f(0));
    CHECK(dup.at(v1a.value)[0].range.end() == f(3));
    CHECK(dup.at(v2a.value)[0].range.start == f(4));
    CHECK(dup.at(v2a.value)[0].range.end() == f(7));
    CHECK(dup.at(v1a.value)[0].colorIndex == dup.at(v2a.value)[0].colorIndex);
    REQUIRE(dup.contains(b1.value));
    REQUIRE(dup.contains(b2.value));
    CHECK(dup.at(b1.value)[0].range.end() == f(7));
    CHECK(dup.at(b1.value)[0].colorIndex != dup.at(v1a.value)[0].colorIndex);
    CHECK_FALSE(dup.contains(clipAt(tl.state, tl.seq, "V2@11").value));

    Timeline unique("V1 |AaaaBbbb|");
    CHECK(duplicateFrameRanges(unique.sequence()).empty());
}

TEST_CASE("Through edits: a cut that continues the same source", "[app][timeline][throughEdits]") {
    Timeline tl("V1 |AaaaA4aaA9aa|");
    const Track& v1 = *tl.sequence().videoTracks.front();
    REQUIRE(v1.clips.size() == 3);
    CHECK(isThroughEdit(v1.clips[0], v1.clips[1]));       // source 0-4 then 4-8
    CHECK_FALSE(isThroughEdit(v1.clips[1], v1.clips[2])); // source 4-8 then 9-13: a frame is skipped
    Timeline other("V1 |AaaaBbbb|");
    const Track& o = *other.sequence().videoTracks.front();
    CHECK_FALSE(isThroughEdit(o.clips[0], o.clips[1]));
}

// =====================================================================================================================
// Screenshots

TEST_CASE("Screenshot: the timeline with filmstrips, markers and a through edit", "[app][timeline][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* markers = shell.engine.singletonInstance<MarkersBridge*>("Wabcut", "Markers");
    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    QQuickItem* timeline = shell.item("timelinePanel");
    REQUIRE(controller);
    REQUIRE(markers);
    REQUIRE(view);
    REQUIRE(timeline);
    view->setFilmstripSource(std::make_shared<PictureFilmstrips>());
    view->setFilmstripMode(QStringLiteral("continuous"));
    // A through edit on V1 (Add Edit at 6 s) and markers in four colours, one a range (instead of the demo's).
    markers->clearAll();
    controller->setPlayhead(Ticks::fromSeconds(6));
    controller->razorAtPlayhead(true);
    const struct {
        int seconds;
        int color;
        const char* name;
        int length;
    } list[] = {{2, 0, "Intro", 0}, {9, 1, "Fix colour", 3}, {14, 3, "B-roll", 0}, {21, 6, "Outro", 0}};
    for (const auto& m : list) {
        controller->setPlayhead(Ticks::fromSeconds(m.seconds));
        markers->add(false);
        const int index = markers->totalCount() - 1;
        markers->update(index, {{QStringLiteral("name"), QString::fromLatin1(m.name)}, {QStringLiteral("colorIndex"), m.color}});
        if (m.length > 0)
            markers->setEnd(index, QString::number((Ticks::fromSeconds(m.seconds + m.length)).value));
    }
    controller->setPlayhead(Ticks::fromSeconds(11));
    view->setTrackHeight(trackKey(controller->currentSequence()->videoTracks.front()->id), 72);
    view->setTrackHeight(trackKey(controller->currentSequence()->videoTracks[1]->id), 60);
    REQUIRE_FALSE(shell.grab().isNull());
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    CHECK(view->stats().filmstripTiles > 0);
    saveCrop(image, timeline, QStringLiteral("timeline-filmstrips.png"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Screenshot: sequence tabs over the timeline", "[app][timeline][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* tabs = shell.engine.singletonInstance<SequenceTabsBridge*>("Wabcut", "SequenceTabs");
    QQuickItem* timeline = shell.item("timelinePanel");
    REQUIRE(tabs);
    REQUIRE(timeline);
    const QVariantList all = tabs->sequences();
    REQUIRE(all.size() == 3);
    tabs->open(all[2].toMap().value(QStringLiteral("id")).toString());
    tabs->open(all[1].toMap().value(QStringLiteral("id")).toString());
    tabs->activate(all[0].toMap().value(QStringLiteral("id")).toString());
    CHECK(tabs->count() == 3);
    REQUIRE_FALSE(shell.grab().isNull());
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    saveCrop(image, timeline, QStringLiteral("sequence-tabs.png"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Screenshot: the Trim view in the Program Monitor", "[app][timeline][trim][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* trim = shell.engine.singletonInstance<TrimBridge*>("Wabcut", "Trim");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(trim);
    REQUIRE(controller);
    controller->setPlayhead(Ticks::fromSeconds(23)); // near the cut from Interview A to Interview B
    REQUIRE(trim->trimEdit());
    CHECK((trim->trim(4) || trim->trim(-4)));
    trim->toggleType(); // ripple
    trim->trim(-1);
    spin(400); // the outgoing and incoming frames render off the GUI thread
    REQUIRE_FALSE(shell.grab().isNull());
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    QQuickItem* well = findNamed(shell.window->contentItem(), QStringLiteral("programMonitorWell"));
    REQUIRE(well);
    saveCrop(image, well->parentItem(), QStringLiteral("trim-view.png"));
    saveCrop(image, shell.item("timelinePanel"), QStringLiteral("trim-edit-points.png"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Screenshot: the Markers panel", "[app][markers][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* markers = shell.engine.singletonInstance<MarkersBridge*>("Wabcut", "Markers");
    REQUIRE(controller);
    REQUIRE(markers);
    markers->clearAll();
    const struct {
        int seconds;
        int color;
        const char* name;
        const char* comment;
        const char* type;
        int length;
    } list[] = {{0, 0, "Cold open", "Start on the skyline", "chapter", 0},
                {4, 1, "Fix colour", "Too warm after the cut", "comment", 2},
                {9, 3, "Interview", "Second take is better", "chapter", 0},
                {13, 6, "Sponsor", "https://wabcut.example/partner", "webLink", 0},
                {18, 4, "Ad break", "", "segmentation", 3},
                {24, 7, "Credits", "Music up", "chapter", 0}};
    for (const auto& m : list) {
        controller->setPlayhead(Ticks::fromSeconds(m.seconds));
        markers->add(false);
        const int index = markers->totalCount() - 1;
        QVariantMap fields{{QStringLiteral("name"), QString::fromLatin1(m.name)},
                           {QStringLiteral("colorIndex"), m.color},
                           {QStringLiteral("type"), QString::fromLatin1(m.type)}};
        if (QString::fromLatin1(m.type) == QLatin1String("webLink"))
            fields.insert(QStringLiteral("url"), QString::fromLatin1(m.comment));
        else
            fields.insert(QStringLiteral("comment"), QString::fromLatin1(m.comment));
        REQUIRE(markers->update(index, fields));
        if (m.length > 0)
            markers->setEnd(index, QString::number(Ticks::fromSeconds(m.seconds + m.length).value));
    }
    controller->setPlayhead(Ticks::fromSeconds(9));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QStringLiteral("markers"))));
    REQUIRE_FALSE(shell.grab().isNull());
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    QQuickItem* panel = findNamed(shell.window->contentItem(), QStringLiteral("markersPanel"));
    REQUIRE(panel);
    CHECK(markers->items().size() == 6);
    saveCrop(image, panel, QStringLiteral("markers-panel.png"));
    saveCrop(image, shell.item("timelinePanel"), QStringLiteral("markers-timeline.png"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Filmstrips hide the frames a source redaction covers (review P2-9)", "[app][timeline][filmstrip][review]") {
    Rig rig("V1 |Aaaaaaaaaaaaaaaaaaaa|", 10.0, false); // 20 frames: a full strip of four tiles
    auto film = std::make_shared<ControlledFilmstrips>();
    film->ready = true;
    rig.view->setFilmstripMode(QStringLiteral("continuous"));
    rig.view->setFilmstripSource(film);
    rig.harness.frame();
    REQUIRE(rig.view->stats().filmstripTiles == 4);

    // A redaction over the clip's media: its thumbnails would show what export blurs.
    Redaction plate;
    plate.media = rig.tl.media('A');
    plate.name = "Plate";
    plate.source = {Ticks{}, Ticks::fromSeconds(1)};
    const auto added = edit::addRedaction(*rig.controller.snapshot(), plate);
    REQUIRE(added);
    REQUIRE(rig.controller.session().commit("Redact", added->state));
    rig.harness.frame();
    CHECK(rig.view->stats().filmstripTiles == 0);
    CHECK(rig.view->stats().filmstripPlaceholders == 4);

    // Excepted for this clip ("don't redact here"): the thumbnails come back.
    const auto excepted =
        edit::setRedactionOverride(*rig.controller.snapshot(), added->id, rig.tl.clip("V1@0"), false);
    REQUIRE(excepted);
    REQUIRE(rig.controller.session().commit("Except", *excepted));
    rig.harness.frame();
    CHECK(rig.view->stats().filmstripTiles == 4);
}
