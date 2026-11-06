// 0.7 (card #86): SVG, Illustrator and EPS files import as stills (editor/MediaImport.h) and play as vector artwork drawn at the
// size it is shown: a graphic layer of one picture that the playback evaluator rasterises larger as the clip is scaled up
// (playback/Evaluate.h graphicOversample), so a logo stays sharp at 10x. Offscreen; no window.

#include <wabcut/editor/CameraCards.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/render/GraphicRaster.h>
#include <wabcut/render/VectorImage.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QGuiApplication>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::editor;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

void ensureGui() {
    if (!QGuiApplication::instance()) {
        static int argc = 1;
        static char name[] = "wabcut_editor_tests";
        static char* argv[] = {name, nullptr};
        static QGuiApplication app(argc, argv);
    }
}

struct Scratch {
    fs::path dir;
    Scratch() {
        ensureGui();
        dir = fs::temp_directory_path() / ("wabcut-vector-import-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
        render::VectorPictures::shared().clear();
    }
    ~Scratch() {
        render::setGhostscriptSearchPath(std::nullopt);
        render::VectorPictures::shared().clear();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path write(const std::string& name, const std::string& text) const {
        const fs::path file = dir / name;
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
        return file;
    }
};

/// A 32 x 16 logo with a red rectangle at x 8..24, y 4..12.
const std::string kLogo =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="16" viewBox="0 0 32 16"><rect x="8" y="4" width="16" height="8" fill="#ff0000"/></svg>)";

/// A project with the logo imported and one clip of it on V1 of a 1920 x 1080 sequence.
struct LogoProject {
    Scratch scratch;
    EditSession session{ProjectState{}};
    MediaId media;
    SequenceId sequence;
    ClipId clip;

    LogoProject() {
        const fs::path file = scratch.write("logo.svg", kLogo);
        const MediaImportResult r = importMediaFiles(session, std::vector<fs::path>{file});
        REQUIRE(r.failed.empty());
        REQUIRE(r.added.size() == 1);
        media = r.added.front();
        ProjectState state = *session.snapshot();
        edit::SequenceSettings settings;
        settings.name = "Logo";
        settings.width = 1920;
        settings.height = 1080;
        settings.frameRate = Rate{25, 1};
        settings.videoTracks = 1;
        settings.audioTracks = 0;
        const auto added = edit::addSequence(state, settings);
        REQUIRE(added);
        state = added->state;
        sequence = added->id;
        const Sequence& seq = *state.findSequence(sequence);
        const edit::SourceClip source{media,
                                      TimeRange{Ticks{}, Ticks::fromSeconds(2)},
                                      {edit::SourcePatch{StreamRef{StreamKind::Video, 0}, seq.videoTracks.front()->id, {}}}};
        const auto placed = edit::overwriteEdit(state, sequence, source, Ticks{});
        REQUIRE(placed);
        REQUIRE(placed->placed.size() == 1);
        clip = placed->placed.front();
        state = placed->state;
        scale(state, 1.0);
    }
    /// Sets the clip's Scale (a plain edit of the state; the project is evaluated from `state`).
    void scale(ProjectState& state, double factor) {
        edit::MotionChange change;
        change.scale = factor;
        const auto r = edit::setMotion(state, sequence, clip, Ticks{}, change);
        REQUIRE(r);
        state = r->state;
        current = state;
    }
    ProjectState current;
    std::vector<playback::LayerSpec> layers() const {
        std::vector<playback::LayerSpec> specs;
        playback::evaluateVideoLayers(current, *current.findSequence(sequence), Ticks::fromSeconds(0.5), specs, false);
        return specs;
    }
};

} // namespace

TEST_CASE("An SVG imports as a still of its nominal size, in one Import step (card 86)", "[editor][import][vector]") {
    Scratch scratch;
    const fs::path file = scratch.write("logo.svg", kLogo);
    CHECK(classifyImportFile(file) == ImportFileKind::Media); // artwork, not a timeline
    CHECK(isBrowsableMedia(file));                            // Import Folder and the Media Browser list it

    EditSession session{ProjectState{}};
    const MediaImportResult r = importMediaFiles(session, std::vector<fs::path>{file});
    REQUIRE(r.failed.empty());
    REQUIRE(r.added.size() == 1);
    CHECK(session.undoCount() == 1);
    CHECK(session.undoLabel() == "Import");
    const MediaItem* item = session.snapshot()->findMedia(r.added.front());
    REQUIRE(item != nullptr);
    CHECK(item->name == "logo.svg");
    CHECK(item->info.isStillImage);
    CHECK(item->info.container == "vector");
    REQUIRE(item->info.video.size() == 1);
    CHECK(item->info.video.front().width == 32);
    CHECK(item->info.video.front().height == 16);
    CHECK(item->info.video.front().codec == "svg");
    CHECK(item->info.audio.empty());
    CHECK(item->locator.path == pathToUtf8(fs::absolute(file).lexically_normal()));
    CHECK(item->locator.fingerprint.isKnown());
    // Importing it again finds the item that is there.
    const MediaImportResult again = importMediaFiles(session, std::vector<fs::path>{file});
    CHECK(again.added.empty());
    CHECK(again.alreadyInProject.size() == 1);
    // Saved with the project and read back: the file's path and a still's stream are all there is.
    const auto text = io::toJson(*session.snapshot());
    REQUIRE(text);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK(*loaded == *session.snapshot());
}

