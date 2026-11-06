#pragma once

// Any picture through an image-to-image model in overlapping tiles, so memory is bounded by the tile and not by the
// picture (a 4K frame at 4x would not fit a model in one go). The picture is cut into cores of `tile` x `tile` pixels
// (the last row / column smaller); each core is run with `pad` pixels of context on every side (reflected at the
// picture's edges, so the model never sees a hard border), and only the core of the result is kept: no blending, so no
// ghosting, and the seam error is the model's own response to the missing context beyond the pad (Real-ESRGAN's tile
// mode does the same). Every tile the model sees is the same size (tile + 2 pad), which DirectML wants, and a multiple of
// `multiple` (the network's stride: 8 for the CNNs here, 16 for NAFNet), which the caller guarantees by its choice of
// tile and pad.

#include <wabcut/ai/video/Picture.h>
#include <wabcut/model/Error.h>

#include <expected>
#include <functional>
#include <span>
#include <vector>

namespace wabcut::ai::video {

struct TileSpec {
    int tile = 256; ///< a core's size in input pixels
    int pad = 16;   ///< context on every side, input pixels
    int scale = 1;  ///< the model's output is scale x its input
};

/// One tile through the model: `input` is 3 x `size` x `size` floats (planar R, G, B; 0..1; size = tile + 2 pad); the
/// model writes 3 x (size x scale) x (size x scale) floats to `output`. An error ends the run.
using TileModel = std::function<std::expected<void, Error>(std::span<const float> input, int size, std::vector<float>& output)>;

struct TiledResult {
    Picture picture; ///< colour only (alpha is the caller's)
    int tiles = 0;
};

/// The picture's colour through `model` tile by tile. `cancelled` is asked between tiles. Errors: InvalidArgument (a bad
/// spec or picture), Cancelled, the model's.
std::expected<TiledResult, Error> runTiled(const Picture& in, const TileSpec& spec, const TileModel& model,
                                           const std::function<bool()>& cancelled = {});

/// How many tiles a picture takes.
int tileCount(int width, int height, int tile);

/// The index `i` reflected into [0, n) without repeating the edge (-1 -> 1, n -> n - 2), also for tiny pictures.
int reflectIndex(int i, int n);

} // namespace wabcut::ai::video
