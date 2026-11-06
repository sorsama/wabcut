#include <wabcut/export/Encoders.h>
#include <wabcut/media/VideoDecoder.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wabcut::exporter {

namespace {

std::mutex& orderMutex() {
    static std::mutex m;
    return m;
}

std::optional<std::vector<std::string>>& orderOverride() {
    static std::optional<std::vector<std::string>> names;
    return names;
}

std::string_view codecPrefix(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264:
        return "h264_";
    case VideoCodec::Hevc:
        return "hevc_";
    case VideoCodec::ProRes:
        return "prores_";
    case VideoCodec::DnxHR:
        return "dnxhd_";
    case VideoCodec::Av1:
        return "av1_";
    case VideoCodec::Gif:
        return "gif_";
    case VideoCodec::Vp9:
        return "vp9_";
    case VideoCodec::Png:
    case VideoCodec::Tiff:
    case VideoCodec::Dpx:
    case VideoCodec::Jpeg:
    case VideoCodec::Targa:
    case VideoCodec::Exr:
    case VideoCodec::Animation:
    case VideoCodec::CineForm:
    case VideoCodec::Xdcam: // 0.6 (#73)
    case VideoCodec::AvcIntra:
        break; // 0.2: software only
    }
    return "none_";
}

bool isSoftware(std::string_view name) {
    return name == "libx264" || name == "libx265" || name == "prores_ks" || name == "prores_aw" ||
           name == "prores" || name == "dnxhd" || name == "libx264rgb" || name == "libsvtav1" || name == "libaom-av1" ||
           name == "librav1e" || name == "gif" || name == "libvpx-vp9" || name == "libvpx" || name == "png" ||
           name == "tiff" || name == "dpx" || name == "mjpeg" || name == "targa" || name == "exr" || name == "qtrle" ||
           name == "cfhd" || name == "mpeg2video";
}

/// Hardware encoder names that belong to `codec` ("h264_nvenc" for H264 …); software names are dropped.
std::vector<std::string> filterFor(VideoCodec codec, const std::vector<std::string>& names) {
    std::vector<std::string> out;
    const std::string_view prefix = codecPrefix(codec);
    for (const std::string& n : names) {
        if (n.starts_with(prefix) && !isSoftware(n) && std::find(out.begin(), out.end(), n) == out.end()) {
            out.push_back(n);
        }
    }
    return out;
}

std::vector<std::string> parseList(std::string_view text) {
    std::vector<std::string> names;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find(',', start), text.size());
        std::string_view item = text.substr(start, end - start);
        while (!item.empty() && item.front() == ' ') {
            item.remove_prefix(1);
        }
        while (!item.empty() && item.back() == ' ') {
            item.remove_suffix(1);
        }
        if (!item.empty()) {
            names.emplace_back(item);
        }
        start = end + 1;
    }
    return names;
}

std::optional<std::string> environment(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string text(value);
    std::free(value);
    return text;
#else
    const char* value = std::getenv(name);
    return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

std::vector<std::string> policy(VideoCodec codec) {
    if (codec == VideoCodec::DnxHR || codecPrefix(codec) == "none_") {
        return {};
    }
#if defined(__APPLE__)
    return {std::string(codecPrefix(codec)) + "videotoolbox"};
#else
    if (codec == VideoCodec::ProRes || codec == VideoCodec::Gif) {
        return {};
    }
    std::vector<std::string> names;
    const std::string prefix(codecPrefix(codec));
    for (const std::string& vendor : media::gpuVendors()) {
        if (codec == VideoCodec::Vp9 && vendor != "Intel") {
            continue; // only Quick Sync (and VA-API) encode VP9
        }
        if (vendor == "NVIDIA") {
            names.push_back(prefix + "nvenc");
        }
#if defined(_WIN32)
        else if (vendor == "AMD") {
            names.push_back(prefix + "amf");
        } else if (vendor == "Intel") {
            names.push_back(prefix + "qsv");
        }
#else
        else if (vendor == "AMD" || vendor == "Intel") {
            names.push_back(prefix + "vaapi");
        }
#endif
    }
    return filterFor(codec, names);
#endif
}

} // namespace

