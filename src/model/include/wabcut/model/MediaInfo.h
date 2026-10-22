#pragma once

// Plain-data description of a media file. Filled by media::probe(), stored in the project by model.
// No FFmpeg types here: enums are Wabcut's own so model stays dependency-free.

#include <wabcut/model/Time.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wabcut {

enum class ColorMatrix { Unknown, BT601, BT709, BT2020NonConstant, BT2020Constant, SMPTE240M, RGB };
enum class ColorRange { Unknown, Limited, Full };
enum class ColorPrimaries { Unknown, BT709, BT601_525, BT601_625, BT2020, DCIP3, DisplayP3 };
enum class TransferFunction { Unknown, BT709, SRGB, Linear, Gamma22, Gamma28, PQ, HLG };
enum class ChromaLocation { Unknown, Left, Center, TopLeft, Top, BottomLeft, Bottom };
enum class AlphaMode { None, Straight, Premultiplied };

/// P.2 (E5): the field order of a sequence's frames (Premiere's Sequence Settings > Fields), a media item's
/// interpretation (Interpret Footage > Field Order) or, since 0.2's field options, what a file's video stream says
/// (VideoStreamInfo::fieldOrder). Wabcut composites progressive frames: interlaced pictures are deinterlaced where
/// a clip shows them (FieldOptions.h).
enum class FieldOrder : std::uint8_t { Progressive, UpperFirst, LowerFirst };

struct ColorInfo {
    ColorMatrix matrix = ColorMatrix::Unknown;
    ColorRange range = ColorRange::Unknown;
    ColorPrimaries primaries = ColorPrimaries::Unknown;
    TransferFunction transfer = TransferFunction::Unknown;
    ChromaLocation chromaLocation = ChromaLocation::Unknown;

    bool operator==(const ColorInfo&) const = default;
};

struct VideoStreamInfo {
    int index = -1;               // container stream index
    std::string codec;            // e.g. "h264", "hevc", "prores"
    std::string profile;          // e.g. "High 4:2:2", "Main 10"
    int width = 0;
    int height = 0;
    Rate frameRate{};             // exact nominal rate (e.g. {30000, 1001})
    bool variableFrameRate = false;
    Rate sampleAspect{1, 1};
    std::string pixelFormat;      // FFmpeg name, informational (e.g. "yuv420p10le")
    int bitDepth = 8;
    std::string chromaSubsampling; // "4:2:0", "4:2:2", "4:4:4"
    ColorInfo color{};
    AlphaMode alpha = AlphaMode::None;
    int rotationDegrees = 0;      // display rotation (0/90/180/270)
    Ticks duration{};
    std::int64_t frameCount = -1; // -1 when unknown
    /// The camera log the file's metadata names (0.2, colour management's "auto-detected log"): an InputColorSpace id
    /// ("s-log3-cine", "logc3", ...; ColorManagement.h), empty when the metadata names none. Camera log is never in the
    /// stream's colour tags: media::probe reads the vendors' metadata (Sony's NonRealTimeMeta XML, QuickTime keys).
    /// Saved only when set (schema 17).
    std::string logColorSpace;
    /// 0.2 (field options): whether the stream's pictures are interlaced and which field comes first, as the file
    /// says (FFmpeg's field order of the stream; bottom-coded orders count by their first field). Interpret Footage
    /// can override it (MediaInterpretation::fieldOrder). Saved only when not Progressive (schema 18).
    FieldOrder fieldOrder = FieldOrder::Progressive;

    bool operator==(const VideoStreamInfo&) const = default;
};

struct AudioStreamInfo {
    int index = -1;
    std::string codec;
    Rate sampleRate{48000, 1};
    int channels = 0;
    std::string channelLayout;    // e.g. "stereo", "5.1"
    Ticks duration{};
    /// A.10: the stream's name in the file (MKV track name, MP4 handler name: OBS, ShadowPlay and Game Bar write their
    /// track names there), UTF-8 and trimmed; empty when it has none or a generic one a muxer writes for every track
    /// ("SoundHandler", ...). Multi-track ingest names timeline tracks after it (EditOps.h: sourceTrackNames). Saved
    /// only when set (schema 15).
    std::string title;

    bool operator==(const AudioStreamInfo&) const = default;
};

struct MediaInfo {
    std::string path;             // UTF-8
    std::string container;        // e.g. "mov,mp4,m4a,3gp,3g2,mj2"
    Ticks duration{};
    std::int64_t fileSize = 0;
    std::int64_t modifiedTime = 0; // seconds since epoch; used for cache keys / relink checks
    bool isStillImage = false;
    std::vector<VideoStreamInfo> video;
    std::vector<AudioStreamInfo> audio;
    /// The timecode of the file's first frame as a time since 00:00:00:00, when the file has one (C.8: multicam sync
    /// by timecode). Saved only when set.
    std::optional<Ticks> startTimecode;

    bool hasVideo() const { return !video.empty(); }
    bool hasAudio() const { return !audio.empty(); }
    bool operator==(const MediaInfo&) const = default;
};

} // namespace wabcut
