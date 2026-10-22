#pragma once

// Pictures of synthetic media (P.2: Bars and Tone, the Universal Counting Leader; model/Synthetic.h), drawn on the CPU
// as premultiplied sRGB-encoded RGBA8 rasters like titles (render::TitleRaster): the compositor uploads them as frames.

#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>
#include <wabcut/render/PreparedComposite.h>
#include <wabcut/render/Title.h>

#include <memory>

namespace wabcut::render {

/// The picture of synthetic item `item` at media time `time`, `size` pixels: the bars of its pattern
/// (barsPattern: patches in video levels, ramps interpolated, below-black pluge clipped), or the counting leader
/// (countingLeaderAt: background, clock wipe, cross-hair, target circles and the number). Recent pictures are kept (a
/// bars clip draws once; the leader once per frame). Null for other items.
std::shared_ptr<const TitleRaster> syntheticPicture(const MediaItem& item, Ticks time, CanvasSize size);

} // namespace wabcut::render
