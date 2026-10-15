#pragma once

// StillImage: a display-referred still picture (8-bit BT.709-encoded R'G'B', opaque) drawn as a layer - the Color
// workspace's reference frame beside the program (I.5). Its pixels already are what the monitor showed, so it is
// tagged BT.709 / full range: the compositor decodes it to linear and the output transform encodes it back unchanged.

#include <wabcut/render/FrameView.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace wabcut::render {

class StillImage {
public:
    StillImage(int width, int height, std::vector<std::uint8_t> rgba)
        : m_width(width), m_height(height), m_rgba(std::move(rgba)) {}

    int width() const { return m_width; }
    int height() const { return m_height; }
    const std::vector<std::uint8_t>& rgba() const { return m_rgba; }

    /// A FrameView of the pixels, kept alive by `self` (the shared_ptr this image lives in). One image = one
    /// identity, so it stays resident on the GPU across frames.
    FrameView view(std::shared_ptr<const StillImage> self) const {
        FrameView v;
        v.width = m_width;
        v.height = m_height;
        v.layout = PlaneLayout::PackedRGBA;
        v.encoding = SampleEncoding::Bits8;
        v.planeCount = 1;
        v.planes[0] = PlaneView{m_rgba.data(), m_width * 4, m_width, m_height};
        v.color.matrix = ColorMatrix::RGB;
        v.color.range = ColorRange::Full;
        v.color.primaries = ColorPrimaries::BT709;
        v.color.transfer = TransferFunction::BT709;
        v.alpha = AlphaMode::None;
        v.identity = this;
        v.owner = self;
        v.keepAlive = std::move(self);
        return v;
    }

private:
    int m_width = 0;
    int m_height = 0;
    std::vector<std::uint8_t> m_rgba;
};

} // namespace wabcut::render
