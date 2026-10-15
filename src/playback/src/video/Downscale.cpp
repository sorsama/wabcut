#include "Downscale.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <numeric>

namespace wabcut::playback::detail {

namespace {

/// Samples per texel of a plane (NV12/P010 chroma: U,V pairs; RGBA8: four).
int channelsOf(media::PixelFormat format, int plane) {
    switch (format) {
    case media::PixelFormat::NV12:
    case media::PixelFormat::P010:
        return plane == 1 ? 2 : 1;
    case media::PixelFormat::RGBA8:
        return 4;
    default:
        return 1;
    }
}

int ceilDiv(int a, int b) {
    return (a + b - 1) / b;
}

template <class T>
void boxPlane(const media::Plane& in, std::uint8_t* out, int outStride, int outWidth, int outHeight, int channels,
              int d) {
    for (int y = 0; y < outHeight; ++y) {
        const int y0 = y * d;
        const int y1 = std::min(y0 + d, in.height);
        T* dst = reinterpret_cast<T*>(out + static_cast<std::ptrdiff_t>(y) * outStride);
        for (int x = 0; x < outWidth; ++x) {
            const int x0 = x * d;
            const int x1 = std::min(x0 + d, in.width);
            const std::uint32_t count = static_cast<std::uint32_t>((y1 - y0) * (x1 - x0));
            for (int c = 0; c < channels; ++c) {
                std::uint32_t sum = 0;
                for (int yy = y0; yy < y1; ++yy) {
                    const T* row = reinterpret_cast<const T*>(in.data + static_cast<std::ptrdiff_t>(yy) * in.stride);
                    for (int xx = x0; xx < x1; ++xx) {
                        sum += row[xx * channels + c];
                    }
                }
                dst[x * channels + c] = static_cast<T>((sum + count / 2) / count);
            }
        }
    }
}

} // namespace

media::FrameHandle downscaleFrame(const media::FrameHandle& frame, int divisor) {
    const media::CpuPlanes* cpu = frame.cpu();
    if (!cpu || (divisor != 2 && divisor != 4) || !frame.isValid()) {
        return frame;
    }
    const int planes = frame.planeCount();
    struct Layout {
        int width = 0;
        int height = 0;
        int stride = 0;
        std::size_t offset = 0;
    };
    std::array<Layout, 4> layout{};
    std::size_t total = 0;
    for (int p = 0; p < planes; ++p) {
        const media::Plane& in = cpu->planes[static_cast<std::size_t>(p)];
        Layout& l = layout[static_cast<std::size_t>(p)];
        l.width = std::max(1, ceilDiv(in.width, divisor));
        l.height = std::max(1, ceilDiv(in.height, divisor));
        const int bytes = l.width * media::pixelFormatBytesPerTexel(frame.format, p);
        l.stride = (bytes + 63) & ~63;
        l.offset = total;
        total += static_cast<std::size_t>(l.stride) * static_cast<std::size_t>(l.height);
    }
    std::shared_ptr<std::uint8_t[]> storage(new std::uint8_t[total + 64]);
    std::uint8_t* base = storage.get();
    base += (64 - reinterpret_cast<std::uintptr_t>(base) % 64) % 64;

    media::FrameHandle out = frame;
    media::CpuPlanes planesOut;
    planesOut.device = cpu->device;
    for (int p = 0; p < planes; ++p) {
        const media::Plane& in = cpu->planes[static_cast<std::size_t>(p)];
        const Layout& l = layout[static_cast<std::size_t>(p)];
        std::uint8_t* dst = base + l.offset;
        const int channels = channelsOf(frame.format, p);
        const int sampleBytes = media::pixelFormatBytesPerTexel(frame.format, p) / channels;
        if (sampleBytes == 2) {
            boxPlane<std::uint16_t>(in, dst, l.stride, l.width, l.height, channels, divisor);
        } else {
            boxPlane<std::uint8_t>(in, dst, l.stride, l.width, l.height, channels, divisor);
        }
        planesOut.planes[static_cast<std::size_t>(p)] = {dst, l.stride, l.width, l.height};
    }
    planesOut.owner = std::shared_ptr<const void>(storage, storage.get());
    out.payload = std::move(planesOut);
    out.width = std::max(1, ceilDiv(frame.width, divisor));
    out.height = std::max(1, ceilDiv(frame.height, divisor));
    // Keep the displayed shape: width * aspect / height is unchanged.
    const std::int64_t num = static_cast<std::int64_t>(frame.sampleAspect.isValid() ? frame.sampleAspect.num : 1) *
                             frame.width * out.height;
    const std::int64_t den = static_cast<std::int64_t>(frame.sampleAspect.isValid() ? frame.sampleAspect.den : 1) *
                             out.width * frame.height;
    const std::int64_t g = std::max<std::int64_t>(1, std::gcd(num, den));
    std::int64_t n = num / g;
    std::int64_t dd = den / g;
    while (n > INT32_MAX || dd > INT32_MAX) { // never for real sizes; keeps the cast safe
        n /= 2;
        dd /= 2;
    }
    out.sampleAspect = Rate{static_cast<std::int32_t>(std::max<std::int64_t>(1, n)),
                            static_cast<std::int32_t>(std::max<std::int64_t>(1, dd))};
    return out;
}

int extraDivisor(int streamHeight, int frameHeight, int targetDivisor) {
    if (targetDivisor <= 1 || streamHeight <= 0 || frameHeight <= 0) {
        return 1;
    }
    const int target = ceilDiv(streamHeight, targetDivisor);
    int d = 1;
    while (d < 4 && ceilDiv(frameHeight, d * 2) >= target) {
        d *= 2;
    }
    return d;
}

} // namespace wabcut::playback::detail
