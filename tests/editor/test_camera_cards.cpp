// 0.2: the Media Browser's camera cards and spanned clips (editor/CameraCards.h), layered files and camera clips through
// their commands (media.layeredFileInfo / importLayeredFile / browse / importCameraClips over editor::mediaFormatServices),
// and the items of several files in Collect Files and relinking. Card structures are built here from the generated test
// clips (copied under the names cameras give them).

#include "../media/PsdWriter.h"

#include <wabcut/command/Engine.h>
#include <wabcut/editor/CameraCards.h>
#include <wabcut/editor/Capsule.h>
#include <wabcut/editor/MediaFormats.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/ImageSequence.h>
#include <wabcut/model/LayeredFile.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SpannedClip.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;

namespace {

fs::path testMedia(const char* name) { return fs::path(WABCUT_TEST_MEDIA_DIR) / name; }

bool haveChapters() {
    return fs::is_regular_file(testMedia("GX010042.MP4")) && fs::is_regular_file(testMedia("GX020042.MP4")) &&
           fs::is_regular_file(testMedia("avchd_00000.MTS")) && fs::is_regular_file(testMedia("sine_48k.wav"));
}

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void copyAs(const char* clip, const fs::path& to) {
    fs::create_directories(to.parent_path());
    fs::copy_file(testMedia(clip), to, fs::copy_options::overwrite_existing);
}

void writeText(const fs::path& to, const std::string& text) {
    fs::create_directories(to.parent_path());
    std::ofstream(to, std::ios::binary) << text;
}

/// An AVCHD playlist (MPLS) of play items: each clip name and its connection condition (1 = a new recording, 5 = the
/// camera continued the one before seamlessly).
std::string playlist(const std::vector<std::pair<std::string, int>>& items) {
    std::string out = "MPLS0200";
    const auto be32 = [&](std::uint32_t v) {
        for (int i = 3; i >= 0; --i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
    };
    const auto be16 = [&](std::uint16_t v) {
        out += static_cast<char>(v >> 8);
        out += static_cast<char>(v & 0xFF);
    };
    be32(40); // PlayList_start_address
    be32(0);
    be32(0);
    out.resize(40, '\0');
    be32(0); // the list's length (the reader walks the items)
    be16(0);
    be16(static_cast<std::uint16_t>(items.size()));
    be16(0);
    for (const auto& [name, condition] : items) {
        be16(20); // PlayItem length after this field: the name, codec, flags, STC id, IN and OUT times
        out += name;
        out += "M2TS";
        be16(static_cast<std::uint16_t>(condition & 0x0F));
        out += '\0'; // ref_to_STC_id
        be32(0);     // IN_time
        be32(0);     // OUT_time
    }
    return out;
}

std::vector<std::string> fileNames(const CameraClip& clip) {
    std::vector<std::string> out;
    for (const fs::path& f : clip.files) {
        out.push_back(pathToUtf8(f.filename()));
    }
    return out;
}

command::CommandResult run(EditSession& session, std::shared_ptr<command::CommandServices> services,
                           const std::string& name, command::Json params) {
    command::EnginePolicy policy;
    policy.services = std::move(services);
    command::CommandEngine engine(session, command::builtinCommands(), std::move(policy));
    command::CommandRequest request;
    request.command = name;
    request.params = std::move(params);
    return engine.execute(request);
}

} // namespace

TEST_CASE("Media Browser: GoPro chapters list as one clip and import as one spanned item", "[editor][camera-cards]") {
    if (!haveChapters()) {
        SKIP("test media not generated");
    }
    const fs::path dcim = workDir("gopro") / "DCIM" / "100GOPRO";
    copyAs("GX010042.MP4", dcim / "GX010042.MP4");
    copyAs("GX020042.MP4", dcim / "GX020042.MP4");
    copyAs("sine_48k.wav", dcim / "notes.wav");
    writeText(dcim / "GX010042.THM", "thumbnail");
    const std::vector<BrowserEntry> listing = browseFolder(dcim);
    REQUIRE(listing.size() == 2);
    CHECK(listing[0].name == "GX010042");
    REQUIRE(listing[0].clip.has_value());
    CHECK(fileNames(*listing[0].clip) == std::vector<std::string>{"GX010042.MP4", "GX020042.MP4"});
    CHECK(listing[1].name == "notes");
    // The second chapter belongs to the same clip.
    CHECK(cameraClipOf(dcim / "GX020042.MP4").files.size() == 2);

    auto items = describeCameraClip(cameraClipOf(dcim / "GX010042.MP4"), {});
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 1);
    const MediaItem& item = items->front();
    CHECK(item.name == "GX010042");
    CHECK(parseSpannedClipPath(item.locator.path).has_value());
    CHECK(item.info.duration.toSeconds() == Catch::Approx(2.0).margin(0.05));
    CHECK(item.locator.fingerprint.isKnown());
    CHECK_FALSE(checkMediaOnDisk(item, {}).first.locator.offline);
    fs::remove(dcim / "GX010042.MP4");
    CHECK(checkMediaOnDisk(item, {}).first.locator.offline);
}

