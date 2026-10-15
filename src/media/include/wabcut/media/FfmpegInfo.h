#pragma once

#include <string>
#include <vector>

namespace wabcut::media {

/// "libavcodec 62.x.y / libavformat …" — shown in About and `wabcut-cli --version`.
std::string ffmpegVersionString();

/// FFmpeg hardware device types compiled into this build (e.g. "d3d11va", "cuda", "qsv", "videotoolbox").
/// Compiled-in does not mean usable: the decoder still probes each device at runtime and falls back to
/// software.
std::vector<std::string> compiledHwDeviceTypes();

} // namespace wabcut::media
