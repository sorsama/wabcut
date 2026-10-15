#pragma once

// Helpers of the reference-render tests: golden files and small pictures.

#include <wabcut/render/reference/Image.h>

#include <filesystem>
#include <string>

namespace wabcut::render::reference::test {

/// Goldens hold six significant digits: a value read back is within 5e-7 relative of what was written.
inline constexpr double kGoldenTolerance = 2e-5;

struct GoldenCheck {
    bool matches = false;
    Difference difference;
    std::string message;           ///< why it does not match (empty when it does)
    std::filesystem::path written; ///< the new picture's file on a mismatch (empty when it matches)
};

/// Compares `image` with `<goldenDir>/<name>.wri`. With `update` the golden is (re)written first. On a mismatch or a
/// missing golden the picture is written to `<outputDir>/<name>.wri`.
GoldenCheck compareWithGolden(const std::string& name, const Image& image, const std::filesystem::path& goldenDir,
                              const std::filesystem::path& outputDir, bool update, double tolerance = kGoldenTolerance);

/// The suite's check: goldens in tests/render_reference/goldens, mismatches written to test-output/render_reference,
/// WABCUT_UPDATE_GOLDEN=1 (any value) rewrites the goldens. Fails the test (CHECK) on a mismatch.
void checkGolden(const std::string& name, const Image& image, double tolerance = kGoldenTolerance);

/// A straight colour and alpha as a premultiplied pixel.
Pixel premultiplied(double r, double g, double b, double a);
/// Encoded (sequence encoding) straight colour -> linear premultiplied pixel.
Pixel fromEncoded(const Rgb& encoded, double a);

} // namespace wabcut::render::reference::test
