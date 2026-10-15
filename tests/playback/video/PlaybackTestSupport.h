#pragma once

// Helpers for the video playback tests: projects built over the generated test media, the frame-index
// barcode, and a consumer loop that plays the render thread's part (acquire from the mailbox at display
// rate).

#include <wabcut/media/VideoFrame.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>
#include <wabcut/render/CompositeMailbox.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::playback::test {

inline constexpr Ticks kFrame60{kTicksPerSecond / 60};

std::string mediaPath(std::string_view name);
bool haveMedia(std::string_view name);

struct ClipSpec {
    std::string media;           ///< Test-media file name.
    int track = 0;               ///< 0 = V1.
    std::int64_t startFrame = 0; ///< Sequence frames.
    std::int64_t frames = 60;
    std::int64_t sourceInFrame = 0; ///< In the clip's own 60 fps frames.
    ClipProperties properties{};
    bool enabled = true;
};

struct TestProject {
    ProjectState state;
    SequenceId sequence;
    std::vector<ClipId> clips;
};

/// A sequence (`width` x `height` at `rate`) with one video track per used track index, over real probed
/// media. REQUIREs that probing succeeds.
TestProject makeProject(const std::vector<ClipSpec>& clips, int width = 1920, int height = 1080,
                        Rate rate = kRate60);

/// The frame number gen_test_media.py burnt into the frame (16-bit barcode, luma, top-left).
std::int64_t barcodeOf(const media::VideoFrame& frame);

struct Presented {
    Ticks target{};
    std::int64_t barcode = -1; ///< Of the top layer (-1: no layers).
    std::uint64_t epoch = 0;
    int layers = 0;
};

/// Acts as the render thread: acquires from `mailbox` every ~16.7 ms for `duration` (or until `stop` returns
/// true) and records every new picture.
std::vector<Presented> consume(render::CompositeMailbox& mailbox, std::chrono::milliseconds duration,
                               const std::function<bool(const std::vector<Presented>&)>& stop = {});

} // namespace wabcut::playback::test
