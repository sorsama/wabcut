#include <wabcut/ai/search/Embedding.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wabcut::ai::search {

void normalize(Vector& v) {
    double sum = 0.0;
    for (const float x : v) {
        sum += static_cast<double>(x) * static_cast<double>(x);
    }
    if (!(sum > 0.0) || !std::isfinite(sum)) {
        std::ranges::fill(v, 0.0f);
        return;
    }
    const double scale = 1.0 / std::sqrt(sum);
    for (float& x : v) {
        x = static_cast<float>(static_cast<double>(x) * scale);
    }
}

float dot(std::span<const float> a, std::span<const float> b) {
    const std::size_t n = std::min(a.size(), b.size());
    float sum = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

// --- base64 ------------------------------------------------------------------------------------------------------

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int sextet(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

} // namespace

std::string toBase64(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const auto b0 = static_cast<unsigned char>(bytes[i]);
        const auto b1 = static_cast<unsigned char>(bytes[i + 1]);
        const auto b2 = static_cast<unsigned char>(bytes[i + 2]);
        out += kAlphabet[b0 >> 2];
        out += kAlphabet[((b0 & 0x03) << 4) | (b1 >> 4)];
        out += kAlphabet[((b1 & 0x0F) << 2) | (b2 >> 6)];
        out += kAlphabet[b2 & 0x3F];
    }
    if (i < bytes.size()) {
        const auto b0 = static_cast<unsigned char>(bytes[i]);
        const bool two = i + 1 < bytes.size();
        const auto b1 = two ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        out += kAlphabet[b0 >> 2];
        out += kAlphabet[((b0 & 0x03) << 4) | (b1 >> 4)];
        out += two ? kAlphabet[(b1 & 0x0F) << 2] : '=';
        out += '=';
    }
    return out;
}

std::optional<std::string> fromBase64(std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const int a = sextet(text[i]);
        const int b = sextet(text[i + 1]);
        const bool pad2 = last && text[i + 2] == '=' && text[i + 3] == '=';
        const bool pad1 = last && !pad2 && text[i + 3] == '=';
        const int c = pad2 ? 0 : sextet(text[i + 2]);
        const int d = pad1 || pad2 ? 0 : sextet(text[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            return std::nullopt;
        }
        const std::uint32_t n = (static_cast<std::uint32_t>(a) << 18) | (static_cast<std::uint32_t>(b) << 12) |
                                (static_cast<std::uint32_t>(c) << 6) | static_cast<std::uint32_t>(d);
        out += static_cast<char>((n >> 16) & 0xFF);
        if (!pad2) {
            out += static_cast<char>((n >> 8) & 0xFF);
        }
        if (!pad1 && !pad2) {
            out += static_cast<char>(n & 0xFF);
        }
    }
    return out;
}

std::string encodeVector(std::span<const float> v) {
    std::string bytes(v.size() * 4, '\0');
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v[i], 4);
        for (int b = 0; b < 4; ++b) {
            bytes[i * 4 + static_cast<std::size_t>(b)] = static_cast<char>((bits >> (8 * b)) & 0xFF);
        }
    }
    return toBase64(bytes);
}

std::optional<Vector> decodeVector(std::string_view base64, std::size_t expectedDimension) {
    const auto bytes = fromBase64(base64);
    if (!bytes || bytes->size() % 4 != 0 || bytes->empty()) {
        return std::nullopt;
    }
    Vector v(bytes->size() / 4);
    if (expectedDimension != 0 && v.size() != expectedDimension) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::uint32_t bits = 0;
        for (int b = 0; b < 4; ++b) {
            bits |= static_cast<std::uint32_t>(static_cast<unsigned char>((*bytes)[i * 4 + static_cast<std::size_t>(b)])) << (8 * b);
        }
        std::memcpy(&v[i], &bits, 4);
        if (!std::isfinite(v[i])) {
            return std::nullopt;
        }
    }
    return v;
}

// --- Resampling (after Pillow's Resample.c, bilinear filter) -----------------------------------------------------

