#pragma once

// Private (0.6, #73): the MXF side of an export - the muxer's metadata (timecode, clip and reel names, signal standard),
// the audio laid out as MXF wants it (OP1a: one mono PCM track per channel or one multichannel track; OP-Atom: one file
// per channel next to the video file), and the UMID read back for the report.
//
// The mix is rendered at its own channel count (stereo, or 5.1 when the master is 5.1 and there are 6 or more tracks) and
// spread over the tracks: track n carries mix channel n, the tracks after the mix are silent (a stereo programme in 8
// tracks: 1-2 the mix, 3-8 silence - a common broadcast layout).

#include "Encode.h"

#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/Sequence.h>

#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace wabcut::exporter::detail {

/// OP-Atom: the file of audio channel `channel` (1-based) of the video file `video`: "<stem>_A<channel>.mxf".
std::filesystem::path opAtomAudioPath(const std::filesystem::path& video, int channel);

/// The channels the mix of an MXF export with `outputs` tracks renders: 6 for a 5.1 master into 6 or more tracks, else
/// stereo (mono for one track).
int mxfMixChannels(int outputs, ChannelFormat master);

/// What every MXF file of an export says about itself.
struct MxfFileInfo {
    std::string timecode; ///< the start timecode ("01:00:00:00", ";" drop-frame)
    std::string clipName; ///< the material package name (the sequence's name)
    std::string reelName; ///< the tape source package; empty = none
    Rate frameRate{};
    int height = 0; ///< the picture's height (its signal standard); 0 = an audio file
};

/// Sets the metadata and muxer options of one MXF file (before Muxer::open).
void configureMxfMuxer(Muxer& muxer, const MxfFileInfo& info);

/// The audio of an MXF export: PCM encoders for its tracks or files.
class MxfAudioOut {
public:
    /// OP1a: the tracks are streams of `main` (added here, after the video stream). OP-Atom: one file per track, each with
    /// its own muxer, next to `video` (written to temporary names, moved into place by commitFiles()). `settings`: the
    /// export's audio settings with channels resolved (the track count).
    static std::expected<std::unique_ptr<MxfAudioOut>, Error> create(Container container, MxfAudioLayout layout,
                                                                     const AudioSettings& settings, int mixChannels,
                                                                     Muxer& main, const std::filesystem::path& video,
                                                                     const MxfFileInfo& info);
    ~MxfAudioOut();
    MxfAudioOut(const MxfAudioOut&) = delete;
    MxfAudioOut& operator=(const MxfAudioOut&) = delete;

    /// OP-Atom: opens every audio file (after the main muxer is configured; before rendering).
    std::expected<void, Error> open();
    /// Encodes `mix` (interleaved, mixChannels a frame) into every track.
    std::expected<void, Error> encode(std::span<const float> mix);
    std::expected<void, Error> flush();
    /// OP-Atom: writes the audio files' trailers; they stay under their temporary names (removed with this object).
    std::expected<void, Error> finishFiles();
    /// OP-Atom: moves the finished audio files into place - after the picture file is in place, so a failed or
    /// cancelled export never leaves audio files without their picture; the files moved (OP1a: none).
    std::expected<std::vector<std::filesystem::path>, Error> commitFiles();

    /// "8 mono tracks · PCM 24-bit 48 kHz · 1-2 stereo mix, 3-8 silent" (the export details).
    std::string describe() const;
    const std::string& encoderName() const { return m_encoderName; }
    int tracks() const { return static_cast<int>(m_tracks.size()); }

private:
    MxfAudioOut() = default;

    struct Track {
        AudioEncoder encoder;
        Muxer* muxer = nullptr;
        AVStream* stream = nullptr;
        std::vector<int> channels; ///< mix channel per track channel; -1 = silence
        std::vector<float> buffer;
        std::unique_ptr<Muxer> own;   ///< OP-Atom: this track's file
        std::filesystem::path temp;   ///< and its temporary name
        std::filesystem::path final;  ///< and where it goes
    };
    std::vector<Track> m_tracks;
    Container m_container = Container::Mxf;
    MxfAudioLayout m_layout = MxfAudioLayout::MonoTracks;
    int m_mixChannels = 2;
    int m_outputs = 2;
    int m_bits = 24;
    std::string m_encoderName;
};

/// Writes an export's caption track (Muxer::addCaptionStream): MXF - each frame's cc_data as one packet (the SMPTE 436M
/// track needs one per edit unit); QuickTime - c608 samples of the frame's field 1 pairs, a frame without any lengthening
/// the sample before (59.94: a pair every other frame). A null stream writes nothing.
class CaptionTrackWriter {
public:
    CaptionTrackWriter(Muxer& muxer, AVStream* stream, AVRational frameTimeBase, bool quickTime)
        : m_muxer(&muxer), m_stream(stream), m_timeBase(frameTimeBase), m_quickTime(quickTime) {}

    std::expected<void, Error> frame(std::int64_t k, std::span<const std::uint8_t> ccData);
    /// Writes what is held back (QuickTime's last sample) at the end of `frames` frames.
    std::expected<void, Error> finish(std::int64_t frames);

private:
    std::expected<void, Error> write(const std::vector<std::uint8_t>& bytes, std::int64_t pts, std::int64_t duration);

    Muxer* m_muxer = nullptr;
    AVStream* m_stream = nullptr;
    AVRational m_timeBase{1, 25};
    bool m_quickTime = false;
    std::vector<std::uint8_t> m_pending;
    std::int64_t m_pendingStart = -1;
};

/// The material package UMID of an MXF file as FFmpeg's demuxer reports it ("0x060A2B34..."), "" when unreadable.
std::string mxfMaterialUmid(const std::filesystem::path& file);

} // namespace wabcut::exporter::detail
