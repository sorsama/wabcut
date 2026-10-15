#include <wabcut/media/FrameHandle.h>

#include <cstdlib>

namespace wabcut::media {

std::string_view pixelFormatName(PixelFormat format) {
    switch (format) {
    case PixelFormat::NV12:
        return "nv12";
    case PixelFormat::P010:
        return "p010";
    case PixelFormat::YUV420P:
        return "yuv420p";
    case PixelFormat::YUV420P10:
        return "yuv420p10";
    case PixelFormat::YUV422P:
        return "yuv422p";
    case PixelFormat::YUV422P10:
        return "yuv422p10";
    case PixelFormat::YUV444P:
        return "yuv444p";
    case PixelFormat::YUV444P10:
        return "yuv444p10";
    case PixelFormat::RGBA8:
        return "rgba8";
    case PixelFormat::YUVA420P:
        return "yuva420p";
    case PixelFormat::YUVA444P10:
        return "yuva444p10";
    case PixelFormat::Unknown:
        break;
    }
    return "unknown";
}

int pixelFormatPlaneCount(PixelFormat format) {
    switch (format) {
    case PixelFormat::NV12:
    case PixelFormat::P010:
        return 2;
    case PixelFormat::YUV420P:
    case PixelFormat::YUV420P10:
    case PixelFormat::YUV422P:
    case PixelFormat::YUV422P10:
    case PixelFormat::YUV444P:
    case PixelFormat::YUV444P10:
        return 3;
    case PixelFormat::YUVA420P:
    case PixelFormat::YUVA444P10:
        return 4;
    case PixelFormat::RGBA8:
        return 1;
    case PixelFormat::Unknown:
        break;
    }
    return 0;
}

int pixelFormatBitDepth(PixelFormat format) {
    switch (format) {
    case PixelFormat::P010:
    case PixelFormat::YUV420P10:
    case PixelFormat::YUV422P10:
    case PixelFormat::YUV444P10:
    case PixelFormat::YUVA444P10:
        return 10;
    case PixelFormat::Unknown:
        return 0;
    default:
        return 8;
    }
}

int pixelFormatBytesPerTexel(PixelFormat format, int plane) {
    if (plane < 0 || plane >= pixelFormatPlaneCount(format)) {
        return 0;
    }
    const int sampleBytes = pixelFormatBitDepth(format) > 8 ? 2 : 1;
    switch (format) {
    case PixelFormat::NV12:
    case PixelFormat::P010:
        return plane == 0 ? sampleBytes : 2 * sampleBytes;
    case PixelFormat::RGBA8:
        return 4;
    default:
        return sampleBytes;
    }
}

bool pixelFormatHasAlpha(PixelFormat format) {
    return format == PixelFormat::RGBA8 || format == PixelFormat::YUVA420P ||
           format == PixelFormat::YUVA444P10;
}

DeviceToken FrameHandle::device() const {
    return std::visit(
        [](const auto& p) -> DeviceToken {
            if constexpr (std::is_same_v<std::decay_t<decltype(p)>, std::monostate>) {
                return {};
            } else {
                return p.device;
            }
        },
        payload);
}

bool FrameHandle::isValid() const {
    if (format == PixelFormat::Unknown || width <= 0 || height <= 0) {
        return false;
    }
    return std::visit(
        [](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return false;
            } else if constexpr (std::is_same_v<T, CpuPlanes>) {
                return p.owner != nullptr && p.planes[0].data != nullptr;
            } else {
                return p.owner != nullptr;
            }
        },
        payload);
}

std::size_t FrameHandle::byteSize() const {
    if (const CpuPlanes* planes = cpu()) {
        std::size_t total = 0;
        const int n = planeCount();
        for (int i = 0; i < n; ++i) {
            const Plane& p = planes->planes[static_cast<std::size_t>(i)];
            total += static_cast<std::size_t>(std::abs(p.stride)) * static_cast<std::size_t>(p.height);
        }
        return total;
    }
    if (payloadKind() == PayloadKind::None) {
        return 0;
    }
    // Native surfaces: estimate from the layout (luma plane plus chroma/alpha at their subsampled sizes).
    std::size_t total = 0;
    const int n = planeCount();
    for (int i = 0; i < n; ++i) {
        const bool full = i == 0 || i == 3 || format == PixelFormat::YUV444P ||
                          format == PixelFormat::YUV444P10 || format == PixelFormat::YUVA444P10 ||
                          format == PixelFormat::RGBA8;
        const bool halfHeight = !full && format != PixelFormat::YUV422P && format != PixelFormat::YUV422P10;
        const std::size_t w =
            full ? static_cast<std::size_t>(width) : static_cast<std::size_t>((width + 1) / 2);
        const std::size_t h =
            halfHeight ? static_cast<std::size_t>((height + 1) / 2) : static_cast<std::size_t>(height);
        total += w * h * static_cast<std::size_t>(pixelFormatBytesPerTexel(format, i));
    }
    return total;
}

} // namespace wabcut::media
