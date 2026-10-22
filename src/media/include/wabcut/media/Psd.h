#pragma once

// Layered Photoshop files (0.2, model/LayeredFile.h): a reader for PSD and PSB documents - the layer records (names,
// bounds, opacity, visibility, blend modes, groups, clipping) and their pixels (raw, PackBits RLE, ZIP with or without
// prediction; 8 and 16 bits and 32-bit float; RGB, grayscale and CMYK, with transparency and layer masks) - and the
// composite of chosen layers that Premiere's Import Layered File makes. CMYK documents are converted to RGB without
// their colour profile (each ink taken off white); 32-bit documents are linear light, merged as such and written out
// with the sRGB curve. Files are untrusted: every length and count is checked against the file and
// against fixed bounds, and anything malformed is an error, never a crash. No FFmpeg or Qt here.

#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace wabcut::media {

struct PsdLayer {
    int index = 0;      ///< position in the file, 0 = the bottom layer
    std::string name;   ///< UTF-8 (the Unicode name when the file has one)
    int left = 0;       ///< pixel bounds in document pixels, [left, right) x [top, bottom)
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool visible = true;   ///< its own eye and every group above it
    double opacity = 1.0;  ///< its opacity times its groups' opacities, [0, 1]
    std::string blendMode; ///< Photoshop's key: "norm", "mul ", "scrn", "over", "hue ", "lum ", ...
    bool clipped = false;  ///< a clipping mask: shown only where the layer below it (its base) is
    bool group = false;    ///< a group's folder or end marker: no pixels of its own
    std::vector<std::string> groups; ///< the groups holding it, outermost first

    int width() const { return right > left ? right - left : 0; }
    int height() const { return bottom > top ? bottom - top : 0; }
    bool hasPixels() const { return !group && width() > 0 && height() > 0; }
};

struct PsdDocument {
    int width = 0;
    int height = 0;
    int depth = 8;     ///< bits per channel: 8, 16 or 32 (float)
    bool psb = false;  ///< the large document format
    bool grayscale = false;
    bool cmyk = false; ///< converted to RGB on reading
    std::vector<PsdLayer> layers; ///< bottom to top; empty for a flattened file (its merged picture is the image)
};

/// A picture: straight alpha, 16 bits per sample (8-bit documents scaled by 257; 32-bit ones clamped to [0, 1] and
/// encoded with the sRGB curve), rows top to bottom, RGBA.
struct PsdImage {
    int width = 0;
    int height = 0;
    /// Where its top-left pixel sits in the document.
    int left = 0;
    int top = 0;
    int depth = 8; ///< the document's depth (what the picture is worth): 8, 16 or 32
    std::vector<std::uint16_t> rgba;
};

/// Bounds: a side of the document (PSB's limit), the pixels of a layer or the document (1 Gpx), layers in a file.
inline constexpr int kMaxPsdSide = 300'000;
inline constexpr std::int64_t kMaxPsdPixels = 1'000'000'000;
inline constexpr std::size_t kMaxPsdLayers = 8192;

/// The document's header and layer records (no pixel data is read). Errors: NotFound, IoError, Unsupported (not a
/// PSD / PSB, a colour mode other than RGB, CMYK or grayscale, 1-bit channels), InvalidArgument (a malformed file).
std::expected<PsdDocument, Error> readPsdDocument(const std::filesystem::path& file);
/// The same from bytes in memory (fuzzing, tests).
std::expected<PsdDocument, Error> readPsdDocument(std::span<const std::uint8_t> bytes);

/// Premiere's Import Layered File: `layers` (indices, any order; empty = every visible layer) merged bottom to top with
/// their opacity, blend mode, clipping and masks; hidden layers among them are left out (the dialog lists the visible
/// ones). `documentSize`: on the document's canvas; else cropped to the bounds of the merged layers' pixels (Layer
/// Size). A flattened file's merged picture when it has no layers. Errors as readPsdDocument, and InvalidArgument for
/// an index the file does not have.
std::expected<PsdImage, Error> compositePsd(const std::filesystem::path& file, std::span<const int> layers,
                                            bool documentSize);
std::expected<PsdImage, Error> compositePsd(std::span<const std::uint8_t> bytes, std::span<const int> layers,
                                            bool documentSize);

/// The picture as a PAM file (netpbm "P7", TUPLTYPE RGB_ALPHA; MAXVAL 255 for 8-bit documents, 65535 for 16 and 32),
/// which FFmpeg's pam_pipe demuxer reads.
std::string toPam(const PsdImage& image);

} // namespace wabcut::media
