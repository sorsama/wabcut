#pragma once

// Embeddings for smart search (docs/ai.md, "Smart search"): pictures and texts mapped into one vector space by an
// image-text model (SigLIP 2), so a text query finds the frames that show it. Vectors are L2-normalised; their dot
// product is the cosine similarity used for ranking.
//
// The model itself runs out of process (wabcut-embed, HelperEmbedder.h); this header holds what both sides share:
// the Embedder interface, vector arithmetic, the exact wire/cache encoding of vectors and the picture resampling the
// model expects (SiglipImageProcessor: 224 x 224, PIL bilinear).

#include <wabcut/model/Error.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::search {

using Vector = std::vector<float>;

/// Scales `v` to unit length; a zero (or non-finite) vector becomes all zeros.
void normalize(Vector& v);
float dot(std::span<const float> a, std::span<const float> b);

/// The vector's float32 values, little-endian, as base64: exact and a third the size of decimal JSON numbers.
std::string encodeVector(std::span<const float> v);
/// The inverse; nullopt for invalid base64, a byte count that is not a multiple of 4, a dimension other than
/// `expectedDimension` (when non-zero) or a non-finite value.
std::optional<Vector> decodeVector(std::string_view base64, std::size_t expectedDimension = 0);

std::string toBase64(std::string_view bytes);
std::optional<std::string> fromBase64(std::string_view text);

/// A picture: packed 8-bit RGB, row by row (width * height * 3 bytes).
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;

    bool valid() const {
        return width > 0 && height > 0 && rgb.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
    }
};

/// Resizes like PIL's Image.resize(..., BILINEAR): a triangle filter, widened by the scale factor when shrinking
/// (antialiased), separable, rounding to 8 bits after each pass. This is what SiglipImageProcessor (resample=2)
/// does before the model sees a picture. Returns an empty Image for an invalid input or size.
Image resizeBilinear(const Image& image, int width, int height);

/// Turns pictures and texts into vectors of one space.
class Embedder {
public:
    virtual ~Embedder() = default;
    /// One L2-normalised vector per picture, in order. Errors: the engine's (DecoderError for a failed or crashed
    /// helper, NotFound for a missing model), Cancelled.
    virtual std::expected<std::vector<Vector>, Error> embedImages(std::span<const Image> images, std::stop_token stop) = 0;
    /// The L2-normalised vector of a search text.
    virtual std::expected<Vector, Error> embedText(std::string_view text, std::stop_token stop) = 0;
    /// Identity of the model and its preprocessing: vectors of different versions are never compared.
    virtual std::string modelVersion() const = 0;
    virtual int dimension() const = 0;
    /// What the model runs on ("DirectML", "CPU", ...), once known; for status lines.
    virtual std::string device() const { return {}; }
};

} // namespace wabcut::ai::search
