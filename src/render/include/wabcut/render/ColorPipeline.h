#pragma once

// ColorPipeline: how source pixels reach the compositor's working space and how the working space is encoded
// for the display and for export (docs/architecture.md, "render"; Principle 5: preview == export).
//
//   source Y'CbCr / R'G'B' code values
//     -> R'G'B' (matrix + range, chroma upsampled at its siting)       [resolveSourceColor(), yuvToRgb()]
//     -> linear light (inverse of the source transfer function)        [transferToLinear()]
//     -> Rec.709 primaries, linear: the working space                  [primariesToRec709()]
//     -> premultiplied alpha; every blend happens here, in RGBA16F
//     -> output transform: display or export encoding                  [linearToOutput()]
//
// Colour management (version 2, I.9, docs/color.md "Colour management"): with `management` other than the default
// display-referred Rec.709, the layers reach the project's working space (ACEScg; linear Rec.2020 with 1.0 = the
// reference white for HDR) through their input colour space (color::planInput: analytic BT.709 / sRGB / BT.2020 /
// PQ / HLG curves, or an OpenColorIO transform baked into a 3D LUT), and the outputs come from color::planOutput (the
// ACES output transforms, PQ, HLG, or the tone-mapped SDR preview of HDR). The default leaves everything above as it
// was, byte for byte.
//
// The struct is versioned and value-typed: it travels inside every PreparedComposite and will be stored with
// the sequence settings. The functions below are the CPU reference of exactly what the shaders do; the
// compositor feeds their results to the GPU as uniforms, and tests compare GPU output against them.

#include <wabcut/color/ColorSpaces.h>
#include <wabcut/model/ColorManagement.h>
#include <wabcut/model/MediaInfo.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace wabcut::render {

/// Encoding of the linear working space for one output.
enum class OutputTransform : std::uint8_t {
    /// BT.709 OETF (BT.601/BT.2020 SDR use the same curve). A Rec.709 source passes through unchanged, so
    /// this
    /// shows exactly the code values an export will contain, as an unmanaged SDR viewer (Premiere's default
    /// monitor, common players) does.
    Rec709,
    /// IEC 61966-2-1 sRGB encoding of the linear working space (stills, web graphics, a managed sRGB viewer).
    SRGB,
    /// No encoding: scene-linear values (float targets, debugging).
    Linear,
};

std::string_view outputTransformName(OutputTransform transform);

/// Extension point for OCIO (or any LUT-based transform). Interface only in the MVP: the SDR pipeline never
/// calls it. A hook replaces the built-in output transform of the output it is attached to.
class ColorTransformHook {
public:
    virtual ~ColorTransformHook() = default;
    /// Stable identity of the configuration (config file + view + display), used as the cache key for baked
    /// LUTs.
    virtual std::string cacheKey() const = 0;
    /// Bakes the transform from linear Rec.709 working-space RGB into a `size`^3 RGBA float lattice
    /// (red fastest). Returns false when the transform cannot be represented that way.
    virtual bool bake3dLut(int size, std::span<float> rgbaOut) const = 0;
};

/// The colour contract of one render request. Plain value; compare with ==.
struct ColorPipeline {
    static constexpr std::uint32_t kCurrentVersion = 2;

    /// Serialized layout/semantics version. Readers reject versions they do not know.
    std::uint32_t version = kCurrentVersion;
    /// Applied to every colour field a frame leaves Unknown (the contract's defined SDR fallback, BT.709
    /// limited). Decoders already resolve untagged streams (BT.601 for SD, BT.709 for HD), so this is the
    /// last resort.
    ColorInfo untaggedFallback{ColorMatrix::BT709, ColorRange::Limited, ColorPrimaries::BT709,
                               TransferFunction::BT709, ChromaLocation::Left};
    /// Program monitor / any on-screen view.
    OutputTransform displayTransform = OutputTransform::Rec709;
    /// Export / render to file.
    OutputTransform exportTransform = OutputTransform::Rec709;
    /// OCIO hook points (null = built-in transform). Reserved: the MVP ignores them.
    std::shared_ptr<const ColorTransformHook> displayHook;
    std::shared_ptr<const ColorTransformHook> exportHook;
    /// Version 2 (I.9): the project's colour management. The default (display-referred Rec.709) is the pipeline above.
    ColorManagement management;
    /// Version 2, managed projects: what an export writes - SDR (an ACES SDR output, or the tone-mapped SDR version of an
    /// HDR project), PQ or HLG (an HDR file). The display is always SDR here (an HDR project shows its tone-mapped
    /// preview). Ignored by the default pipeline.
    color::OutputTarget exportTarget = color::OutputTarget::Sdr;

    /// A managed working space (Wide Gamut or HDR; ColorManagement::managed). Direct Rec.709 and Disable Color
    /// Management keep the display-referred pipeline (Direct Rec.709 tone-maps HDR and log media on input).
    bool managed() const { return management.managed(); }

