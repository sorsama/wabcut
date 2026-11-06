#pragma once

// How a capture is written (0.7, docs/capture.md): three presets and the disk a capture takes. The picture is encoded while
// it is captured, so a preset has to keep up with a live source on a modest computer; hardware encoders are tried first
// (the export's order), and a software encoder takes its fast preset.
//
//   high    H.264 at constant quality 18, one key frame a second, Matroska. The default: a good picture, files an editor
//           reads without effort (estimated at 0.2 bits per pixel: 1080p30 is about 1.6 MB/s, 6 GB an hour).
//   compact H.264 at constant quality 24: about half of that, for long recordings of material that will not be graded.
//   intra   ProRes 422 (Standard) in Matroska: every frame stands alone, 10-bit 4:2:2, for tape and for material that will
//           be cut hard or graded (about 150 Mb/s at 1080p30, 65 GB an hour; standard definition is a sixth of that).
// The sound is uncompressed 16-bit PCM in all of them.

#include <wabcut/capture/CaptureTypes.h>

#include <wabcut/export/ExportSettings.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::capture {

struct CapturePreset {
    std::string id; ///< "high", "compact", "intra" (stored in the user's settings)
    std::string label;
    std::string description;
    exporter::Container container = exporter::Container::Mkv;
    exporter::VideoSettings video;
    exporter::AudioSettings audio;
    double bitsPerPixel = 0.1; ///< average, for the disk estimate
};

const std::vector<CapturePreset>& capturePresets();
/// The preset with that id, or the first ("high") for an unknown one.
const CapturePreset& capturePreset(std::string_view id);

/// About how many bytes a second a capture of `format` takes with `preset` (picture and sound).
std::int64_t estimateBytesPerSecond(const CapturePreset& preset, const VideoFormat& format, bool audio, int audioChannels,
                                    int audioRate);

/// "42 s", "12 min", "1 h 20 min" for a duration in seconds (the free-disk estimate); "" for a negative one.
std::string durationText(double seconds);

} // namespace wabcut::capture
