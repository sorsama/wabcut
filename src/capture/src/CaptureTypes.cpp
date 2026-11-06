#include <wabcut/capture/CaptureTypes.h>

#include <format>

namespace wabcut::capture {

std::string_view pixelFormatName(PixelFormat format) {
    switch (format) {
    case PixelFormat::Nv12:
        return "NV12";
    case PixelFormat::Yuy2:
        return "YUY2";
    case PixelFormat::Uyvy:
        return "UYVY";
    case PixelFormat::I420:
        return "I420";
    case PixelFormat::P010:
        return "P010";
    case PixelFormat::Bgr24:
        return "RGB24";
    case PixelFormat::Bgra:
        return "RGB32";
    }
    return "?";
}

int planeCount(PixelFormat format) {
    switch (format) {
    case PixelFormat::Nv12:
    case PixelFormat::P010:
        return 2;
    case PixelFormat::I420:
        return 3;
    case PixelFormat::Yuy2:
    case PixelFormat::Uyvy:
    case PixelFormat::Bgr24:
    case PixelFormat::Bgra:
        break;
    }
    return 1;
}

int planeStride(PixelFormat format, int width, int plane) {
    const int half = (width + 1) / 2;
    switch (format) {
    case PixelFormat::Nv12:
        return width;
    case PixelFormat::P010:
        return 2 * width;
    case PixelFormat::I420:
        return plane == 0 ? width : half;
    case PixelFormat::Yuy2:
    case PixelFormat::Uyvy:
        return 2 * half * 2;
    case PixelFormat::Bgr24:
        return 3 * width;
    case PixelFormat::Bgra:
        return 4 * width;
    }
    return width;
}

int planeRows(PixelFormat format, int height, int plane) {
    switch (format) {
    case PixelFormat::Nv12:
    case PixelFormat::P010:
    case PixelFormat::I420:
        return plane == 0 ? height : (height + 1) / 2;
    case PixelFormat::Yuy2:
    case PixelFormat::Uyvy:
    case PixelFormat::Bgr24:
    case PixelFormat::Bgra:
        break;
    }
    return height;
}

std::size_t frameBytes(PixelFormat format, int width, int height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    std::size_t total = 0;
    for (int p = 0; p < planeCount(format); ++p) {
        total += static_cast<std::size_t>(planeStride(format, width, p)) * static_cast<std::size_t>(planeRows(format, height, p));
    }
    return total;
}

std::string VideoFormat::label() const {
    const double rate = fps();
    // 59.94, 29.97, 23.976 keep their decimals; 30, 60, 25 show whole.
    std::string rateText = std::format("{:.3f}", rate);
    while (!rateText.empty() && rateText.back() == '0') {
        rateText.pop_back();
    }
    if (!rateText.empty() && rateText.back() == '.') {
        rateText.pop_back();
    }
    std::string text = std::format("{} x {} · {} fps", width, height, rateText);
    if (!nativeName.empty()) {
        text += " · " + nativeName;
    }
    if (interlaced) {
        text += " · interlaced";
    }
    return text;
}

const std::uint8_t* CaptureFrame::plane(int index) const {
    if (!data || index < 0 || index >= planeCount(pixel)) {
        return nullptr;
    }
    std::size_t offset = 0;
    for (int p = 0; p < index; ++p) {
        offset += static_cast<std::size_t>(planeStride(pixel, width, p)) * static_cast<std::size_t>(planeRows(pixel, height, p));
    }
    return data->data() + offset;
}

} // namespace wabcut::capture