std::vector<std::string> hardwareEncoderOrder(VideoCodec codec) {
    {
        std::lock_guard lock(orderMutex());
        if (orderOverride()) {
            return filterFor(codec, *orderOverride());
        }
    }
    if (const auto env = environment("WABCUT_HW_ENCODERS")) {
        return filterFor(codec, parseList(*env));
    }
    return policy(codec);
}

void setHardwareEncoderOrder(std::vector<std::string> encoders) {
    std::lock_guard lock(orderMutex());
    if (encoders.empty()) {
        orderOverride().reset();
    } else {
        orderOverride() = std::move(encoders);
    }
}

std::string_view softwareEncoder(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264:
        return "libx264";
    case VideoCodec::Hevc:
        return "libx265";
    case VideoCodec::ProRes:
        return "prores_ks";
    case VideoCodec::DnxHR:
        return "dnxhd";
    case VideoCodec::Av1:
        return "libsvtav1";
    case VideoCodec::Gif:
        return "gif";
    case VideoCodec::Vp9:
        return "libvpx-vp9";
    case VideoCodec::Png:
        return "png";
    case VideoCodec::Tiff:
        return "tiff";
    case VideoCodec::Dpx:
        return "dpx";
    case VideoCodec::Jpeg:
        return "mjpeg";
    case VideoCodec::Targa:
        return "targa";
    case VideoCodec::Exr:
        return "exr";
    case VideoCodec::Animation:
        return "qtrle";
    case VideoCodec::CineForm:
        return "cfhd";
    case VideoCodec::Xdcam: // 0.6 (#73): XDCAM HD422 (MPEG-2 4:2:2P@HL)
        return "mpeg2video";
    case VideoCodec::AvcIntra: // libx264's SMPTE RP 2027 AVC-Intra classes
        return "libx264";
    }
    return "libx264";
}

namespace {
std::atomic<int> g_threadLimit{0};
} // namespace

void setExportThreadLimit(int threads) {
    g_threadLimit = std::clamp(threads, 0, 256);
}

namespace {
thread_local int t_threadLimit = 0; // ScopedExportThreadLimit: 0 = none
} // namespace

int exportThreadLimit() {
    const int global = g_threadLimit.load();
    if (t_threadLimit <= 0) {
        return global;
    }
    return global > 0 ? std::min(global, t_threadLimit) : t_threadLimit;
}

ScopedExportThreadLimit::ScopedExportThreadLimit(int threads) : m_previous(t_threadLimit) {
    t_threadLimit = std::clamp(threads, 0, 256);
}

ScopedExportThreadLimit::~ScopedExportThreadLimit() {
    t_threadLimit = m_previous;
}

namespace {
thread_local bool t_backgroundMode = false; // ScopedExportBackgroundMode
} // namespace

ScopedExportBackgroundMode::ScopedExportBackgroundMode() : m_previous(t_backgroundMode) {
    t_backgroundMode = true;
}

ScopedExportBackgroundMode::~ScopedExportBackgroundMode() {
    t_backgroundMode = m_previous;
}

bool exportBackgroundMode() {
    return t_backgroundMode;
}

BackgroundThreadMode::BackgroundThreadMode(bool on) {
#ifdef _WIN32
    // THREAD_MODE_BACKGROUND_BEGIN lowers the CPU, I/O and memory priority of the calling thread only.
    m_active = on && SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != 0;
#else
    (void)on;
#endif
}

BackgroundThreadMode::~BackgroundThreadMode() {
#ifdef _WIN32
    if (m_active) {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
#endif
}

int threadLimitForPercent(int percent) {
    if (percent >= 100 || percent <= 0) {
        return 0;
    }
    const int cores = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    return std::max(1, cores * percent / 100);
}

std::vector<std::string> encoderCandidates(VideoCodec codec) {
    std::vector<std::string> names = hardwareEncoderOrder(codec);
    names.emplace_back(softwareEncoder(codec));
    return names;
}

} // namespace wabcut::exporter
