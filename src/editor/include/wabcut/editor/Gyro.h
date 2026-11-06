#pragma once

// The camera's gyro for the stabilizer (0.7; docs/motion.md "Stabilize with the camera's gyro"): wabcut::gyro::GyroServices
// over the media files. probeGyro() is the cheap look the Effect Controls header takes (which camera, what was found, why a
// file cannot be used); MediaGyroService::read() reads the whole data track for an analysis.
//
// Which files: GoPro (HERO5 and later) and Sony (XAVC S / HS / S-I with a gyro) MP4 files. Everything else says why not:
// DJI's metadata track is recognised but not read (DJI has not published it), RED R3D and Blackmagic RAW keep the gyro
// behind the makers' SDKs (Wabcut plays those files through a decoder helper that carries pictures only), Insta360's
// IMU trailer is not documented. Threading: free of shared state, so any thread may call both.

#include <wabcut/gyro/Gyro.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace wabcut::editor {

struct GyroProbe {
    enum class State : std::uint8_t {
        None,       ///< the file has no gyro data
        Readable,   ///< a gyro track Wabcut reads
        Unreadable, ///< the file has gyro data Wabcut does not read (or a damaged track): `reason` says why
    };
    State state = State::None;
    std::string make;
    std::string model;
    std::string format;
    double rateHz = 0.0; ///< gyroscope readings per second (measured on the start of the track)
    std::optional<double> readoutSeconds;
    std::optional<double> horizontalFovDegrees;
    bool axesChecked = false;
    std::string reason; ///< for None / Unreadable: one sentence, for the user
};

/// What the file at `utf8Path` offers: reads the first seconds of its gyro track only.
GyroProbe probeGyro(const std::string& utf8Path);

/// The service the stabilizer reads the gyro through: the media item's file (the original; a proxy carries no
/// camera data), the first readable gyro track of it.
class MediaGyroService final : public gyro::GyroServices {
public:
    explicit MediaGyroService(std::filesystem::path projectFile = {}, std::function<bool()> cancelled = {});
    std::expected<gyro::GyroTrack, Error> read(const ProjectState& state, MediaId media, int streamIndex) override;

private:
    std::filesystem::path m_projectFile;
    std::function<bool()> m_cancelled;
};

} // namespace wabcut::editor