TEST_CASE("Media Browser: AVCHD cards list the playlist's recordings, split ones joined", "[editor][camera-cards]") {
    if (!haveChapters()) {
        SKIP("test media not generated");
    }
    const fs::path card = workDir("avchd");
    const fs::path bdmv = card / "PRIVATE" / "AVCHD" / "BDMV";
    copyAs("avchd_00000.MTS", bdmv / "STREAM" / "00000.MTS");
    copyAs("avchd_00001.MTS", bdmv / "STREAM" / "00001.MTS");
    copyAs("avchd_00000.MTS", bdmv / "STREAM" / "00002.MTS");
    writeText(bdmv / "PLAYLIST" / "00000.MPL", playlist({{"00000", 1}, {"00001", 5}, {"00002", 1}}));
    fs::create_directories(bdmv / "CLIPINF");
    // The card's root and any folder inside it list the clips, not the folders.
    for (const fs::path& at : {card, bdmv / "STREAM"}) {
        INFO(pathToUtf8(at));
        const auto found = cameraCardAt(at);
        REQUIRE(found.has_value());
        CHECK(found->first == CameraCard::Avchd);
        const std::vector<BrowserEntry> listing = browseFolder(at);
        REQUIRE(listing.size() == 2);
        CHECK_FALSE(listing[0].folder);
        CHECK(listing[0].name == "00000");
        CHECK(fileNames(*listing[0].clip) == std::vector<std::string>{"00000.MTS", "00001.MTS"});
        CHECK(listing[1].name == "00002");
    }
    auto items = describeCameraClip(cameraClipOf(bdmv / "STREAM" / "00000.MTS"), {});
    REQUIRE(items.has_value());
    REQUIRE(items->size() == 1);
    CHECK(items->front().info.duration.toSeconds() == Catch::Approx(2.0).margin(0.1));
    // Frame-exact across the join: barcode 29 then 30.
    auto decoder = media::VideoDecoder::open(items->front().locator.path);
    REQUIRE(decoder.has_value());
    for (const int frame : {29, 30}) {
        // From the first picture (MPEG-TS audio may start before it, as in the camera's own file).
        auto f = (*decoder)->frameAt((*decoder)->firstFrameTime() + framesToTicks(frame, Rate{30, 1}) + Ticks{1000});
        REQUIRE(f.has_value());
        int index = 0;
        for (int bit = 0; bit < 16 && bit * 32 + 16 < f->width; ++bit) {
            if (f->cpu()->planes[0].data[static_cast<std::ptrdiff_t>(16) * f->cpu()->planes[0].stride + bit * 32 + 16] > 128) {
                index |= 1 << bit;
            }
        }
        CHECK(index == frame);
    }
}

