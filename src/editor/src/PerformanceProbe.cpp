#include <wabcut/editor/PerformanceProbe.h>

#include <wabcut/audio/AudioEngine.h>
#include <wabcut/export/Encoders.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/export/VideoFrameSource.h>
#include <wabcut/media/GpuInfo.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectOps.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <stop_token>
#include <system_error>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wabcut::editor {

namespace {

using Clock = std::chrono::steady_clock;

struct Sample {
    const char* name;
    const char* file;
    exporter::VideoCodec codec;
    exporter::Container container;
};

constexpr Sample kSamples[] = {
    {"H.264", "sample-h264.mp4", exporter::VideoCodec::H264, exporter::Container::Mp4},
    {"HEVC", "sample-hevc.mp4", exporter::VideoCodec::Hevc, exporter::Container::Mp4},
    {"ProRes", "sample-prores.mov", exporter::VideoCodec::ProRes, exporter::Container::Mov},
};

double secondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

bool isHardwareEncoder(std::string_view encoder) {
    return !encoder.empty() && !encoder.starts_with("lib") && encoder != "prores_ks" && encoder != "dnxhd";
}

/// Writes a `frames`-frame sample of `sample` with the export path (the synthetic test pattern, no audio): hardware
/// encoders first for H.264 / HEVC, so making it is quick where a GPU encoder exists.
std::expected<std::filesystem::path, Error> makeSample(const Sample& sample, const ProbeOptions& options, int frames,
                                                       const std::filesystem::path& dir, std::stop_token stop) {
    edit::SequenceSettings settings;
    settings.width = options.width;
    settings.height = options.height;
    settings.frameRate = options.frameRate;
    auto created = edit::addSequence(ProjectState{}, settings);
    if (!created) {
        return std::unexpected(created.error());
    }
    exporter::ExportRequest request;
    request.snapshot = std::make_shared<const ProjectState>(std::move(created->state));
    request.sequence = created->id;
    request.settings.container = sample.container;
    request.settings.video.codec = sample.codec;
    request.settings.video.encoder = "auto";
    if (sample.codec == exporter::VideoCodec::ProRes) {
        request.settings.video.proresProfile = exporter::ProResProfile::Proxy;
        request.settings.video.pixelFormat = exporter::OutputPixelFormat::Yuv422p10;
    }
    request.settings.audio.enabled = false;
    request.settings.range = exporter::RangeMode::Custom;
    request.settings.customRange = TimeRange{Ticks{}, framesToTicks(frames, options.frameRate)};
    request.output = dir / sample.file;
    request.videoSource = exporter::syntheticVideoSourceFactory();
    auto result = exporter::ExportJob(std::move(request)).run(stop);
    if (!result) {
        return std::unexpected(result.error());
    }
    return result->output;
}

/// Frames per second decoding `file` front to back with `decoderOptions` (the first frame, which pays for opening the
/// decoder, is not timed). `backend` gets the backend that decoded it.
double decodeFps(const std::filesystem::path& file, const media::DecoderOptions& decoderOptions, std::string& backend,
                 double maxSeconds) {
    auto decoder = media::VideoDecoder::open(pathToUtf8(file), -1, decoderOptions);
    if (!decoder) {
        return 0.0;
    }
    media::VideoDecoder& d = **decoder;
    backend = std::string(d.backendName());
    const Rate rate = d.streamInfo().frameRate.num > 0 ? d.streamInfo().frameRate : Rate{30, 1};
    const Ticks first = d.firstFrameTime();
    if (!d.frameAt(first)) {
        return 0.0;
    }
    const auto start = Clock::now();
    int decoded = 0;
    for (std::int64_t k = 1;; ++k) {
        const auto t = framesToTicks(k, rate, Rounding::Floor);
        if (!t || !d.frameAt(first + *t)) {
            break; // past the end
        }
        ++decoded;
        if (secondsSince(start) > maxSeconds) {
            break;
        }
    }
    const double elapsed = secondsSince(start);
    return decoded > 0 && elapsed > 0.0 ? decoded / elapsed : 0.0;
}

std::filesystem::path freshWorkDir() {
    std::error_code ec;
    std::mt19937_64 random(std::random_device{}());
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    return base / ("wabcut-performance-" + std::to_string(random() % 1'000'000'000));
}

} // namespace

PerformanceReport runPerformanceProbe(const ProbeOptions& options, std::stop_token stop,
                                      const std::function<void(double, std::string_view)>& onProgress) {
    const auto start = Clock::now();
    const auto remaining = [&] { return options.budgetSeconds - secondsSince(start); };
    const auto progress = [&](double fraction, std::string_view step) {
        if (onProgress) {
            onProgress(std::clamp(fraction, 0.0, 1.0), step);
        }
    };
    PerformanceReport report;
    report.width = options.width;
    report.height = options.height;
    report.frameRate = options.frameRate.toDouble();
    report.cpuThreads = static_cast<int>(std::thread::hardware_concurrency());
    constexpr const char* kOutOfTime = "skipped: the check's time ran out";

    progress(0.0, "Graphics");
    report.gpus = media::gpuVendors();
    // Issue #10: which GPU, and why (the Settings choice, the Windows Graphics setting, or the high-performance one).
    if (const media::GpuSelection gpu = media::selectedGpu(); gpu.adapter) {
        report.gpuName = gpu.adapter->name;
        report.gpuReason = std::string(media::gpuReasonKey(gpu.reason));
        if (const media::GpuSelection preview = media::previewGpu(); preview.adapter && preview.adapter->key != gpu.adapter->key) {
            report.previewGpuName = preview.adapter->name;
        }
    }
    for (const media::GpuAdapter& a : media::gpuAdapters()) {
        report.gpuNames.push_back(a.name);
    }

    if (options.audio && !stop.stop_requested()) {
        progress(0.04, "Audio output");
        if (auto devices = audio::AudioEngine::enumerateOutputDevices(); devices && !devices->empty()) {
            report.audioDevice = true;
            const auto def = std::ranges::find_if(*devices, [](const audio::AudioDeviceInfo& d) { return d.isDefault; });
            report.audioName = def != devices->end() ? def->name : devices->front().name;
        } else {
            report.audioNote = devices ? "no output device" : devices.error().message;
        }
    }

    // Export encoders: what an export would really use (each hardware candidate test-encodes).
    for (const auto& [name, codec] : {std::pair{"H.264", exporter::VideoCodec::H264}, std::pair{"HEVC", exporter::VideoCodec::Hevc}}) {
        if (stop.stop_requested()) {
            break;
        }
        progress(0.08 + (codec == exporter::VideoCodec::Hevc ? 0.05 : 0.0), std::string("Export encoder: ") + name);
        EncoderMeasurement e;
        e.codec = name;
        if (remaining() < 0.5) {
            e.note = kOutOfTime;
            report.outOfTime = true;
        } else {
            exporter::VideoSettings video;
            video.codec = codec;
            video.encoder = "auto";
            if (auto probe = exporter::probeVideoEncoder(video, options.width, options.height, options.frameRate)) {
                e.encoder = probe->encoder;
                e.hardware = isHardwareEncoder(e.encoder);
            } else {
                e.note = probe.error().message;
            }
        }
        report.encoders.push_back(std::move(e));
    }

    // Decoding: make each sample, then time it through the hardware backend and in software.
    const std::filesystem::path dir = options.workDir.empty() ? freshWorkDir() : options.workDir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    int index = 0;
    for (const Sample& sample : kSamples) {
        const double base = 0.18 + 0.27 * index++;
        DecodeMeasurement m;
        m.codec = sample.name;
        if (stop.stop_requested()) {
            break;
        }
        // Each sample needs a couple of seconds; HEVC in software is the slowest to make, so it gets half the frames.
        if (remaining() < 1.5) {
            m.note = kOutOfTime;
            report.outOfTime = true;
            report.decode.push_back(std::move(m));
            continue;
        }
        progress(base, std::string("Making a ") + sample.name + " sample");
        const bool slowEncoder =
            sample.codec == exporter::VideoCodec::Hevc &&
            std::ranges::none_of(report.encoders, [](const EncoderMeasurement& e) { return e.codec == "HEVC" && e.hardware; });
        const int frames = slowEncoder ? std::max(6, options.frames / 2) : options.frames;
        auto file = makeSample(sample, options, frames, dir, stop);
        if (!file) {
            m.note = stop.stop_requested() ? "cancelled" : "no sample: " + file.error().message;
            report.decode.push_back(std::move(m));
            continue;
        }
        m.available = true;
        const double slice = std::max(0.3, std::min(2.0, remaining() / 4.0));
        progress(base + 0.12, std::string("Decoding ") + sample.name + " (hardware)");
        media::DecoderOptions hardware;
        hardware.allowHardware = true;
        std::string backend;
        const double hwFps = decodeFps(*file, hardware, backend, slice);
        if (backend != "software" && !backend.empty()) {
            m.hardwareBackend = backend;
            m.hardwareFps = hwFps;
        }
        if (stop.stop_requested()) {
            report.decode.push_back(std::move(m));
            break;
        }
        progress(base + 0.2, std::string("Decoding ") + sample.name + " (software)");
        if (remaining() > 0.3) {
            media::DecoderOptions software;
            software.allowHardware = false;
            software.preferredBackend = "software";
            std::string softwareBackend;
            m.softwareFps = backend == "software" ? hwFps : decodeFps(*file, software, softwareBackend, slice);
        } else {
            m.note = kOutOfTime;
            report.outOfTime = true;
        }
        report.decode.push_back(std::move(m));
        std::filesystem::remove(*file, ec);
    }
    if (options.workDir.empty()) {
        std::filesystem::remove_all(dir, ec);
    }
    report.cancelled = stop.stop_requested();
    report.seconds = secondsSince(start);
    report.recommendation = recommend(report);
    progress(1.0, "Done");
    return report;
}

ReadinessRecommendation recommend(const PerformanceReport& report) {
    ReadinessRecommendation r;
    const auto find = [&](std::string_view codec) -> const DecodeMeasurement* {
        const auto it = std::ranges::find_if(report.decode, [&](const DecodeMeasurement& m) { return m.codec == codec; });
        return it != report.decode.end() && it->available ? &*it : nullptr;
    };
    const double realTime = report.frameRate > 0.0 ? report.frameRate : 30.0;
    // How many times real time the harder of the long-GOP codecs decodes (1080p by default).
    double factor = -1.0;
    for (const char* codec : {"H.264", "HEVC"}) {
        if (const DecodeMeasurement* m = find(codec); m && m->bestFps() > 0.0) {
            const double f = m->bestFps() / realTime;
            factor = factor < 0.0 ? f : std::min(factor, f);
        }
    }
    const std::string size = std::to_string(report.width) + "x" + std::to_string(report.height);
    char measured[64];
    std::snprintf(measured, sizeof measured, "%.1fx", std::max(factor, 0.0));
    if (factor < 0.0) {
        r.reasons.push_back("Decoding could not be measured: the usual settings are kept (proxies when media needs "
                            "them, preview quality Auto).");
    } else if (factor >= 2.0) {
        r.reasons.push_back("This computer decodes " + size + " H.264 and HEVC at " + measured +
                            " real time or better: full-quality preview, proxies only for media that needs them.");
    } else if (factor >= 1.0) {
        r.previewQuality = "half";
        r.proxyDivisor = 4;
        r.reasons.push_back("Decoding " + size + " runs at " + measured +
                            " real time: preview at 1/2 resolution and quarter-size proxies keep playback smooth.");
    } else {
        r.proxyPolicy = "always";
        r.previewQuality = "quarter";
        r.proxyDivisor = 4;
        r.reasons.push_back("Decoding " + size + " is slower than real time (" + measured +
                            "): proxies for every video and a 1/4 preview.");
    }
    const DecodeMeasurement* h264 = find("H.264");
    const bool gpuH264Encoder = std::ranges::any_of(report.encoders, [](const EncoderMeasurement& e) {
        return e.codec == "H.264" && e.hardware;
    });
    if (gpuH264Encoder && h264 && !h264->hardwareBackend.empty()) {
        r.proxyFormat = "h264";
        r.reasons.push_back("The GPU encodes and decodes H.264: proxies are made as all-intra H.264 (small, quick to make).");
    } else {
        r.proxyFormat = "prores";
        r.reasons.push_back("Proxies are ProRes Proxy: quick to decode on any processor.");
    }
    return r;
}

std::string toJson(const PerformanceReport& report) {
    nlohmann::json decode = nlohmann::json::array();
    for (const DecodeMeasurement& m : report.decode) {
        decode.push_back({{"codec", m.codec}, {"available", m.available}, {"hardwareBackend", m.hardwareBackend},
                          {"hardwareFps", m.hardwareFps}, {"softwareFps", m.softwareFps}, {"note", m.note}});
    }
    nlohmann::json encoders = nlohmann::json::array();
    for (const EncoderMeasurement& e : report.encoders) {
        encoders.push_back({{"codec", e.codec}, {"encoder", e.encoder}, {"hardware", e.hardware}, {"note", e.note}});
    }
    const ReadinessRecommendation& r = report.recommendation;
    nlohmann::json j{{"version", 1},
                     {"decode", std::move(decode)},
                     {"encoders", std::move(encoders)},
                     {"gpus", report.gpus},
                     {"graphics", report.graphics},
                     {"gpuName", report.gpuName},
                     {"gpuReason", report.gpuReason},
                     {"previewGpuName", report.previewGpuName},
                     {"gpuNames", report.gpuNames},
                     {"audioDevice", report.audioDevice},
                     {"audioName", report.audioName},
                     {"audioNote", report.audioNote},
                     {"cpuThreads", report.cpuThreads},
                     {"width", report.width},
                     {"height", report.height},
                     {"frameRate", report.frameRate},
                     {"seconds", report.seconds},
                     {"cancelled", report.cancelled},
                     {"outOfTime", report.outOfTime},
                     {"recommendation",
                      {{"proxyPolicy", r.proxyPolicy},
                       {"proxyFormat", r.proxyFormat},
                       {"proxyDivisor", r.proxyDivisor},
                       {"previewQuality", r.previewQuality},
                       {"reasons", r.reasons}}}};
    return j.dump();
}

std::optional<PerformanceReport> reportFromJson(std::string_view text) {
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        if (!j.is_object() || j.value("version", 0) != 1) {
            return std::nullopt;
        }
        PerformanceReport report;
        for (const auto& m : j.at("decode")) {
            report.decode.push_back({m.value("codec", ""), m.value("available", false), m.value("hardwareBackend", ""),
                                     m.value("hardwareFps", 0.0), m.value("softwareFps", 0.0), m.value("note", "")});
        }
        for (const auto& e : j.at("encoders")) {
            report.encoders.push_back({e.value("codec", ""), e.value("encoder", ""), e.value("hardware", false), e.value("note", "")});
        }
        report.gpus = j.value("gpus", std::vector<std::string>{});
        report.graphics = j.value("graphics", "");
        report.gpuName = j.value("gpuName", "");
        report.gpuReason = j.value("gpuReason", "");
        report.previewGpuName = j.value("previewGpuName", "");
        report.gpuNames = j.value("gpuNames", std::vector<std::string>{});
        report.audioDevice = j.value("audioDevice", false);
        report.audioName = j.value("audioName", "");
        report.audioNote = j.value("audioNote", "");
        report.cpuThreads = j.value("cpuThreads", 0);
        report.width = j.value("width", 0);
        report.height = j.value("height", 0);
        report.frameRate = j.value("frameRate", 0.0);
        report.seconds = j.value("seconds", 0.0);
        report.cancelled = j.value("cancelled", false);
        report.outOfTime = j.value("outOfTime", false);
        const auto& r = j.at("recommendation");
        report.recommendation.proxyPolicy = r.value("proxyPolicy", "auto");
        report.recommendation.proxyFormat = r.value("proxyFormat", "prores");
        report.recommendation.proxyDivisor = r.value("proxyDivisor", 2);
        report.recommendation.previewQuality = r.value("previewQuality", "auto");
        report.recommendation.reasons = r.value("reasons", std::vector<std::string>{});
        return report;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

} // namespace wabcut::editor
