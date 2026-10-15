#include <wabcut/media/FfmpegInfo.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
}

namespace wabcut::media {

namespace {
std::string versionTriple(unsigned v) {
    return std::to_string(AV_VERSION_MAJOR(v)) + "." + std::to_string(AV_VERSION_MINOR(v)) + "." +
           std::to_string(AV_VERSION_MICRO(v));
}
} // namespace

std::string ffmpegVersionString() {
    return "libavcodec " + versionTriple(avcodec_version()) + " / libavformat " +
           versionTriple(avformat_version());
}

std::vector<std::string> compiledHwDeviceTypes() {
    std::vector<std::string> types;
    for (AVHWDeviceType t = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE); t != AV_HWDEVICE_TYPE_NONE;
         t = av_hwdevice_iterate_types(t)) {
        types.emplace_back(av_hwdevice_get_type_name(t));
    }
    return types;
}

} // namespace wabcut::media
