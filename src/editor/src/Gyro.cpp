// The camera's gyro for the stabilizer (editor/Gyro.h).

#include <wabcut/editor/Gyro.h>

#include <wabcut/media/CameraLog.h>
#include <wabcut/media/DataStreams.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <span>

namespace wabcut::editor {

namespace {

constexpr std::size_t kProbePackets = 12; // packets of a track read to describe it (about a second of GoPro, half a second of Sony)

std::string lowerExtension(const std::string& utf8Path) {
    std::string ext = pathFromUtf8(utf8Path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

/// A file Wabcut cannot even demux, by its extension: the camera makers keep the gyro behind their own SDKs or formats.
std::string vendorReason(const std::string& ext) {
    if (ext == ".r3d") {
        return "RED keeps the gyro of R3D clips inside its REDCODE SDK, which Wabcut cannot include: it is not read";
    }
    if (ext == ".braw") {
        return "Blackmagic RAW keeps the gyro behind Blackmagic's RAW SDK, which Wabcut cannot include: it is not read";
    }
    if (ext == ".insv" || ext == ".insp") {
        return "Insta360 writes its gyro in a trailer format Insta360 has not published: it is not read";
    }
    return {};
}

std::string describeNone() {
    return "this file carries no camera gyro data (GoPro HERO5 and later and Sony cameras with a gyro, recording to MP4, do)";
}

/// Reads the track's packets through the parser of its kind, one at a time (a recording of hours is never held as bytes);
/// `options` limit or cancel the read.
std::expected<gyro::GyroTrack, Error> readTrack(gyro::TrackKind kind, const std::string& path, int stream,
                                                const media::DataReadOptions& options) {
    gyro::GpmfReader gpmf;
    gyro::SonyRtmdReader sony;
    const bool isGpmf = kind == gyro::TrackKind::GoProGpmf;
    const auto read = media::forEachDataPacket(
        path, stream,
        [&](const media::DataPacket& p) {
            const gyro::Packet packet{p.seconds, p.duration, std::span<const std::uint8_t>(p.bytes)};
            if (isGpmf) {
                gpmf.add(packet);
            } else {
                sony.add(packet);
            }
            return true;
        },
        options);
    if (!read) {
        return std::unexpected(read.error());
    }
    return isGpmf ? gpmf.finish() : sony.finish(media::cameraModelFromFile(pathFromUtf8(path)));
}

struct Candidate {
    int stream = -1;
    gyro::TrackKind kind = gyro::TrackKind::NotTelemetry;
};

/// Finds the data tracks of the file: the first one that is read, and the first recognised one that is not.
std::optional<Error> findTracks(const std::string& path, std::optional<Candidate>& readable, std::string& unreadableReason) {
    auto streams = media::listDataStreams(path);
    if (!streams) {
        return streams.error();
    }
    for (const media::DataStreamInfo& s : *streams) {
        const gyro::TrackKind kind = gyro::classifyDataTrack(s.tag, s.handler);
        if (gyro::isReadable(kind)) {
            if (!readable) {
                readable = Candidate{s.index, kind};
            }
        } else if (unreadableReason.empty()) {
            unreadableReason = std::string(gyro::unreadableReason(kind));
        }
    }
    return std::nullopt;
}

GyroProbe fromTrack(const gyro::GyroTrack& track) {
    GyroProbe p;
    p.state = GyroProbe::State::Readable;
    p.make = track.make;
    p.model = track.model;
    p.format = track.format;
    p.rateHz = track.rateHz;
    p.readoutSeconds = track.readoutSeconds;
    p.horizontalFovDegrees = track.horizontalFovDegrees;
    p.axesChecked = track.axesChecked;
    return p;
}

} // namespace

GyroProbe probeGyro(const std::string& utf8Path) {
    GyroProbe out;
    if (const std::string vendor = vendorReason(lowerExtension(utf8Path)); !vendor.empty()) {
        out.state = GyroProbe::State::Unreadable;
        out.reason = vendor;
        return out;
    }
    std::optional<Candidate> readable;
    std::string unreadable;
    if (const std::optional<Error> e = findTracks(utf8Path, readable, unreadable)) {
        out.reason = e->message;
        return out;
    }
    if (!readable) {
        out.state = unreadable.empty() ? GyroProbe::State::None : GyroProbe::State::Unreadable;
        out.reason = unreadable.empty() ? describeNone() : unreadable;
        return out;
    }
    media::DataReadOptions options;
    options.firstPackets = kProbePackets;
    auto track = readTrack(readable->kind, utf8Path, readable->stream, options);
    if (!track) {
        out.state = GyroProbe::State::Unreadable;
        out.reason = track.error().message;
        return out;
    }
    return fromTrack(*track);
}

MediaGyroService::MediaGyroService(std::filesystem::path projectFile, std::function<bool()> cancelled)
    : m_projectFile(std::move(projectFile)), m_cancelled(std::move(cancelled)) {}

std::expected<gyro::GyroTrack, Error> MediaGyroService::read(const ProjectState& state, MediaId media, int streamIndex) {
    const MediaItem* item = state.findMedia(media);
    if (!item || streamIndex < 0 || streamIndex >= static_cast<int>(item->info.video.size())) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such video stream"});
    }
    // The original file only: a proxy has its own container without the camera's tracks.
    const auto found = resolveMediaPath(item->locator, m_projectFile);
    if (!found) {
        return std::unexpected(Error{ErrorCode::NotFound, "the media file is offline: " + item->name});
    }
    const std::string path = pathToUtf8(*found);
    if (const std::string vendor = vendorReason(lowerExtension(path)); !vendor.empty()) {
        return std::unexpected(Error{ErrorCode::NotFound, vendor});
    }
    std::optional<Candidate> readable;
    std::string unreadable;
    if (const std::optional<Error> e = findTracks(path, readable, unreadable)) {
        return std::unexpected(*e);
    }
    if (!readable) {
        return std::unexpected(Error{ErrorCode::NotFound, unreadable.empty() ? describeNone() : unreadable});
    }
    media::DataReadOptions options;
    options.cancelled = m_cancelled;
    options.maxBytes = ~std::size_t{0}; // the readings are what is kept; kMaxGyroReadings bounds them
    return readTrack(readable->kind, path, readable->stream, options);
}

} // namespace wabcut::editor
