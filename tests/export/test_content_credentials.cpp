// AI-15: signed C2PA Content Credentials in a real MP4 export (docs/ai.md "Content Credentials"). ExportJob's finalize
// step signs the complete file with an identity made for the test (in memory only), and the same c2pa library verifies
// it: the manifest says Wabcut made it, lists the hashed source and the AI-generated one, the AI segment and the
// redaction (no prompt, no redaction name); a changed byte fails it; a signing failure leaves a complete unsigned file.
#include "ExportTestUtil.h"

#include <wabcut/ai/c2pa/ContentCredentials.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ReviewFrames.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>

using namespace exporttest;
using namespace wabcut;
using namespace wabcut::exporter;
namespace c2pa = wabcut::ai::c2pa;

namespace {

std::string readAll(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// 1 s at 30 fps: the camera clip, then 0.5 s of AI B-roll over it (V2), the camera's name redacted with a bleep.
TestProject credentialsProject() {
    TestProject p(640, 360, Rate{30, 1}, Ticks::fromSeconds(1));
    const MediaId camera = p.addMedia("h264_1080p60_aac.mp4");
    const MediaId broll = p.addMedia("prores_422.mov");
    p.editMedia(broll, [](MediaItem& m) {
        GeneratedProvenance g;
        g.provider = "fal";
        g.model = "veo3-fast";
        g.promptHash = std::string(64, 'c');
        g.createdAt = "2026-10-12T08:00:00Z";
        g.kind = "video";
        g.operation = "text-to-video";
        m.generated = g;
    });
    p.addVideoTrack(camera, Ticks{}, Ticks::fromSeconds(1), Ticks{});
    p.addVideoTrack(broll, ms(250), ms(500), Ticks{});
    Redaction r;
    r.media = camera;
    r.name = "Guest name plate";
    r.source = TimeRange{ms(100), ms(400)};
    r.audio = RedactionAudio::Bleep;
    r.region.shape = MaskShape::Rectangle;
    r.region.set("size.x", 0.2);
    r.region.set("size.y", 0.1);
    auto added = edit::addRedaction(p.state, r);
    REQUIRE(added);
    p.state = std::move(added->state);
    return p;
}

} // namespace

TEST_CASE("An MP4 export carries signed Content Credentials that verify and a changed byte fails them", "[export][media][c2pa]") {
    if (!c2pa::available()) {
        SKIP("this build has no c2pa library");
    }
    if (!haveMedia("h264_1080p60_aac.mp4") || !haveMedia("prores_422.mov")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject p = credentialsProject();
    REQUIRE(c2pa::containsGenerated(p.state, p.sequence, std::nullopt));
    auto identity = c2pa::generateIdentity();
    REQUIRE(identity);
    const auto dir = freshOutputDir("content-credentials");
    const auto out = dir / "signed.mp4";
    ExportRequest request = makeRequest(p, findPreset("youtube-1080p")->settings, out);
    c2pa::ManifestOptions options;
    options.title = "signed.mp4";
    options.format = "video/mp4";
    options.softwareVersion = "0.1.0";
    auto signedFlag = std::make_shared<std::atomic<bool>>(false);
    // A SigningIdentity holds its key as a move-only Secret: each signing gets a copy.
    request.finalize = c2pa::exportFinalizer(p.snapshot(), p.sequence, options, [&]() -> std::expected<c2pa::SigningIdentity, ai::AiError> {
        c2pa::SigningIdentity copy;
        copy.certificateChainPem = identity->certificateChainPem;
        copy.privateKeyPem = identity->privateKeyPem.clone();
        copy.rootPem = identity->rootPem;
        copy.algorithm = identity->algorithm;
        return copy;
    }, signedFlag);
    const auto result = ExportJob(request).run();
    INFO((result ? std::string() : result.error().message));
    REQUIRE(result);
    INFO("warnings: " << (result->warnings.empty() ? std::string() : result->warnings.front()));
    CHECK(result->warnings.empty());
    CHECK(signedFlag->load());
    REQUIRE(ffprobe(out)); // still a valid MP4 for every player
    CHECK(partialFiles(dir).empty());

    const auto checked = c2pa::verifyFile(out, identity->rootPem);
    REQUIRE(checked);
    INFO("codes: " << ai::Json(checked->codes).dump());
    CHECK(checked->hasManifest);
    CHECK(checked->valid);
    CHECK(checked->trusted);
    const std::string manifest = checked->manifest.dump();
    CHECK(manifest.find("Wabcut") != std::string::npos);
    CHECK(manifest.find("c2pa.created") != std::string::npos);
    CHECK(manifest.find("trainedAlgorithmicMedia") != std::string::npos);  // the AI B-roll
    CHECK(manifest.find("org.wabcut.ai-segments") != std::string::npos);
    CHECK(manifest.find("org.wabcut.redactions") != std::string::npos);
    CHECK(manifest.find("h264_1080p60_aac.mp4") != std::string::npos);   // an ingredient, hashed
    CHECK(manifest.find("sha256") != std::string::npos);
    CHECK(manifest.find("Guest name plate") == std::string::npos);        // the redaction's name never
    CHECK((manifest.find(std::string(64, 'c')) == std::string::npos ||
           manifest.find("promptSha256") != std::string::npos)); // the prompt only as its hash

    // Anyone else sees a valid file from an unknown signer (a local identity), not a trusted one.
    const auto stranger = c2pa::verifyFile(out);
    REQUIRE(stranger);
    CHECK(stranger->valid);
    CHECK_FALSE(stranger->trusted);

    // One changed byte in the media data fails the asset hash.
    std::string bytes = readAll(out);
    const std::size_t mdat = bytes.find("mdat");
    REQUIRE(mdat != std::string::npos);
    bytes[mdat + 64] = static_cast<char>(bytes[mdat + 64] ^ 0x5A);
    const auto tampered = dir / "tampered.mp4";
    std::ofstream(tampered, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto broken = c2pa::verifyFile(tampered, identity->rootPem);
    REQUIRE(broken);
    INFO("tampered: " << ai::Json(broken->failures).dump());
    CHECK(broken->hasManifest);
    CHECK_FALSE(broken->valid);
}

TEST_CASE("Premiere's Content Credentials settings: the producer, a thumbnail of the export and no AI training (0.2)",
          "[export][media][c2pa][gpu]") {
    if (!c2pa::available()) {
        SKIP("this build has no c2pa library");
    }
    if (!haveMedia("h264_1080p60_aac.mp4") || !haveMedia("prores_422.mov")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject p = credentialsProject();
    auto identity = c2pa::generateIdentity();
    REQUIRE(identity);
    const auto dir = freshOutputDir("content-credentials-settings");
    const auto out = dir / "produced.mp4";
    ExportRequest request = makeRequest(p, findPreset("youtube-1080p")->settings, out);
    c2pa::ManifestOptions options;
    options.title = "produced.mp4";
    options.format = "video/mp4";
    options.softwareVersion = "0.2.0";
    options.producer = "Wabcut Test Studio";
    options.doNotTrain = true;
    // The thumbnail the app and the CLI attach: the export's own picture, rendered by the exporter (redactions in).
    int thumbnails = 0;
    std::size_t thumbnailBytes = 0;
    options.thumbnail = [&, snapshot = p.snapshot(), sequence = p.sequence](const std::filesystem::path&) {
        ++thumbnails;
        RenderedReviewFrames frames;
        auto frame = frames.frame(*snapshot, sequence, ms(500), 320);
        const std::string bytes = frame && frame->mime == "image/jpeg" ? frame->bytes : std::string();
        thumbnailBytes = bytes.size();
        return bytes;
    };
    auto signedFlag = std::make_shared<std::atomic<bool>>(false);
    request.finalize = c2pa::exportFinalizer(p.snapshot(), p.sequence, options, [&]() -> std::expected<c2pa::SigningIdentity, ai::AiError> {
        c2pa::SigningIdentity copy;
        copy.certificateChainPem = identity->certificateChainPem;
        copy.privateKeyPem = identity->privateKeyPem.clone();
        copy.rootPem = identity->rootPem;
        copy.algorithm = identity->algorithm;
        return copy;
    }, signedFlag);
    const auto result = ExportJob(request).run();
    REQUIRE(result);
    INFO("warnings: " << (result->warnings.empty() ? std::string() : result->warnings.front()));
    CHECK(result->warnings.empty());
    CHECK(signedFlag->load());
    CHECK(thumbnails == 1);
    CHECK(thumbnailBytes > 1000);
    REQUIRE(ffprobe(out));

    // The c2pa library's reader validates the file and reads back what the settings put in.
    const auto checked = c2pa::verifyFile(out, identity->rootPem);
    REQUIRE(checked);
    INFO("codes: " << ai::Json(checked->codes).dump());
    CHECK(checked->hasManifest);
    CHECK(checked->valid);
    CHECK(checked->trusted);
    REQUIRE(checked->manifest.contains("thumbnail"));
    CHECK(checked->manifest["thumbnail"].value("format", std::string()) == "image/jpeg");
    const std::string manifest = checked->manifest.dump();
    CHECK(manifest.find("stds.schema-org.CreativeWork") != std::string::npos);
    CHECK(manifest.find("Wabcut Test Studio") != std::string::npos);
    CHECK(manifest.find("cawg.training-mining") != std::string::npos);
    CHECK(manifest.find("notAllowed") != std::string::npos);
    CHECK(manifest.find("trainedAlgorithmicMedia") != std::string::npos); // the AI B-roll is declared
}

TEST_CASE("A failure to sign never fails the export: a complete unsigned MP4 and a warning", "[export][media][c2pa]") {
    if (!haveMedia("h264_1080p60_aac.mp4") || !haveMedia("prores_422.mov")) {
        SKIP("test clips were not generated (tools/gen_test_media.py)");
    }
    const TestProject p = credentialsProject();
    const auto dir = freshOutputDir("content-credentials-unsigned");
    const auto out = dir / "unsigned.mp4";
    ExportRequest request = makeRequest(p, findPreset("youtube-1080p")->settings, out);
    c2pa::ManifestOptions options;
    options.format = "video/mp4";
    auto signedFlag = std::make_shared<std::atomic<bool>>(false);
    request.finalize = c2pa::exportFinalizer(p.snapshot(), p.sequence, options, [] {
        return std::expected<c2pa::SigningIdentity, ai::AiError>(std::unexpected(ai::AiError(ai::AiErrorCode::Unsupported, "no keychain here")));
    }, signedFlag);
    const auto result = ExportJob(request).run();
    REQUIRE(result);
    REQUIRE(result->warnings.size() == 1);
    CHECK(result->warnings[0].find("Content Credentials not signed") != std::string::npos);
    CHECK_FALSE(signedFlag->load());
    REQUIRE(ffprobe(out));
    if (c2pa::available()) {
        const auto checked = c2pa::verifyFile(out);
        REQUIRE(checked);
        CHECK_FALSE(checked->hasManifest);
    }
}
