// 0.2: media paths that name more than one file or part of a file - image sequences (ImageSequence.h), spanned clips
// (SpannedClip.h) and layered files (LayeredFile.h) - or carry a RAW clip's Source Settings (RawSourceSettings.h): what
// stands for them in file checks, and relinking them.

#include <wabcut/model/EditSession.h>
#include <wabcut/model/ImageSequence.h>
#include <wabcut/model/LayeredFile.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/RawSourceSettings.h>
#include <wabcut/model/SpannedClip.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;

TEST_CASE("Layered file paths: built, parsed and bounded", "[model][media-paths]") {
    LayeredFileSpec spec{pathFromUtf8("D:/Design/Title.psd"), {0, 3, 4}, false};
    const std::string path = layeredFilePath(spec);
    CHECK(path == "D:/Design/Title.psd?layers=0,3,4&size=layer");
    CHECK(parseLayeredFilePath(path) == spec);
    LayeredFileSpec all{pathFromUtf8("D:/Design/Title.PSB"), {}, true};
    CHECK(layeredFilePath(all) == "D:/Design/Title.PSB?layers=all&size=document");
    CHECK(parseLayeredFilePath(layeredFilePath(all)) == all);
    CHECK(layeredFilePath(LayeredFileSpec{pathFromUtf8("D:/a.png"), {}, true}).empty()); // not a PSD
    for (const std::string_view bad : {"D:/a.psd?layers=&size=document", "D:/a.psd?layers=1,&size=layer",
                                        "D:/a.psd?layers=-1&size=layer", "D:/a.psd?layers=1&size=huge",
                                        "D:/a.png?layers=1&size=layer", "D:/a.psd?layers=99999&size=layer"}) {
        INFO(bad);
        CHECK_FALSE(parseLayeredFilePath(bad).has_value());
    }
}

TEST_CASE("Media paths: the file that stands for them, and relinking keeps what they say", "[model][media-paths]") {
    // A layered file: its PSD; relinked to a moved PSD, the layers stay.
    const std::string layered = "D:/Design/Title.psd?layers=2&size=document";
    CHECK(mediaFileToCheck(pathFromUtf8(layered)) == pathFromUtf8("D:/Design/Title.psd"));
    CHECK(relinkedMediaPath(layered, pathFromUtf8("E:/Moved/Title v2.psd")) == "E:/Moved/Title v2.psd?layers=2&size=document");
    // A spanned clip: its first part; relinked, the other parts are looked for next to the new first part.
    const std::string spanned = "D:/DCIM/GX010042.MP4?span=GX010042.MP4:1000000|GX020042.MP4:2000000";
    CHECK(mediaFileToCheck(pathFromUtf8(spanned)) == pathFromUtf8("D:/DCIM/GX010042.MP4"));
    CHECK(relinkedMediaPath(spanned, pathFromUtf8("E:/Card/GX010042.MP4")) ==
          "E:/Card/GX010042.MP4?span=GX010042.MP4:1000000|GX020042.MP4:2000000");
    // An image sequence: the numbering and rate from the new first frame.
    ImageSequenceSpec seq;
    seq.directory = pathFromUtf8("D:/Renders");
    seq.prefix = "shot_";
    seq.suffix = ".png";
    seq.digits = 4;
    seq.first = 1;
    seq.frameRate = Rate{24, 1};
    const std::string sequence = imageSequencePath(seq);
    const std::string relinked = relinkedMediaPath(sequence, pathFromUtf8("E:/Final/take_0010.png"));
    const auto parsed = parseImageSequencePath(relinked);
    REQUIRE(parsed.has_value());
    CHECK(parsed->prefix == "take_");
    CHECK(parsed->first == 10);
    CHECK(parsed->frameRate == (Rate{24, 1}));
    CHECK(parsed->directory == pathFromUtf8("E:/Final"));
    // A plain file stays itself.
    CHECK(relinkedMediaPath("D:/a.mov", pathFromUtf8("E:/b.mov")) == "E:/b.mov");
    CHECK(mediaFileToCheck(pathFromUtf8("D:/a.mov")) == pathFromUtf8("D:/a.mov"));
}

