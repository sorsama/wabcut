#include <wabcut/editor/Tracking.h>

#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace wabcut::editor {

namespace {

/// The stored frame's luma, full size, 0..1.
masks::GrayImage fullLuma(const media::FrameHandle& frame) {
    masks::GrayImage out(frame.width, frame.height);
    const media::CpuPlanes* cpu = frame.cpu();
    if (!cpu) {
        return {};
    }
    const media::Plane& p = cpu->planes[0];
    const auto rowOf = [&](int y) { return p.data + static_cast<std::ptrdiff_t>(y) * p.stride; };
    float* dst = out.px.data();
    switch (frame.format) {
    case media::PixelFormat::NV12:
    case media::PixelFormat::YUV420P:
    case media::PixelFormat::YUV422P:
    case media::PixelFormat::YUV444P:
    case media::PixelFormat::YUVA420P:
        for (int y = 0; y < frame.height; ++y) {
            const std::uint8_t* row = rowOf(y);
            for (int x = 0; x < frame.width; ++x) {
                *dst++ = row[x] / 255.0f;
            }
        }
        break;
    case media::PixelFormat::P010:
        for (int y = 0; y < frame.height; ++y) {
            const auto* row = reinterpret_cast<const std::uint16_t*>(rowOf(y));
            for (int x = 0; x < frame.width; ++x) {
                *dst++ = row[x] / 65535.0f;
            }
        }
        break;
    case media::PixelFormat::YUV420P10:
    case media::PixelFormat::YUV422P10:
    case media::PixelFormat::YUV444P10:
    case media::PixelFormat::YUVA444P10:
        for (int y = 0; y < frame.height; ++y) {
            const auto* row = reinterpret_cast<const std::uint16_t*>(rowOf(y));
            for (int x = 0; x < frame.width; ++x) {
                *dst++ = row[x] / 1023.0f;
            }
        }
        break;
    case media::PixelFormat::RGBA8:
        for (int y = 0; y < frame.height; ++y) {
            const std::uint8_t* row = rowOf(y);
            for (int x = 0; x < frame.width; ++x) {
                *dst++ = (0.2126f * row[4 * x] + 0.7152f * row[4 * x + 1] + 0.0722f * row[4 * x + 2]) / 255.0f;
            }
        }
        break;
    case media::PixelFormat::Unknown:
        return {};
    }
    return out;
}

} // namespace

masks::GrayImage lumaOf(const media::FrameHandle& frame, int width) {
    const masks::GrayImage full = fullLuma(frame);
    if (full.empty()) {
        return {};
    }
    const double par = frame.sampleAspect.isValid() ? frame.sampleAspect.toDouble() : 1.0;
    const double displayWidth = full.width * par;
    const int w = std::clamp(width, 16, std::max(16, static_cast<int>(std::lround(displayWidth))));
    const int h = std::max(9, static_cast<int>(std::lround(static_cast<double>(w) * full.height / displayWidth)));
    if (w == full.width && h == full.height) {
        return full;
    }
    // Area average over each output pixel's footprint (integer bounds; at least one source pixel).
    masks::GrayImage out(w, h);
    const double fx = static_cast<double>(full.width) / w;
    const double fy = static_cast<double>(full.height) / h;
    std::vector<int> x0(static_cast<std::size_t>(w)), x1(static_cast<std::size_t>(w));
    for (int x = 0; x < w; ++x) {
        x0[static_cast<std::size_t>(x)] = std::min(full.width - 1, static_cast<int>(std::floor(x * fx)));
        x1[static_cast<std::size_t>(x)] = std::max(x0[static_cast<std::size_t>(x)] + 1, std::min(full.width, static_cast<int>(std::floor((x + 1) * fx))));
    }
    for (int y = 0; y < h; ++y) {
        const int y0 = std::min(full.height - 1, static_cast<int>(std::floor(y * fy)));
        const int y1 = std::max(y0 + 1, std::min(full.height, static_cast<int>(std::floor((y + 1) * fy))));
        for (int x = 0; x < w; ++x) {
            double sum = 0.0;
            for (int sy = y0; sy < y1; ++sy) {
                const float* row = full.px.data() + static_cast<std::size_t>(sy) * full.width;
                for (int sx = x0[static_cast<std::size_t>(x)]; sx < x1[static_cast<std::size_t>(x)]; ++sx) {
                    sum += row[sx];
                }
            }
            out.px[static_cast<std::size_t>(y) * w + x] =
                static_cast<float>(sum / ((y1 - y0) * (x1[static_cast<std::size_t>(x)] - x0[static_cast<std::size_t>(x)])));
        }
    }
    return out;
}

MediaTrackingService::MediaTrackingService(std::filesystem::path projectFile) : m_projectFile(std::move(projectFile)) {}

MediaTrackingService::~MediaTrackingService() = default;

std::expected<masks::TrackingFrame, Error> MediaTrackingService::frame(const ProjectState& state, MediaId media, int stream,
                                                                       Ticks time, int maxWidth) {
    const MediaItem* item = state.findMedia(media);
    if (!item || stream < 0 || stream >= static_cast<int>(item->info.video.size())) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such video stream"});
    }
    // The original when it is there, else its proxy (frame for frame the same source).
    std::string path;
    if (const auto found = resolveMediaPath(item->locator, m_projectFile)) {
        path = pathToUtf8(*found);
    } else if (item->locator.proxy && std::filesystem::is_regular_file(pathFromUtf8(item->locator.proxy->path))) {
        path = item->locator.proxy->path;
    } else {
        return std::unexpected(Error{ErrorCode::NotFound, "the media file is offline and has no proxy: " + item->name});
    }
    std::lock_guard lock(m_mutex);
    Open& open = m_open[{media.value, stream}];
    if (!open.decoder || open.path != path) {
        media::DecoderOptions options;
        options.allowHardware = true;
        auto decoder = media::VideoDecoder::open(path, item->info.video[static_cast<std::size_t>(stream)].index, options);
        if (!decoder) {
            m_open.erase({media.value, stream});
            return std::unexpected(decoder.error());
        }
        open.decoder = std::move(*decoder);
        open.path = path;
    }
    auto decoded = open.decoder->frameAt(time);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    masks::TrackingFrame out;
    out.luma = lumaOf(*decoded, maxWidth);
    if (out.luma.empty()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "the decoder delivered a picture the tracker cannot read"});
    }
    out.start = decoded->pts;
    out.duration = decoded->duration;
    return out;
}

void MediaTrackingService::progress(const masks::TrackProgress& progress) {
    std::function<void(const masks::TrackProgress&)> callback;
    {
        std::lock_guard lock(m_mutex);
        callback = m_progress;
    }
    if (callback) {
        callback(progress);
    }
}

void MediaTrackingService::setProgressCallback(std::function<void(const masks::TrackProgress&)> callback) {
    std::lock_guard lock(m_mutex);
    m_progress = std::move(callback);
}

} // namespace wabcut::editor