TEST_CASE("Media Browser: Canon XF, P2, XDCAM EX and XAVC cards", "[editor][camera-cards]") {
    if (!haveChapters()) {
        SKIP("test media not generated");
    }
    const fs::path root = workDir("cards");
    // Canon XF: a clip folder's numbered MXF parts.
    const fs::path xf = root / "xf";
    copyAs("GX010042.MP4", xf / "CONTENTS" / "CLIPS001" / "AA0012" / "AA001201.MXF");
    copyAs("GX020042.MP4", xf / "CONTENTS" / "CLIPS001" / "AA0012" / "AA001202.MXF");
    writeText(xf / "CONTENTS" / "CLIPS001" / "AA0012" / "AA0012.XML", "<Clip/>");
    auto xfList = browseFolder(xf / "CONTENTS");
    REQUIRE(xfList.size() == 1);
    CHECK(xfList[0].name == "AA0012");
    CHECK(xfList[0].clip->card == CameraCard::CanonXf);
    CHECK(xfList[0].clip->files.size() == 2);
    // P2: the picture's MXF and one per audio channel, merged on import.
    const fs::path p2 = root / "p2";
    copyAs("GX010042.MP4", p2 / "CONTENTS" / "VIDEO" / "0001AB.MXF");
    copyAs("sine_48k.wav", p2 / "CONTENTS" / "AUDIO" / "0001AB00.MXF");
    copyAs("sine_48k.wav", p2 / "CONTENTS" / "AUDIO" / "0001AB01.MXF");
    writeText(p2 / "CONTENTS" / "CLIP" / "0001AB.XML", "<P2Main><ClipContent><ClipName>0001AB</ClipName></ClipContent></P2Main>");
    auto p2List = browseFolder(p2);
    REQUIRE(p2List.size() == 1);
    CHECK(p2List[0].clip->card == CameraCard::P2);
    CHECK(p2List[0].clip->audio.size() == 2);
    auto p2Items = describeCameraClip(*p2List[0].clip, {});
    REQUIRE(p2Items.has_value());
    CHECK(p2Items->size() == 3);
    CHECK((*p2Items)[1].name == "0001AB A1");
    // XDCAM EX and XAVC.
    copyAs("GX010042.MP4", root / "ex" / "BPAV" / "CLPR" / "801_0001_01" / "801_0001_01.MP4");
    auto ex = browseFolder(root / "ex");
    REQUIRE(ex.size() == 1);
    CHECK(ex[0].name == "801_0001_01");
    copyAs("GX010042.MP4", root / "xavc" / "PRIVATE" / "M4ROOT" / "CLIP" / "C0001.MP4");
    auto xavc = browseFolder(root / "xavc" / "PRIVATE");
    REQUIRE(xavc.size() == 1);
    CHECK(xavc[0].name == "C0001");
    CHECK(xavc[0].clip->card == CameraCard::Xavc);
    // A plain folder lists its subfolders, then its clips.
    auto plain = browseFolder(root);
    REQUIRE(plain.size() == 4); // ex, p2, xavc, xf
    CHECK(plain[0].folder);
    CHECK(plain[0].name == "ex");
}

TEST_CASE("Media commands: browse and import camera clips (spanned, P2 merged) in one undo step", "[editor][camera-cards]") {
    if (!haveChapters()) {
        SKIP("test media not generated");
    }
    const fs::path root = workDir("camera-commands");
    copyAs("GX010042.MP4", root / "GX010042.MP4");
    copyAs("GX020042.MP4", root / "GX020042.MP4");
    copyAs("GX010042.MP4", root / "p2" / "CONTENTS" / "VIDEO" / "0002CD.MXF");
    copyAs("sine_48k.wav", root / "p2" / "CONTENTS" / "AUDIO" / "0002CD00.MXF");
    fs::create_directories(root / "p2" / "CONTENTS" / "CLIP");
    EditSession session;
    const auto services = mediaFormatServices({});
    const command::CommandResult listed = run(session, services, "media.browse", {{"folder", pathToUtf8(root)}});
    REQUIRE(listed.ok());
    const auto& entries = listed.data["entries"];
    REQUIRE(entries.size() == 2); // the p2 folder, the GoPro clip
    CHECK(entries[1]["spanned"] == true);
    const command::CommandResult imported =
        run(session, services, "media.importCameraClips",
            {{"files", command::Json::array({pathToUtf8(root / "GX010042.MP4"),
                                             pathToUtf8(root / "p2" / "CONTENTS" / "VIDEO" / "0002CD.MXF")})}});
    INFO((imported.error ? imported.error->message : std::string()));
    REQUIRE(imported.ok());
    const ProjectState& state = *session.snapshot();
    CHECK(state.media.size() == 3); // the spanned item, the P2 picture and its audio
    CHECK(imported.data["imported"][1].contains("merged"));
    CHECK(state.sequences.size() == 1); // the merged clip
    REQUIRE(session.canUndo());
    session.undo();
    CHECK(session.snapshot()->media.empty());

    // Ingest (Copy): the chapters copied into <copyTo>/<clip>/ first, the item playing the copies.
    const command::CommandResult ingested =
        run(session, services, "media.importCameraClips",
            {{"files", command::Json::array({pathToUtf8(root / "GX010042.MP4")})}, {"copyTo", pathToUtf8(root / "ingest")}});
    INFO((ingested.error ? ingested.error->message : std::string()));
    REQUIRE(ingested.ok());
    CHECK(fs::is_regular_file(root / "ingest" / "GX010042" / "GX020042.MP4"));
    const MediaItem& copied = *session.snapshot()->media.front();
    const auto spec = parseSpannedClipPath(copied.locator.path);
    REQUIRE(spec.has_value());
    CHECK(spec->directory == root / "ingest" / "GX010042");
}

