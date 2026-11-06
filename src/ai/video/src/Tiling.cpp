#include <wabcut/ai/video/Tiling.h>

#include <algorithm>
#include <cmath>

namespace wabcut::ai::video {

int reflectIndex(int i, int n) {
    if (n <= 1) {
        return 0;
    }
    const int period = 2 * (n - 1);
    int m = i % period;
    if (m < 0) {
        m += period;
    }
    return m < n ? m : period - m;
}

int tileCount(int width, int height, int tile) {
    if (width <= 0 || height <= 0 || tile <= 0) {
        return 0;
    }
    return ((width + tile - 1) / tile) * ((height + tile - 1) / tile);
}

std::expected<TiledResult, Error> runTiled(const Picture& in, const TileSpec& spec, const TileModel& model,
                                           const std::function<bool()>& cancelled) {
    if (!in.valid() || spec.tile <= 0 || spec.pad < 0 || (spec.scale != 1 && spec.scale != 2 && spec.scale != 4) || !model) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "tiled processing needs a picture, a tile size and a model"));
    }
    const int size = spec.tile + 2 * spec.pad;
    const int scale = spec.scale;
    const std::size_t plane = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    const int outSize = size * scale;
    const std::size_t outPlane = static_cast<std::size_t>(outSize) * static_cast<std::size_t>(outSize);

    TiledResult result;
    result.picture.width = in.width * scale;
    result.picture.height = in.height * scale;
    result.picture.rgb.assign(result.picture.pixels() * 3, 0);

    std::vector<float> tile(plane * 3);
    std::vector<float> produced;
    // Column / row source indices of a window are the same for every tile of its column / row: reflect once per window.
    std::vector<int> cols(static_cast<std::size_t>(size));
    std::vector<int> rows(static_cast<std::size_t>(size));
    for (int cy = 0; cy < in.height; cy += spec.tile) {
        for (int i = 0; i < size; ++i) {
            rows[static_cast<std::size_t>(i)] = reflectIndex(cy - spec.pad + i, in.height);
        }
        for (int cx = 0; cx < in.width; cx += spec.tile) {
            if (cancelled && cancelled()) {
                return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
            }
            for (int i = 0; i < size; ++i) {
                cols[static_cast<std::size_t>(i)] = reflectIndex(cx - spec.pad + i, in.width);
            }
            for (int y = 0; y < size; ++y) {
                const std::size_t rowBase = static_cast<std::size_t>(rows[static_cast<std::size_t>(y)]) * static_cast<std::size_t>(in.width);
                for (int x = 0; x < size; ++x) {
                    const std::size_t src = (rowBase + static_cast<std::size_t>(cols[static_cast<std::size_t>(x)])) * 3;
                    const std::size_t dst = static_cast<std::size_t>(y) * static_cast<std::size_t>(size) + static_cast<std::size_t>(x);
                    tile[dst] = static_cast<float>(in.rgb[src]) * (1.0f / 65535.0f);
                    tile[plane + dst] = static_cast<float>(in.rgb[src + 1]) * (1.0f / 65535.0f);
                    tile[2 * plane + dst] = static_cast<float>(in.rgb[src + 2]) * (1.0f / 65535.0f);
                }
            }
            produced.clear();
            if (auto ran = model(tile, size, produced); !ran) {
                return std::unexpected(ran.error());
            }
            if (produced.size() != outPlane * 3) {
                return std::unexpected(Error(ErrorCode::DecoderError, "the model returned a tile of the wrong size"));
            }
            const int cw = std::min(spec.tile, in.width - cx);
            const int ch = std::min(spec.tile, in.height - cy);
            for (int y = 0; y < ch * scale; ++y) {
                const std::size_t srcRow = static_cast<std::size_t>(spec.pad * scale + y) * static_cast<std::size_t>(outSize) +
                                           static_cast<std::size_t>(spec.pad * scale);
                const std::size_t dstRow = (static_cast<std::size_t>(cy * scale + y) * static_cast<std::size_t>(result.picture.width) +
                                            static_cast<std::size_t>(cx * scale)) *
                                           3;
                for (int x = 0; x < cw * scale; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        float v = produced[static_cast<std::size_t>(c) * outPlane + srcRow + static_cast<std::size_t>(x)];
                        v = std::isfinite(v) ? v : 0.0f; // a model's NaN is black, never undefined
                        result.picture.rgb[dstRow + static_cast<std::size_t>(x) * 3 + static_cast<std::size_t>(c)] =
                            static_cast<std::uint16_t>(std::clamp(v * 65535.0f + 0.5f, 0.0f, 65535.0f));
                    }
                }
            }
            ++result.tiles;
        }
    }
    return result;
}

} // namespace wabcut::ai::video