TEST_CASE("RAW Source Settings: carried in the media path, bounded, read past by the other paths", "[model][media-paths][raw]") {
    RawSourceSettings settings;
    CHECK(settings.isDefault());
    CHECK(withRawSourceSettings("D:/A001.mov", settings) == "D:/A001.mov"); // the defaults leave the path bare
    settings.exposure = 1.254;
    CHECK(withRawSourceSettings("D:/A001.mov", settings) == "D:/A001.mov?raw=exposure:+1.25");
    settings.exposure = -0.5;
    settings.white = RawWhiteBalance{3200, -12};
    const std::string path = withRawSourceSettings("D:/A001.mov", settings);
    CHECK(path == "D:/A001.mov?raw=exposure:-0.50,white:3200:-12");
    CHECK(rawSourceSettingsOf(path) == settings);
    CHECK(withoutRawSourceSettings(path) == "D:/A001.mov");
    // Replacing them, and back to the defaults.
    settings.white.reset();
    settings.exposure = 2.0;
    CHECK(withRawSourceSettings(path, settings) == "D:/A001.mov?raw=exposure:+2.00");
    CHECK(withRawSourceSettings(path, RawSourceSettings{}) == "D:/A001.mov");
    // Out of bounds: no path; malformed or out-of-bounds suffixes are not settings.
    CHECK(withRawSourceSettings("D:/A001.mov", RawSourceSettings{6.0, std::nullopt}).empty());
    CHECK(withRawSourceSettings("D:/A001.mov", RawSourceSettings{0.0, RawWhiteBalance{1000, 0}}).empty());
    for (const std::string_view bad : {"D:/a.mov?raw=exposure:+9.00", "D:/a.mov?raw=exposure:1", "D:/a.mov?raw=exposure:+1.5",
                                        "D:/a.mov?raw=exposure:+1.00,white:3200", "D:/a.mov?raw=exposure:+1.00,white:900:0",
                                        "D:/a.mov?raw=exposure:+1.00,white:5600:101", "D:/a.mov?raw=exposure:+0.00",
                                        "D:/a.mov&raw=exposure:+1.00", "D:/raw=exposure:+1.00", "D:/a.mov?raw=exposure:+1.-5"}) {
        INFO(bad);
        CHECK(rawSourceSettingsOf(bad).isDefault());
        CHECK(withoutRawSourceSettings(bad) == bad);
    }
    // After an image sequence's own query (CinemaDNG): the sequence still parses, its first frame stands for it.
    ImageSequenceSpec seq;
    seq.directory = pathFromUtf8("D:/A001");
    seq.prefix = "A001_C001_";
    seq.suffix = ".dng";
    seq.digits = 3;
    seq.first = 1;
    seq.frameRate = Rate{24, 1};
    RawSourceSettings brighter{1.0, std::nullopt};
    const std::string dng = withRawSourceSettings(imageSequencePath(seq), brighter);
    CHECK(dng == imageSequencePath(seq) + "&raw=exposure:+1.00");
    const auto parsed = parseImageSequencePath(dng);
    REQUIRE(parsed.has_value());
    CHECK(parsed->first == 1);
    CHECK(mediaFileToCheck(pathFromUtf8(dng)) == pathFromUtf8("D:/A001/A001_C001_001.dng"));
    CHECK(rawSourceSettingsOf(dng) == brighter);
    // A plain RAW file: the file stands for it; relinked, the settings stay.
    CHECK(mediaFileToCheck(pathFromUtf8(path)) == pathFromUtf8("D:/A001.mov"));
    CHECK(relinkedMediaPath("D:/A001.mov?raw=exposure:+2.00", pathFromUtf8("E:/Card/A001.mov")) ==
          "E:/Card/A001.mov?raw=exposure:+2.00");
    const std::string relinked = relinkedMediaPath(dng, pathFromUtf8("E:/Card/A001_C001_005.dng"));
    CHECK(rawSourceSettingsOf(relinked) == brighter);
    REQUIRE(parseImageSequencePath(relinked).has_value());
    CHECK(parseImageSequencePath(relinked)->first == 5);
}