TEST_CASE("Layered files: Import Layered File as merged layers, individual layers and a sequence", "[editor][psd]") {
    const fs::path dir = workDir("layered");
    using media::test::TestPsdLayer;
    std::vector<TestPsdLayer> layers(3);
    layers[0].name = "Backdrop";
    layers[0].width = 64;
    layers[0].height = 32;
    layers[1].name = "Logo";
    layers[1].left = 40;
    layers[1].top = 4;
    layers[1].width = 16;
    layers[1].height = 8;
    layers[1].rgba = [](int, int) { return std::array<std::uint16_t, 4>{65535, 0, 0, 65535}; };
    layers[2].name = "Notes";
    layers[2].hidden = true;
    const fs::path psd = dir / "Lower third.psd";
    media::test::writeFile(psd, media::test::writePsd(64, 32, 8, layers));
    EditSession session;
    const auto services = mediaFormatServices({});
    const command::CommandResult info = run(session, services, "media.layeredFileInfo", {{"path", pathToUtf8(psd)}});
    REQUIRE(info.ok());
    CHECK(info.data["layers"].size() == 3);
    CHECK(info.data["layers"][2]["visible"] == false);
    CHECK(info.data["colorMode"] == "rgb");
    // CMYK and 32-bit documents are read too (converted to RGB; linear light).
    const fs::path cmyk = dir / "Print.psd";
    media::test::writeFile(cmyk, media::test::writePsd(64, 32, 8, layers, media::test::TestPsdCompression::Rle, false,
                                                       {32768, 32768, 32768, 65535}, media::test::TestPsdMode::Cmyk));
    const command::CommandResult cmykInfo = run(session, services, "media.layeredFileInfo", {{"path", pathToUtf8(cmyk)}});
    REQUIRE(cmykInfo.ok());
    CHECK(cmykInfo.data["colorMode"] == "cmyk");
    const fs::path hdr = dir / "Glow.psd";
    media::test::writeFile(hdr, media::test::writePsd(64, 32, 32, layers));
    const command::CommandResult hdrInfo = run(session, services, "media.layeredFileInfo", {{"path", pathToUtf8(hdr)}});
    REQUIRE(hdrInfo.ok());
    CHECK(hdrInfo.data["depth"] == 32);
    CHECK(hdrInfo.data["layers"].size() == 3);
    const command::CommandResult hdrImport = run(session, services, "media.importLayeredFile", {{"path", pathToUtf8(hdr)}});
    REQUIRE(hdrImport.ok());
    const MediaItem* glow = session.snapshot()->findMedia(MediaId{hdrImport.data["media"].get<std::uint64_t>()});
    REQUIRE(glow != nullptr);
    CHECK(glow->info.video.front().width == 64); // probed through the composite: the 32-bit layers decode

    const command::CommandResult merged = run(session, services, "media.importLayeredFile", {{"path", pathToUtf8(psd)}});
    REQUIRE(merged.ok());
    const MediaItem* all = session.snapshot()->findMedia(MediaId{merged.data["media"].get<std::uint64_t>()});
    REQUIRE(all != nullptr);
    CHECK(all->name == "Lower third.psd");
    CHECK(all->info.isStillImage);
    CHECK(all->info.video.front().width == 64);

    const command::CommandResult sequence =
        run(session, services, "media.importLayeredFile",
            {{"path", pathToUtf8(psd)}, {"as", "sequence"}, {"footage", "layer"}, {"duration", 2.0}});
    INFO((sequence.error ? sequence.error->message : std::string()));
    REQUIRE(sequence.ok());
    const ProjectState& state = *session.snapshot();
    REQUIRE(sequence.data["items"].size() == 2); // the hidden layer is left out
    const MediaItem* logo = state.findMedia(MediaId{sequence.data["items"][1].get<std::uint64_t>()});
    REQUIRE(logo != nullptr);
    CHECK(logo->name == "Logo/Lower third.psd");
    CHECK(logo->info.video.front().width == 16); // Layer Size
    CHECK(state.findBin(BinId{sequence.data["bin"].get<std::uint64_t>()})->name == "Lower third");
    const Sequence* seq = state.findSequence(SequenceId{sequence.data["sequence"].get<std::uint64_t>()});
    REQUIRE(seq != nullptr);
    CHECK(seq->width == 64);
    REQUIRE(seq->videoTracks.size() >= 2);
    REQUIRE(seq->videoTracks[1]->clips.size() == 1);
    const Clip& placed = seq->videoTracks[1]->clips.front();
    CHECK(placed.duration() == Ticks::fromSeconds(2));
    // Where the logo sits in the document: centre (48, 8) of a 64 x 32 canvas.
    CHECK(placed.properties()[ParamId::PositionX].constant() == Catch::Approx(16.0));
    CHECK(placed.properties()[ParamId::PositionY].constant() == Catch::Approx(-8.0));
    session.undo();
    CHECK(session.snapshot()->sequences.empty());
}