TEST_CASE("EPS and Illustrator files without Ghostscript are refused with what to install, nothing is added (card 86)",
          "[editor][import][vector]") {
    Scratch scratch;
    const fs::path empty = scratch.dir / "no-gs";
    fs::create_directories(empty);
    render::setGhostscriptSearchPath(pathToUtf8(empty));
    const std::string eps = "%!PS-Adobe-3.0 EPSF-3.0\n%%BoundingBox: 0 0 200 100\n%%EndComments\nshowpage\n";
    EditSession session{ProjectState{}};
    const MediaImportResult r = importMediaFiles(session, std::vector<fs::path>{scratch.write("art.eps", eps), scratch.write("old.ai", eps)});
    CHECK(r.added.empty());
    REQUIRE(r.failed.size() == 2);
    for (const auto& failed : r.failed) {
        INFO(pathToUtf8(failed.file));
        CHECK(failed.error.message.find("Ghostscript") != std::string::npos);
        CHECK(failed.error.message.find("https://ghostscript.com") != std::string::npos);
    }
    CHECK(session.undoCount() == 0);
    // An SVG beside them still imports.
    const MediaImportResult mixed = importMediaFiles(session, std::vector<fs::path>{scratch.write("art2.eps", eps), scratch.write("ok.svg", kLogo)});
    CHECK(mixed.added.size() == 1);
    CHECK(mixed.failed.size() == 1);
}

TEST_CASE("A vector still plays as a graphic of one picture drawn at the size shown (card 86)", "[editor][import][vector][playback]") {
    LogoProject p;
    const std::vector<playback::LayerSpec> specs = p.layers();
    REQUIRE(specs.size() == 1);
    const playback::LayerSpec& spec = specs.front();
    // No decode: the artwork is drawn by the graphic rasteriser, at the artwork's own size when it is not scaled up.
    REQUIRE(spec.graphic != nullptr);
    CHECK(spec.graphicSize.width == 32);
    CHECK(spec.graphicSize.height == 16);
    CHECK(spec.graphicOversample == 1.0);
    REQUIRE(spec.graphic->items.size() == 1);
    CHECK(spec.graphic->items.front().kind == DrawKind::Image);
    CHECK(spec.graphic->items.front().image.media == p.media);
}

TEST_CASE("A vector still scaled up is drawn larger, so it stays crisp - a bitmap would blur (card 86)", "[editor][import][vector][playback]") {
    LogoProject p;
    ProjectState state = p.current;
    p.scale(state, 10.0);
    const std::vector<playback::LayerSpec> specs = p.layers();
    REQUIRE(specs.size() == 1);
    const playback::LayerSpec& spec = specs.front();
    REQUIRE(spec.graphic != nullptr);
    // Ten times larger on the canvas: the raster is drawn that much larger (in half-octave steps, 11.3 x).
    CHECK(spec.graphicOversample > 10.0);
    CHECK(spec.graphicOversample < 12.0);
    const int width = static_cast<int>(std::lround(spec.graphicSize.width * spec.graphicOversample));
    const int height = static_cast<int>(std::lround(spec.graphicSize.height * spec.graphicOversample));
    const auto raster = render::rasterizeGraphic(*spec.graphic, width, height, spec.graphicImages.get());
    REQUIRE(raster != nullptr);
    CHECK(raster->width() == width);
    const auto alphaAt = [&](int x, int y) { return raster->rgba()[(static_cast<std::size_t>(y) * raster->width() + x) * 4 + 3]; };
    const double s = static_cast<double>(width) / 32.0;
    const int edge = static_cast<int>(8.0 * s); // the rectangle's left edge
    const int mid = static_cast<int>(8.0 * s);  // a row through it (y = 8)
    // Crisp: a few pixels either side of the edge the picture is fully one or the other. Upscaling the 32-pixel picture
    // would be a ramp of 11 pixels here.
    CHECK(alphaAt(edge - 4, mid) == 0);
    CHECK(alphaAt(edge + 4, mid) == 255);
    CHECK(raster->info().images == 1);
    CHECK(raster->info().missingImages == 0);
    // The layer keeps the clip's own scale; the composite layer (what preview and export draw) has the larger raster and a
    // transform shrunk to match, so the clip covers the same part of the canvas as before.
    CHECK(spec.transform.scale == 10.0);
    std::vector<media::VideoFrame> frames(specs.size());
    std::vector<render::CompositeLayer> composite;
    playback::buildCompositeLayers(specs, frames, composite);
    REQUIRE(composite.size() == 1);
    REQUIRE(composite[0].view.has_value());
    CHECK(composite[0].view->width == width);
    CHECK(composite[0].view->height == height);
    CHECK(composite[0].transform.scale == Approx(10.0 * 16.0 / height));
    const render::Affine2D drawn = render::layerToCanvas(composite[0].transform, width, height, 1.0, {1920, 1080});
    const render::Affine2D nominal = render::layerToCanvas(spec.transform, 32, 16, 1.0, {1920, 1080});
    CHECK(drawn.map({0.0, 0.0}).x == Approx(nominal.map({0.0, 0.0}).x).margin(1e-6));
    CHECK(drawn.map({static_cast<double>(width), static_cast<double>(height)}).y ==
          Approx(nominal.map({32.0, 16.0}).y).margin(1e-6));
}

TEST_CASE("A vector that cannot be read shows nothing, not a crash (card 86)", "[editor][import][vector][playback]") {
    LogoProject p;
    std::error_code ec;
    fs::remove(p.scratch.dir / "logo.svg", ec);
    const std::vector<playback::LayerSpec> specs = p.layers();
    CHECK(specs.empty());
}
