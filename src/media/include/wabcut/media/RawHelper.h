#pragma once

// Camera RAW through helpers (0.2): formats whose only decoders are their vendors' proprietary SDKs (RED .r3d,
// Blackmagic .braw, ...) cannot be linked into GPL-3.0 Wabcut. Instead a separate program built with the SDK - a
// helper the user installs - decodes them, and Wabcut reads its frames through a pipe. Nothing of the SDK is shipped.
//
// Discovery. For a file "<name>.<ext>" the helper is "wabcut-raw-<ext>" (".exe" on Windows; the extension in lower
// case) in the folder WABCUT_RAW_HELPERS names, else in "raw-helpers" next to the running program.
//
// Protocol (all lines UTF-8, ending in '\n'). Wabcut starts `wabcut-raw-<ext> "<file>"`. The helper writes one JSON
// header line:
//     {"width": 4096, "height": 2160, "rateNum": 24000, "rateDen": 1001, "frames": 2400, "pixelFormat": "rgba64le",
//      "colorTransfer": "bt709"}
// ("pixelFormat": an FFmpeg pixel format name, "rgba64le" or "yuv444p10le" recommended; "colorTransfer": "bt709",
// "srgb" or "linear"), or {"error": "why"} and exits. Then it reads requests on stdin: a frame index ("17\n")
// answered by exactly one frame of that format (width x height, no padding), or "quit\n". Frames are asked for in any
// order. The helper exits when stdin closes.
//
// media::detail::openInput opens such a file as the helper's frames in FFmpeg's rawvideo format (the frame index is
// the byte offset over the frame size), so probing, decoding, thumbnails, playback and export work unchanged. Sound
// is not carried (the file's audio is not read).

#include <wabcut/model/Error.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::media {

/// The helper that decodes `file`, or nullopt when none is installed.
std::optional<std::filesystem::path> rawHelperFor(const std::filesystem::path& file);

struct RawHelperHeader {
    int width = 0;
    int height = 0;
    std::int32_t rateNum = 0;
    std::int32_t rateDen = 0;
    std::int64_t frames = 0;
    std::string pixelFormat;   ///< an FFmpeg pixel format name
    std::string colorTransfer; ///< "bt709", "srgb" or "linear"
};

/// One running helper for one file.
class RawHelper {
public:
    /// Starts `helper` on `file` and reads its header. Errors: NotFound, IoError (it cannot start or its pipe broke),
    /// Unsupported (its header says why, or it is malformed or out of bounds).
    static std::expected<std::unique_ptr<RawHelper>, Error> start(const std::filesystem::path& helper,
                                                                 const std::filesystem::path& file);
    ~RawHelper(); ///< asks it to quit, then ends it
    RawHelper(const RawHelper&) = delete;
    RawHelper& operator=(const RawHelper&) = delete;

    const RawHelperHeader& header() const;
    /// Bytes of one frame in the header's pixel format (set by the caller: FFmpeg knows the layout).
    void setFrameBytes(std::size_t bytes);
    std::size_t frameBytes() const;
    /// Frame `index` (0 .. frames - 1) into `out` (frameBytes() bytes). Errors: InvalidArgument, IoError.
    std::expected<void, Error> readFrame(std::int64_t index, std::uint8_t* out);

private:
    struct Impl;
    explicit RawHelper(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::media