    bool isSupportedVersion() const { return version >= 1 && version <= kCurrentVersion; }
    bool operator==(const ColorPipeline&) const = default;
};

/// Which output of a pipeline a render targets.
enum class OutputRole : std::uint8_t { Display, Export };

inline OutputTransform outputTransformFor(const ColorPipeline& pipeline, OutputRole role) {
    return role == OutputRole::Display ? pipeline.displayTransform : pipeline.exportTransform;
}

/// A managed pipeline's output target for `role` (the display is SDR).
inline color::OutputTarget outputTargetFor(const ColorPipeline& pipeline, OutputRole role) {
    return role == OutputRole::Display ? color::OutputTarget::Sdr : pipeline.exportTarget;
}

/// The pipeline of a project's colour management (display and export Rec.709, as the default pipeline has).
inline ColorPipeline colorPipelineFor(const ColorManagement& management) {
    ColorPipeline pipeline;
    pipeline.management = management;
    return pipeline;
}

// ---- Source colour resolution ----

/// A frame's colour description with every Unknown replaced, plus what the MVP pipeline can do with it.
struct ResolvedColor {
    ColorMatrix matrix = ColorMatrix::BT709;
    ColorRange range = ColorRange::Limited;
    ColorPrimaries primaries = ColorPrimaries::BT709;
    TransferFunction transfer = TransferFunction::BT709;
    ChromaLocation chromaLocation = ChromaLocation::Left;
    /// PQ/HLG or wide-gamut HDR. Detected and flagged; the MVP decodes it with the SDR BT.709 curve.
    bool hdr = false;
};

/// Resolves `color` against the pipeline's fallback. `isRgb` frames (RGBA8) ignore the matrix and default to
/// full range and sRGB transfer when untagged.
ResolvedColor resolveSourceColor(const ColorInfo& color, const ColorPipeline& pipeline, bool isRgb);

// ---- Reference math (double precision) ----

/// Row-major 3x3 matrix.
struct Mat3 {
    std::array<double, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};

    std::array<double, 3> operator*(const std::array<double, 3>& v) const {
        return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
                m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
    }
    Mat3 operator*(const Mat3& o) const;
    static Mat3 identity() { return {}; }
};

Mat3 inverse(const Mat3& a);

/// How plane samples are stored in their textures (UNORM), which fixes how a normalized sample maps back to a
/// code value.
enum class SampleEncoding : std::uint8_t {
    Bits8,      ///< 8-bit code value c in an 8-bit texel: sample = c / 255.
    Bits10Low,  ///< 10-bit code value in the low bits of a 16-bit texel (yuv4xxp10le): sample = c / 65535.
    Bits10High, ///< 10-bit code value in the high bits of a 16-bit texel (P010): sample = 64c / 65535.
};

/// Affine map from normalized plane samples (Y, Cb, Cr) to non-linear R'G'B' in [0, 1]:
/// rgb = matrix * sample + offset. For RGB frames it only applies the range.
struct SampleToRgb {
    Mat3 matrix;
    std::array<double, 3> offset{0, 0, 0};

    std::array<double, 3> apply(const std::array<double, 3>& sample) const {
        std::array<double, 3> r = matrix * sample;
        for (int i = 0; i < 3; ++i) {
            r[static_cast<std::size_t>(i)] += offset[static_cast<std::size_t>(i)];
        }
        return r;
    }
};

/// Luma coefficients (Kr, Kb) of a matrix; BT.709's for Unknown/RGB.
std::array<double, 2> lumaCoefficients(ColorMatrix matrix);

/// Y'CbCr samples -> R'G'B' for the given matrix/range/storage. `isRgb` frames get only the range scaling.
SampleToRgb sampleToRgb(ColorMatrix matrix, ColorRange range, SampleEncoding encoding, bool isRgb);

/// Inverse transfer function (encoded value -> relative linear light), defined on [0, 1]. PQ / HLG use the BT.709
/// curve here: their pictures are tone-mapped as a whole (color::hdrCaptureToSdr; referencePixel does).
double transferToLinear(TransferFunction transfer, double encoded);
/// Output encoding of linear light.
double linearToOutput(OutputTransform transform, double linear);

/// Linear RGB in `primaries` -> linear Rec.709 RGB (D65; DCI-P3's white is Bradford-adapted). Identity for
/// BT.709 and Unknown.
Mat3 primariesToRec709(ColorPrimaries primaries);

/// Full reference conversion of one pixel: normalized samples (Y, Cb, Cr) or (R, G, B), straight alpha
/// `alpha`, to the encoded output value (no blending, composited over black when alpha < 1). Returns R, G, B
/// in [0, 1].
std::array<double, 3> referencePixel(const std::array<double, 3>& samples, double alpha,
                                     const ResolvedColor& color, SampleEncoding encoding, bool isRgb,
                                     OutputTransform output);

} // namespace wabcut::render