namespace {

constexpr int kPrecisionBits = 32 - 8 - 2;

struct Kernel {
    int first = 0;
    std::vector<std::int32_t> weights;
};

/// Pillow's precompute_coeffs + normalize_coeffs_8bpc for the triangle filter (support 1).
std::vector<Kernel> coefficients(int inSize, int outSize) {
    const double scale = static_cast<double>(inSize) / outSize;
    const double filterScale = std::max(scale, 1.0);
    const double support = 1.0 * filterScale;
    std::vector<Kernel> kernels(static_cast<std::size_t>(outSize));
    for (int xx = 0; xx < outSize; ++xx) {
        const double center = (xx + 0.5) * scale;
        const double ss = 1.0 / filterScale;
        int xmin = static_cast<int>(center - support + 0.5);
        xmin = std::max(xmin, 0);
        int xmax = static_cast<int>(center + support + 0.5);
        xmax = std::min(xmax, inSize) - xmin;
        std::vector<double> k(static_cast<std::size_t>(std::max(xmax, 0)));
        double total = 0.0;
        for (int x = 0; x < xmax; ++x) {
            const double t = std::abs((x + xmin - center + 0.5) * ss);
            const double w = t < 1.0 ? 1.0 - t : 0.0;
            k[static_cast<std::size_t>(x)] = w;
            total += w;
        }
        Kernel& out = kernels[static_cast<std::size_t>(xx)];
        out.first = xmin;
        out.weights.resize(k.size());
        for (std::size_t x = 0; x < k.size(); ++x) {
            const double w = total != 0.0 ? k[x] / total : 0.0;
            const double fixed = w * static_cast<double>(1 << kPrecisionBits);
            out.weights[x] = static_cast<std::int32_t>(w < 0 ? -0.5 + fixed : 0.5 + fixed);
        }
    }
    return kernels;
}

std::uint8_t clip8(std::int64_t value) {
    const std::int64_t shifted = value >> kPrecisionBits;
    return static_cast<std::uint8_t>(std::clamp<std::int64_t>(shifted, 0, 255));
}

} // namespace

Image resizeBilinear(const Image& image, int width, int height) {
    if (!image.valid() || width <= 0 || height <= 0) {
        return {};
    }
    // Horizontal pass, then vertical, each rounded to 8 bits (as Pillow does).
    Image horizontal;
    if (width == image.width) {
        horizontal = image;
    } else {
        const auto kernels = coefficients(image.width, width);
        horizontal.width = width;
        horizontal.height = image.height;
        horizontal.rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(image.height) * 3);
        for (int y = 0; y < image.height; ++y) {
            const std::uint8_t* row = &image.rgb[static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) * 3];
            std::uint8_t* outRow = &horizontal.rgb[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 3];
            for (int x = 0; x < width; ++x) {
                const Kernel& k = kernels[static_cast<std::size_t>(x)];
                for (int c = 0; c < 3; ++c) {
                    std::int64_t sum = std::int64_t{1} << (kPrecisionBits - 1);
                    for (std::size_t i = 0; i < k.weights.size(); ++i) {
                        sum += static_cast<std::int64_t>(row[(static_cast<std::size_t>(k.first) + i) * 3 + static_cast<std::size_t>(c)]) *
                               k.weights[i];
                    }
                    outRow[static_cast<std::size_t>(x) * 3 + static_cast<std::size_t>(c)] = clip8(sum);
                }
            }
        }
    }
    if (height == horizontal.height) {
        return horizontal;
    }
    const auto kernels = coefficients(horizontal.height, height);
    Image out;
    out.width = width;
    out.height = height;
    out.rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
    const std::size_t stride = static_cast<std::size_t>(width) * 3;
    for (int y = 0; y < height; ++y) {
        const Kernel& k = kernels[static_cast<std::size_t>(y)];
        for (std::size_t x = 0; x < stride; ++x) {
            std::int64_t sum = std::int64_t{1} << (kPrecisionBits - 1);
            for (std::size_t i = 0; i < k.weights.size(); ++i) {
                sum += static_cast<std::int64_t>(horizontal.rgb[(static_cast<std::size_t>(k.first) + i) * stride + x]) * k.weights[i];
            }
            out.rgb[static_cast<std::size_t>(y) * stride + x] = clip8(sum);
        }
    }
    return out;
}

} // namespace wabcut::ai::search
