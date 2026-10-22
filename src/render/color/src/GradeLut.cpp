// Lumetri's Export .cube (0.2, docs/color.md "Export .cube"): a grade baked into a 3D LUT by the CPU reference.

#include <wabcut/color/ColorMath.h>
#include <wabcut/color/Grade.h>

#include <algorithm>
#include <cstddef>

namespace wabcut::color {

namespace {

/// What a title line of a .cube may hold: no '#' (the parser strips comments), quotes or line breaks; at most 200
/// bytes (parseCube keeps 256).
std::string cubeTitle(std::string title) {
    std::erase_if(title, [](char c) { return c == '#' || c == '"' || c == '\n' || c == '\r'; });
    if (title.size() > 200) {
        title.resize(200);
    }
    return title;
}

} // namespace

Lut gradeToLut(const GradeSpec& spec, int size, std::string title) {
    size = std::clamp(size, 2, kMaxLut3dSize);
    GradeSpec pixelLocal = spec;
    pixelLocal.params.vignette = false; // depends on where the pixel is: no LUT can hold it
    Lut lut;
    lut.title = cubeTitle(std::move(title));
    lut.size3d = size;
    lut.table3d.resize(static_cast<std::size_t>(size) * size * size * 3);
    std::size_t k = 0;
    const double step = 1.0 / (size - 1);
    for (int b = 0; b < size; ++b) {
        for (int g = 0; g < size; ++g) {
            for (int r = 0; r < size; ++r) {
                // The lattice is display-referred Rec.709 code values, the grade works on linear light.
                const Rgb code{r * step, g * step, b * step};
                const Rgb out = encode709(gradePixel(pixelLocal, decode709(code)));
                for (std::size_t c = 0; c < 3; ++c) {
                    lut.table3d[k++] = static_cast<float>(std::clamp(out[c], 0.0, 1.0));
                }
            }
        }
    }
    return lut;
}

std::vector<std::string> gradeLutOmissions(const GradeSpec& spec, const EffectInstance& instance) {
    std::vector<std::string> out;
    if (spec.vignetteActive()) {
        out.emplace_back("Vignette");
    }
    if (spec.hslActive() && spec.hslNeedsRefine()) {
        out.emplace_back("HSL Secondary denoise, blur and sharpen");
    }
    if (!instance.masks.empty()) {
        out.emplace_back("masks");
    }
    if (spec.missingLuts > 0) {
        out.emplace_back("missing LUT files");
    }
    return out;
}

} // namespace wabcut::color
