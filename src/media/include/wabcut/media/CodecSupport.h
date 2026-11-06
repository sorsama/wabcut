#pragma once

// What this build of Wabcut can do with the newer camera and phone codecs (0.7): APV, 4:2:2 HEVC, 4:2:2 AV1 and
// MV-HEVC. Not a table written by hand: each answer is asked of the FFmpeg the build links - is there a decoder, is
// there an encoder, and does the encoder take the format - so it follows a library update and says plainly what is
// decode-only. The app puts the words to it (Properties of a clip, About, the CLI's --version).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::media {

struct CodecSupport {
    /// "apv", "hevc-422", "av1-422", "mv-hevc".
    std::string id;
    /// The FFmpeg decoder that plays it (empty: this build has none).
    std::string decoder;
    /// The FFmpeg encoder that exports it in the format named (empty: none: the codec is decode-only here).
    std::string encoder;
    /// HEVC 4:2:2: the export is 8-bit only (the bundled libx265 is an 8-bit build; the 10-bit 4:2:2 hardware encoders
    /// need a GPU generation this version does not rely on).
    bool encoderEightBitOnly = false;
    /// MV-HEVC: the decoder can decode a second view, but Wabcut uses the base view only.
    bool baseViewOnly = false;

    bool decodes() const { return !decoder.empty(); }
    bool exports() const { return !encoder.empty(); }
};

/// The newer codecs, in a fixed order (apv, hevc-422, av1-422, mv-hevc), as this FFmpeg has them.
std::vector<CodecSupport> newerCodecSupport();

/// The entry that applies to a video stream whose FFmpeg codec name is `codec` ("apv", "hevc", "av1", ...) and chroma
/// subsampling `chroma` ("4:2:0", "4:2:2", "4:4:4"): APV; HEVC or AV1 in 4:2:2. nullopt for a stream that needs no note.
std::optional<CodecSupport> codecSupportFor(std::string_view codec, std::string_view chroma);

} // namespace wabcut::media
