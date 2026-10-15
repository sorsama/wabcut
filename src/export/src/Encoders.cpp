#include <wabcut/export/Encoders.h>
#include <wabcut/media/VideoDecoder.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <optional>

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
    }
    return "h264_";
}

bool isSoftware(std::string_view name) {
    return name == "libx264" || name == "libx265" || name == "prores_ks" || name == "prores_aw" ||
           name == "prores" || name == "dnxhd" || name == "libx264rgb" || name == "libsvtav1" || name == "libaom-av1" ||
           name == "librav1e" || name == "gif" || name == "libvpx-vp9" || name == "libvpx";
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
    if (codec == VideoCodec::DnxHR) {
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
    }
    return "libx264";
}

std::vector<std::string> encoderCandidates(VideoCodec codec) {
    std::vector<std::string> names = hardwareEncoderOrder(codec);
    names.emplace_back(softwareEncoder(codec));
    return names;
}

} // namespace wabcut::exporter