TEST_CASE("Items of several files: Collect Files copies them all, relinking keeps them whole", "[editor][camera-cards]") {
    if (!haveChapters()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("multi-file");
    copyAs("GX010042.MP4", dir / "card" / "GX010042.MP4");
    copyAs("GX020042.MP4", dir / "card" / "GX020042.MP4");
    auto items = describeCameraClip(cameraClipOf(dir / "card" / "GX010042.MP4"), dir / "p.wabcut");
    REQUIRE(items.has_value());
    EditSession session;
    ProjectDocument doc(session);
    auto added = edit::addMedia(*session.snapshot(), items->front());
    REQUIRE(added.has_value());
    REQUIRE(session.commit("Import", added->state));
    REQUIRE(doc.saveAs(dir / "p.wabcut"));
    const MediaId spanned = added->id;
    // Collect Files: both parts in a folder of their own; the copy opens with nothing offline.
    CapsuleOptions options;
    options.destination = dir / "out";
    options.name = "Job";
    options.wabcutVersion = "test";
    options.excludeUnused = false; // the item is in no sequence
    auto collected = collectProject(session.stateForSave(), dir / "p.wabcut", options);
    INFO((collected ? std::string() : collected.error().message));
    REQUIRE(collected.has_value());
    CHECK(collected->verified);
    CHECK(collected->offlineAfterReopen == 0);
    CHECK(fs::is_regular_file(collected->folder / "Media" / "GX010042" / "GX020042.MP4"));
    // Relinked to a copy of the card elsewhere: still one spanned clip.
    copyAs("GX010042.MP4", dir / "moved" / "GX010042.MP4");
    copyAs("GX020042.MP4", dir / "moved" / "GX020042.MP4");
    REQUIRE(doc.relink(spanned, dir / "moved" / "GX010042.MP4", false));
    const MediaItem* relinked = session.snapshot()->findMedia(spanned);
    REQUIRE(relinked != nullptr);
    const auto spec = parseSpannedClipPath(relinked->locator.path);
    REQUIRE(spec.has_value());
    CHECK(spec->directory == dir / "moved");
    CHECK(spec->parts.size() == 2);
}
