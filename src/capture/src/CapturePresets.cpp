#include <wabcut/capture/CapturePresets.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace wabcut::capture {

namespace {

std::vector<CapturePreset> makePresets() {
    std::vector<CapturePreset> presets;

    CapturePreset high;
    high.id = "high";
    high.label = "High quality (H.264)";
    high.description = "A good picture in files any editor reads. Uses the graphics card's encoder when there is one.";
    high.video.codec = exporter::VideoCodec::H264;
    high.video.encoder = "auto";
    high.video.rateControl = exporter::RateControl::Quality;
    high.video.quality = 18;
    high.video.pixelFormat = exporter::OutputPixelFormat::Yuv420p;
    high.video.keyframeInterval = Ticks::fromSeconds(1);
    high.audio.codec = exporter::AudioCodec::Pcm16;
    high.bitsPerPixel = 0.2;
    presets.push_back(high);

    CapturePreset compact = high;
    compact.id = "compact";
    compact.label = "Compact (H.264)";
    compact.description = "About half the size of High quality, for long recordings.";
    compact.video.quality = 24;
    compact.bitsPerPixel = 0.1;
    presets.push_back(compact);

    CapturePreset intra;
    intra.id = "intra";
    intra.label = "Every frame whole (ProRes 422)";
    intra.description = "For tape and for material that will be cut hard or graded. Large files: about 65 GB an hour at 1080p30, 12 GB at standard definition.";
    intra.video.codec = exporter::VideoCodec::ProRes;
    intra.video.encoder = "prores_ks";
    intra.video.proresProfile = exporter::ProResProfile::Standard;
    intra.video.pixelFormat = exporter::OutputPixelFormat::Yuv422p10;
    intra.audio.codec = exporter::AudioCodec::Pcm16;
    intra.bitsPerPixel = 2.4;
    presets.push_back(intra);
    return presets;
}

} // namespace

const std::vector<CapturePreset>& capturePresets() {
    static const std::vector<CapturePreset> presets = makePresets();
    return presets;
}

const CapturePreset& capturePreset(std::string_view id) {
    const auto& all = capturePresets();
    const auto it = std::ranges::find(all, id, &CapturePreset::id);
    return it != all.end() ? *it : all.front();
}

std::int64_t estimateBytesPerSecond(const CapturePreset& preset, const VideoFormat& format, bool audio, int audioChannels,
                                    int audioRate) {
    double bytes = 0.0;
    if (format.width > 0 && format.height > 0 && format.fps() > 0.0) {
        bytes += static_cast<double>(format.width) * format.height * format.fps() * preset.bitsPerPixel / 8.0;
    }
    if (audio && audioChannels > 0 && audioRate > 0) {
        bytes += static_cast<double>(audioChannels) * audioRate * 2.0; // 16-bit PCM
    }
    return static_cast<std::int64_t>(std::llround(bytes));
}

std::string durationText(double seconds) {
    if (!(seconds >= 0.0)) {
        return {};
    }
    const auto total = static_cast<std::int64_t>(seconds);
    if (total < 60) {
        return std::format("{} s", total);
    }
    const std::int64_t minutes = total / 60;
    if (minutes < 60) {
        return std::format("{} min", minutes);
    }
    const std::int64_t hours = minutes / 60;
    if (hours >= 100) {
        return std::format("{} h", hours);
    }
    const std::int64_t rest = minutes % 60;
    return rest == 0 ? std::format("{} h", hours) : std::format("{} h {} min", hours, rest);
}

} // namespace wabcut::capture
