// AI video effects (0.7): the model catalogue (ModelStore entries of kind Video: pinned revisions, digests, allowed
// licences), the steps a stage runs, the cache version of a chain, the download card and the estimates.

#include "VideoTestUtil.h"

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/video/VideoModels.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <regex>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::video;
using namespace wabcut::ai::video::test;

namespace {

AiStage restore(int scale, int quality, int deblur, int denoise, int grain = 0, int tile = 0) {
    AiStage s;
    s.effect = std::string(kAiRestoreEffect);
    s.scale = scale;
    s.quality = quality;
    s.deblur = deblur;
    s.denoise = denoise;
    s.grain = grain;
    s.tile = tile;
    return s;
}

AiStage denoiseStage(int strength, int grain = 0) {
    AiStage s;
    s.effect = std::string(kAiDenoiseEffect);
    s.denoise = strength;
    s.grain = grain;
    return s;
}

} // namespace

TEST_CASE("Every model of the AI video effects is a verified, allowed, pinned download", "[ai][video][models]") {
    const std::regex hash64("^[0-9a-f]{64}$");
    const std::regex pinned("^https://huggingface\\.co/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/resolve/[0-9a-f]{40}/[A-Za-z0-9_./-]+\\.onnx$");
    int found = 0;
    for (const Model model : {Model::Denoise, Model::Deblur, Model::UpscaleFast4x, Model::Upscale2x, Model::Upscale4x}) {
        const ModelDescription& d = describe(model);
        const ModelInfo* info = findModel(d.storeId);
        INFO(d.key);
        REQUIRE(info != nullptr);
        ++found;
        CHECK(info->kind == ModelKind::Video);
        CHECK(info->id == d.storeId);
        CHECK(info->size > 1'000'000);
        CHECK(std::regex_match(info->sha256, hash64));
        CHECK(std::regex_match(info->url, pinned));
        CHECK(isAllowedModelLicense(info->license)); // never a non-commercial or community licence
        CHECK_FALSE(requiresLicenseOptIn(*info));
        CHECK(info->fileName.starts_with("ai-video/"));
        CHECK(info->licenseCard.find("License") != std::string::npos);
        CHECK(info->licenseCard.find("https://") != std::string::npos); // the card names where the licence is
        CHECK(info->description.find("MB") != std::string::npos);
    }
    CHECK(found == 5);
    CHECK(describe(Model::Denoise).multiple == 8);
    CHECK(describe(Model::Deblur).multiple == 16);
    CHECK(describe(Model::UpscaleFast4x).scale == 4);
    CHECK(describe(Model::Upscale2x).scale == 2);
}

TEST_CASE("The deblurrer never gets a window below the 384 pixels its graph needs", "[ai][video][models]") {
    // NAFNet's channel attention pools over 384 pixel windows: a smaller input fails on the CPU and is wrong on DirectML (found
    // with the real model), so its tiles are large whatever Tile Size says.
    CHECK(describe(Model::Deblur).minWindow == 384);
    CHECK(tileFor(Model::Deblur, 64) == 352);
    CHECK(tileFor(Model::Deblur, 256) == 352);
    CHECK(tileFor(Model::Deblur, 512) == 512);
    CHECK(tileFor(Model::Denoise, 128) == 128);
    CHECK(tileFor(Model::Denoise, 130) == 136); // a multiple of 8
    CHECK(tileFor(Model::UpscaleFast4x, 64) == 64);
    for (const int tile : {0, 128, 256, 512}) {
        const auto steps = stepsOf(restore(1, 0, 50, 0, 0, tile));
        REQUIRE(steps.size() == 1);
        CHECK(steps[0].model == Model::Deblur);
        CHECK(steps[0].tile + 2 * kTilePad >= 384);
    }
    const auto upscale = stepsOf(restore(2, 0, 0, 0, 0, 128));
    REQUIRE(upscale.size() == 1);
    CHECK(upscale[0].tile == 128); // the other models keep the size asked for
}