TEST_CASE("RAW Source Settings: one undo step on RAW items, refused on others, proxies dropped", "[model][raw]") {
    MediaItem raw;
    raw.name = "A001.mov";
    raw.locator.path = "D:/Shoot/A001.mov";
    raw.locator.relativePath = "Shoot/A001.mov";
    raw.locator.proxy = ProxyMedia{"D:/Proxies/A001.mp4", {}};
    raw.info.path = raw.locator.path;
    raw.info.duration = Ticks::fromSeconds(2);
    VideoStreamInfo v;
    v.codec = "prores_raw";
    v.pixelFormat = "bayer_rggb16le";
    v.width = 64;
    v.height = 32;
    raw.info.video.push_back(v);
    CHECK(isRawMedia(raw.info));
    MediaItem plain = raw;
    plain.name = "B001.mov";
    plain.locator.path = "D:/Shoot/B001.mov";
    plain.info.video.front().pixelFormat = "yuv422p10le";
    CHECK_FALSE(isRawMedia(plain.info));

    auto withRaw = edit::addMedia(ProjectState{}, raw);
    REQUIRE(withRaw.has_value());
    auto withPlain = edit::addMedia(withRaw->state, plain);
    REQUIRE(withPlain.has_value());
    EditSession session(withPlain->state);
    const MediaId id = withRaw->id;
    const RawSourceSettings settings{0.75, RawWhiteBalance{4300, 10}};
    const std::array<MediaId, 1> one{id};
    REQUIRE(session.apply("Source Settings", [&](const ProjectState& s) { return edit::setRawSourceSettings(s, one, settings); }));
    CHECK(session.undoCount() == 1);
    const MediaItem* item = session.snapshot()->findMedia(id);
    REQUIRE(item != nullptr);
    CHECK(item->locator.path == "D:/Shoot/A001.mov?raw=exposure:+0.75,white:4300:10");
    CHECK(item->locator.relativePath == "Shoot/A001.mov?raw=exposure:+0.75,white:4300:10");
    CHECK(rawSourceSettingsOf(item->locator.path) == settings);
    CHECK_FALSE(item->locator.proxy.has_value()); // made from the old development
    // Saved with the item: the project file keeps them in its media path (no key of their own).
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "wabcut_raw_source_settings.wabcut";
    REQUIRE(io::saveProject(*session.snapshot(), file, io::SaveOptions{0}));
    const auto loaded = io::loadProject(file);
    std::filesystem::remove(file);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->findMedia(id) != nullptr);
    CHECK(rawSourceSettingsOf(loaded->findMedia(id)->locator.path) == settings);
    // Back to As Shot: the bare path again.
    REQUIRE(session.apply("Source Settings", [&](const ProjectState& s) {
        return edit::setRawSourceSettings(s, one, RawSourceSettings{});
    }));
    CHECK(session.snapshot()->findMedia(id)->locator.path == "D:/Shoot/A001.mov");
    session.undo();
    session.undo();
    CHECK(session.snapshot()->findMedia(id)->locator.path == "D:/Shoot/A001.mov");
    CHECK(session.snapshot()->findMedia(id)->locator.proxy.has_value());
    // Not RAW, missing, out of bounds: refused.
    const std::array<MediaId, 1> notRaw{withPlain->id};
    CHECK(edit::setRawSourceSettings(*session.snapshot(), notRaw, settings).error().code == ErrorCode::InvalidArgument);
    const std::array<MediaId, 1> missing{MediaId{999}};
    CHECK(edit::setRawSourceSettings(*session.snapshot(), missing, settings).error().code == ErrorCode::NotFound);
    CHECK(edit::setRawSourceSettings(*session.snapshot(), one, RawSourceSettings{0.0, RawWhiteBalance{20000, 0}})
              .error()
              .code == ErrorCode::InvalidArgument);
}
