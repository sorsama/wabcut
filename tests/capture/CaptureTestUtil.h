#pragma once

// Helpers of the capture tests: a capture file read back with FFmpeg (the fake pictures' barcodes, the sound), and fake pictures
// and sound fed to a recorder at chosen times. FFmpeg stays in CaptureTestUtil.cpp.

#include <wabcut/capture/CaptureRecorder.h>
#include <wabcut/capture/CaptureTypes.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace capturetest {

using namespace wabcut;

/// An empty build/<preset>/test-output/capture/<name> directory.
std::filesystem::path freshOutputDir(const std::string& name);

struct Readback {
    bool ok = false;
    std::string error;
    std::string container;     ///< "matroska,webm"
    std::string videoCodec;
    std::string audioCodec;
    int width = 0;
    int height = 0;
    int sarNum = 0;
    int sarDen = 0;
    std::vector<std::int64_t> barcodes;  ///< the fake picture number of every decoded picture, -1 when unreadable
    std::vector<double> times;           ///< presentation time of every decoded picture, seconds
    double videoSeconds = 0.0;           ///< last picture's time + one picture
    int audioRate = 0;
    int audioChannels = 0;
    std::int64_t audioFrames = 0;
    std::vector<float> audio;            ///< interleaved, when asked for
    std::string title;
};

/// Opens `file` and decodes everything. `withAudio`: keep the samples (otherwise only count them).
Readback readCapture(const std::filesystem::path& file, bool withAudio = false);

/// RMS of channel 0 of `samples` (interleaved, `channels`) between the frames [from, to).
double rms(const std::vector<float>& samples, int channels, std::int64_t from, std::int64_t to);

/// A fake NV12 picture number `index` taken at host time `hostNs`.
capture::CaptureFrame fakeFrame(int width, int height, std::int64_t index, std::int64_t hostNs);

/// A block of a 440 Hz tone: `frames` frames from device frame `firstFrame`, heard at host time `hostNs`.
capture::AudioBlock toneBlock(int rate, int channels, std::int64_t firstFrame, std::int64_t frames, std::int64_t hostNs);

/// The recorder's config for 320 x 180 NV12 at `fps`, sound at 48 kHz stereo, software H.264.
capture::RecorderConfig smallConfig(const std::filesystem::path& file, Rate fps = Rate{30, 1}, bool audio = true);

} // namespace capturetest