TEST_CASE("A Restore stage runs denoise, deblur, then the upscaler; a Denoise stage only denoises", "[ai][video][models]") {
    const auto steps = stepsOf(restore(2, 1, 50, 40, 20, 128));
    REQUIRE(steps.size() == 3);
    CHECK(steps[0].model == Model::Denoise);
    CHECK(steps[0].sigma > 0.0);
    CHECK(steps[0].sigma < 25.0 / 255.0);
    CHECK(steps[0].grain == 0.2);
    CHECK(steps[1].model == Model::Deblur);
    CHECK(steps[1].strength == 0.5);
    CHECK(steps[2].model == Model::Upscale2x);
    CHECK(steps[2].outputScale() == 2);
    CHECK(steps[2].tile == 128);

    const auto fourX = stepsOf(restore(4, 1, 0, 0));
    REQUIRE(fourX.size() == 1);
    CHECK(fourX[0].model == Model::Upscale4x);

    const auto fast2 = stepsOf(restore(2, 0, 0, 0));
    REQUIRE(fast2.size() == 1);
    CHECK(fast2[0].model == Model::UpscaleFast4x);
    CHECK(fast2[0].resizeDivisor == 2); // the 4x result is halved
    CHECK(fast2[0].outputScale() == 2);
    const auto fast4 = stepsOf(restore(4, 0, 0, 0));
    CHECK(fast4[0].resizeDivisor == 1);
    CHECK(fast4[0].outputScale() == 4);

    const auto denoise = stepsOf(denoiseStage(100, 15));
    REQUIRE(denoise.size() == 1);
    CHECK(denoise[0].model == Model::Denoise);
    CHECK(denoise[0].sigma == Catch::Approx(40.0 / 255.0));
    CHECK(denoise[0].grain == 0.15);
    CHECK(denoise[0].tile == 256); // Auto

    CHECK(stepsOf(restore(1, 0, 0, 0)).empty()); // nothing to do
    CHECK(stepsOf(denoiseStage(0)).empty());

    AiChain chain;
    chain.stages = {denoiseStage(40), restore(2, 0, 50, 0)};
    CHECK(stepsOf(chain).size() == 3);
    CHECK(chain.outputScale() == 2);
}

TEST_CASE("A chain names the models it needs, once each, and the download that is missing", "[ai][video][models]") {
    AiChain chain;
    chain.stages = {denoiseStage(40), restore(2, 1, 50, 30)};
    const auto models = modelsOf(chain);
    REQUIRE(models.size() == 3); // denoise (used twice), deblur, upscale
    CHECK(models[0] == Model::Denoise);
    CHECK(models[1] == Model::Deblur);
    CHECK(models[2] == Model::Upscale2x);
    const auto files = modelFilesOf(chain);
    REQUIRE(files.size() == 3);
    CHECK(std::ranges::all_of(files, [](const ModelInfo* f) { return f != nullptr; }));

    const ModelStore empty(freshDir("models-empty"));
    CHECK_FALSE(modelsInstalled(chain, empty));
    std::uint64_t total = 0;
    for (const ModelInfo* f : files) {
        total += f->size;
    }
    CHECK(missingBytes(chain, empty) == total);
    const std::string card = downloadCard(chain, empty);
    CHECK(card.find("DRUNet") != std::string::npos);
    CHECK(card.find("MIT") != std::string::npos);
    CHECK(card.find("BSD-3-Clause") != std::string::npos);
    CHECK(modelsInstalled(AiChain{}, empty) == false); // nothing to run is not "installed"
    CHECK(missingBytes(AiChain{}, empty) == 0);
}

TEST_CASE("The model version identifies the weights and the pipeline, and changes with the models", "[ai][video][models]") {
    AiChain a;
    a.stages = {denoiseStage(40)};
    AiChain b;
    b.stages = {restore(2, 1, 0, 0)};
    AiChain c;
    c.stages = {restore(2, 0, 0, 0)};
    const std::string va = modelVersion(a);
    CHECK(va.find("drunet-color-2ae3ab5e") == 0);
    CHECK(va.ends_with("-p1"));
    CHECK(modelVersion(b) != modelVersion(c));
    CHECK(modelVersion(b).find("realesrgan-x2plus-7115ba92") == 0);
    CHECK(modelVersion(c).find("realesr-general-x4v3-09b757ac") == 0);
    // A setting that changes no model (the strength) keeps the version; the cache key is the chain's hash.
    AiChain a2;
    a2.stages = {denoiseStage(80)};
    CHECK(modelVersion(a2) == va);
    CHECK(modelVersion(AiChain{}).empty());
}

TEST_CASE("Frame time estimates grow with the picture, the models and the tile overlap; the CPU is slower", "[ai][video][models]") {
    AiChain light;
    light.stages = {denoiseStage(40)};
    AiChain heavy;
    heavy.stages = {denoiseStage(40), restore(2, 1, 50, 0)};
    const double hd = estimateFrameMilliseconds(light, 1920, 1080, true);
    CHECK(hd > 0.0);
    CHECK(estimateFrameMilliseconds(light, 3840, 2160, true) > 3.5 * hd);
    CHECK(estimateFrameMilliseconds(heavy, 1920, 1080, true) > 2.0 * hd);
    CHECK(estimateFrameMilliseconds(light, 1920, 1080, false) > 3.0 * hd);
    Step small;
    small.tile = 128;
    Step large;
    large.tile = 512;
    CHECK(estimateStepMilliseconds(small, 1920, 1080, true) > estimateStepMilliseconds(large, 1920, 1080, true)); // more overlap
    CHECK(estimateFrameMilliseconds(AiChain{}, 1920, 1080, true) == 0.0);
}
