#pragma once

// Camera gyro metadata (0.7, Warp Stabilizer's "Gyro (camera metadata)" method; docs/motion.md "Stabilize with the
// camera's gyro"): what a camera records about its own rotation, read out of the file as untrusted binary input.
//
// A camera writes its gyroscope (and accelerometer) readings into a data track of the video file. This library parses
// those tracks - pure C++, no FFmpeg and no Qt: the host hands over the track's packets (wabcut/media/DataStreams.h reads
// them with FFmpeg) and gets a GyroTrack back: the angular velocity in ONE camera frame whatever the camera, timed on
// the file's clock, with the timing and lens facts the metadata carries (rolling shutter readout, exposure, focal
// length). Every read is bounds checked; a damaged packet ends that packet's data, never the program.
//
// Formats and what is read (the per-camera table is in docs/motion.md):
//   * GoPro GPMF ('gpmd' track): HERO5 and later, Fusion, MAX. Documented by GoPro (KLV layout, GYRO / ACCL streams,
//     SCAL, ORIN / ORIO / MTRX, STMP): https://github.com/gopro/gpmf-parser (README, "GPMF Deeper Dive").
//   * Sony rtmd ('rtmd' track of XAVC S / HS / S-I MP4 files, Alpha / FX / ZV / RX cameras with a gyroscope): Sony does
//     not publish the layout; the tag numbers come from the open-source parser of the Gyroflow project (MIT or
//     Apache-2.0), https://github.com/AdrianEddy/telemetry-parser (src/sony/rtmd_tags.rs, src/sony/mod.rs), and the
//     SMPTE RDD 18 camera / lens metadata sets it extends.
//   * Recognised but NOT read (classifyDataTrack): DJI's 'djmd' / 'dbgi' protobuf tracks (the layouts are DJI's own and
//     change per model; nothing public documents them), Insta360, Blackmagic RAW and RED R3D (the gyro sits behind the
//     vendor SDKs, FFmpeg cannot open the files).
//
// Camera frame (every GyroTrack): x to the right of the picture, y down, z out of the lens, right-handed; omega is the
// camera's angular velocity in rad/s about those axes, so a positive y turns the view to the right and the scene
// moves left in the picture.

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::gyro {

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    bool operator==(const Vec3&) const = default;
};

/// One packet (sample) of a data track as the container stores it.
struct Packet {
    double seconds = 0.0;  ///< presentation time on the file's clock
    double duration = 0.0; ///< seconds; 0 when the container does not say
    std::span<const std::uint8_t> bytes;
};

/// Hard limits (files are untrusted): a packet, a track's readings, the readings kept for the accelerometer.
inline constexpr std::size_t kMaxPacketBytes = 64u << 20;
inline constexpr std::size_t kMaxGyroReadings = 16'000'000;
inline constexpr int kMaxNesting = 8;

enum class Camera : std::uint8_t { None, GoPro, Sony };

struct GyroTrack {
    Camera camera = Camera::None;
    std::string make;   ///< "GoPro", "Sony"
    std::string model;  ///< as the file names it ("HERO8 Black"); empty when it does not
    std::string format; ///< "GoPro GPMF", "Sony rtmd"
    /// The gyroscope: strictly increasing times (seconds on the file's clock) and the angular velocity (rad/s) in the
    /// camera frame described at the top of this file.
    std::vector<double> times;
    std::vector<Vec3> omega;
    /// The accelerometer in the same frame (m/s^2), on its own times; empty when the file has none. Kept for a horizon
    /// lock; the stabilizer does not use it yet.
    std::vector<double> accelTimes;
    std::vector<Vec3> accel;
    double rateHz = 0.0; ///< gyroscope readings per second (measured)
    /// Rolling shutter: the time the sensor takes to read all of its lines, and the exposure time, when the file says.
    std::optional<double> readoutSeconds;
    std::optional<double> exposureSeconds;
    /// The lens: the actual focal length (mm) and the width (mm) of the sensor area the pictures use, and the
    /// horizontal field of view they give with a rectilinear lens, when the file says enough to compute it.
    std::optional<double> focalLengthMm;
    std::optional<double> imagedWidthMm;
    std::optional<double> horizontalFovDegrees;
    /// Whether the axis mapping was checked against real footage of this camera family (GoPro: yes). An unchecked
    /// mapping is confirmed against the picture by Auto-sync.
    bool axesChecked = false;

    bool empty() const { return times.empty(); }
    double firstTime() const { return times.empty() ? 0.0 : times.front(); }
    double lastTime() const { return times.empty() ? 0.0 : times.back(); }
};

/// What a data track's sample entry (four characters, "gpmd") means to this library.
enum class TrackKind : std::uint8_t {
    GoProGpmf,    ///< read
    SonyRtmd,     ///< read
    DjiMetadata,  ///< recognised, not read: DJI has not published its metadata layout
    OtherCamera,  ///< recognised as some camera's telemetry, not read
    NotTelemetry, ///< a timecode, text or unknown data track
};
TrackKind classifyDataTrack(std::string_view fourcc, std::string_view handlerName = {});
/// Why a recognised track is not read, in a sentence for the user ("" for a track that is read).
std::string_view unreadableReason(TrackKind kind);
inline bool isReadable(TrackKind kind) { return kind == TrackKind::GoProGpmf || kind == TrackKind::SonyRtmd; }

/// Parses a GoPro 'gpmd' track one packet at a time: each packet is read when it is added and only its gyroscope and
/// accelerometer readings are kept, so a recording of hours is never held in memory as bytes. Untrusted input.
class GpmfReader {
public:
    GpmfReader();
    ~GpmfReader();
    GpmfReader(const GpmfReader&) = delete;
    GpmfReader& operator=(const GpmfReader&) = delete;
    /// Packets in track order.
    void add(const Packet& packet);
    /// The track, or ErrorCode::ParseError when no gyroscope stream was found.
    std::expected<GyroTrack, Error> finish();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

/// Parses a Sony 'rtmd' track (one packet per video frame) one packet at a time, as GpmfReader does. `model` is the camera's
/// model name, read from the file's own metadata by the host ("ILCE-7M4"), when it has one.
class SonyRtmdReader {
public:
    SonyRtmdReader();
    ~SonyRtmdReader();
    SonyRtmdReader(const SonyRtmdReader&) = delete;
    SonyRtmdReader& operator=(const SonyRtmdReader&) = delete;
    void add(const Packet& packet);
    std::expected<GyroTrack, Error> finish(std::string_view model = {});

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

/// The whole track in one call (the readers above over `packets`). ErrorCode::ParseError when no gyroscope stream is found.
std::expected<GyroTrack, Error> parseGpmf(std::span<const Packet> packets);
std::expected<GyroTrack, Error> parseSonyRtmd(std::span<const Packet> packets, std::string_view model = {});

/// The accessors the stabilizer reads the gyro through. The app's implementation reads the media file of a clip
/// (wabcut::editor::MediaGyroService); tests give a fake. `streamIndex` is the video stream's index in the media item.
class GyroServices : public command::CommandServices {
public:
    ~GyroServices() override = default;
    /// The gyro track of video stream `streamIndex` of `media`, or why there is none (ErrorCode::NotFound: the file has
    /// no readable gyro; the message names what it has instead, for the user).
    virtual std::expected<GyroTrack, Error> read(const ProjectState& state, MediaId media, int streamIndex) = 0;
};

} // namespace wabcut::gyro
