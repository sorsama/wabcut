#include "ReferenceTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <sstream>

namespace wabcut::render::reference::test {

GoldenCheck compareWithGolden(const std::string& name, const Image& image, const std::filesystem::path& goldenDir,
                              const std::filesystem::path& outputDir, bool update, double tolerance) {
    GoldenCheck out;
    const std::filesystem::path golden = goldenDir / (name + ".wri");
    if (update && !writeGolden(golden, image)) {
        out.message = "cannot write the golden " + golden.string();
    }
    const auto want = readGolden(golden);
    if (want) {
        out.difference = compare(*want, image);
        out.matches = out.difference.maxAbs <= tolerance;
    }
    if (!out.matches) {
        out.written = outputDir / (name + ".wri");
        const bool saved = writeGolden(out.written, image);
        std::ostringstream text;
        if (!want) {
            text << want.error().message;
        } else {
            text << "golden " << golden.string() << " differs: max abs " << out.difference.maxAbs << " > " << tolerance
                 << ", PSNR " << out.difference.psnr << " dB";
        }
        text << "; the new picture " << (saved ? "is " : "could not be written to ")
             << std::filesystem::absolute(out.written).string() << " (WABCUT_UPDATE_GOLDEN=1 rewrites the goldens)";
        if (!out.message.empty()) {
            text << "; " << out.message;
        }
        out.message = text.str();
    }
    return out;
}

void checkGolden(const std::string& name, const Image& image, double tolerance) {
    const bool update = std::getenv("WABCUT_UPDATE_GOLDEN") != nullptr;
    const GoldenCheck result =
        compareWithGolden(name, image, WABCUT_RENDER_REFERENCE_GOLDENS,
                          std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "render_reference", update, tolerance);
    INFO(result.message);
    CHECK(result.matches);
}

Pixel premultiplied(double r, double g, double b, double a) {
    return {static_cast<float>(r * a), static_cast<float>(g * a), static_cast<float>(b * a), static_cast<float>(a)};
}

Pixel fromEncoded(const Rgb& encoded, double a) {
    return premultiplied(toLinear(encoded[0]), toLinear(encoded[1]), toLinear(encoded[2]), a);
}

} // namespace wabcut::render::reference::test
