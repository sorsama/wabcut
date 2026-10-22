#pragma once

// The part of AAF and OMF export they share (AudioPostExport.h): a plan of tracks, clips, fades and audio sources made
// from the model in samples of the export rate, the rendering of its new audio files through the host, and the WAV /
// AIFF file layout those files (and the essence embedded in AAF and OMF files) use.

#include <wabcut/interchange/AudioPostExport.h>
#include <wabcut/model/Sequence.h>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange::detail {

/// The product version files name: `given`, else this build's (AafExporter.cpp).
std::string productVersion(const std::string& given);
/// Sample rate 48000 / 96000, 16 / 24 bits, handles 0..1000 frames. Errors: InvalidArgument.
std::expected<void, Error> validateAudioPostAudio(const AudioPostAudio& audio);

/// Removes a folder (and what is in it) when it goes out of scope.
class FolderGuard {
public:
    explicit FolderGuard(std::filesystem::path folder) : m_folder(std::move(folder)) {}
    ~FolderGuard() {
        if (!m_folder.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(m_folder, ec);
        }
    }
    FolderGuard(const FolderGuard&) = delete;
    FolderGuard& operator=(const FolderGuard&) = delete;

private:
    std::filesystem::path m_folder;
};

} // namespace wabcut::interchange::detail

namespace wabcut::interchange::detail::audiopost {

/// A 128-bit identifier from `text` (two FNV-1a 64 hashes): the same inputs give the same MobIDs, so a re-export of
/// unchanged material names the same files.
std::array<std::uint8_t, 16> stableId(std::string_view text);

// --- Audio files -------------------------------------------------------------------------------------------------

/// Broadcast WAV extension chunk (EBU Tech 3285 v1).
struct Bext {
    std::string description;          ///< at most 256 bytes
    std::string originator;           ///< at most 32
    std::string originatorReference;  ///< at most 32
    std::string originationDate;      ///< "yyyy-mm-dd"
    std::string originationTime;      ///< "hh:mm:ss"
    std::uint64_t timeReference = 0;  ///< samples since midnight of the first sample
    std::array<std::uint8_t, 32> umid{}; ///< a basic UMID (the AAF source mob's MobID)
};

enum class FileKind : std::uint8_t { Wave, Aiff, Aifc };

/// Everything before the first sample of a file of `frames` frames: WAV = RIFF, WAVE, bext (when given), fmt, data
/// header; AIFF = FORM AIFF, COMM, SSND header (offset, block size); AIFF-C the same with FVER and an uncompressed
/// ("NONE") COMM. Samples follow: WAV little-endian, AIFF big-endian, 16 or 24 bits.
std::string fileHeader(FileKind kind, int channels, int sampleRate, int bits, std::uint64_t frames, const Bext* bext);
/// The pad byte after an odd number of sample bytes (both formats align chunks to two bytes), or nothing.
std::string fileTrailer(std::uint64_t dataBytes);
/// Appends `samples` (-1..1, clamped, rounded to nearest) as `bits`-bit integers.
void encodeSamples(std::span<const float> samples, int bits, bool bigEndian, std::string& out);
/// AIFF's 80-bit IEEE extended sample rate.
std::array<std::uint8_t, 10> extended80(double value);

// --- The plan ----------------------------------------------------------------------------------------------------

/// A gain over a segment: constant (dB) or points (sample offset from the segment start, dB), linear between them.
struct Gain {
    double constantDb = 0.0;
    std::vector<std::pair<std::int64_t, double>> points; ///< empty: constant

    bool isUnity() const { return points.empty() && constantDb == 0.0; }
};

/// A pan over a segment (0 = left, 0.5 = centre, 1 = right): constant or points as for Gain.
struct Pan {
    double constant = 0.5;
    std::vector<std::pair<std::int64_t, double>> points;

    bool isCentre() const { return points.empty() && constant == 0.5; }
};

/// An audio file a clip plays: a new (rendered) mono file, or a linked original.
struct Source {
    bool rendered = true;
    std::string name;            ///< mob / file name ("Interview A1 Left")
    std::array<std::uint8_t, 16> material{}; ///< identity (MobIDs derive from it)
    std::int64_t length = 0;     ///< samples at the export rate
    int channels = 1;
    int sampleRate = 48000;      ///< the file's own rate (linked media may differ from the export rate)
    int bits = 24;
    // Linked
    std::string originalPath;    ///< absolute UTF-8 path
    // Rendered
    std::size_t group = 0;
    int channel = 0;
    std::filesystem::path file;  ///< after rendering: the final file (Separate) or a temporary one (Embed)
    std::string header;          ///< after rendering: the file's header (AAF summaries, OMF descriptors)
    std::uint64_t dataBytes = 0; ///< after rendering
    std::uint64_t fileBytes = 0; ///< after rendering
    bool failed = false;         ///< rendering failed (offline media): its clips are left out
};

/// One render request: a clip's audio, one mono Source per channel.
struct RenderGroup {
    AudioPostRenderRequest request;
    std::int64_t frames = 0;
    std::vector<std::size_t> sources; ///< per channel
    std::string where;                ///< report location of the clip
    std::string clipName;
};

struct ClipRef {
    std::string name;
    std::int64_t start = 0;       ///< timeline sample
    std::int64_t length = 0;
    std::size_t source = 0;
    std::int64_t sourceStart = 0; ///< offset into the source, samples at the export rate
    Gain gain;                    ///< clip gain over [start, start + length)
    ClipId clip;
    std::string where;
};

/// An audio transition on a track's edit point, in samples: a crossfade between two clips, or a fade at one clip's
/// edge (the other side is empty).
struct Fade {
    enum class Kind : std::uint8_t { Cross, In, Out } kind = Kind::Cross;
    std::int64_t cut = 0;     ///< the edit point (In: the clip's start; Out: its end)
    std::int64_t before = 0;  ///< extent before the cut
    std::int64_t after = 0;   ///< extent after it
    bool constantPower = true;
    std::string name;
};

struct PlanTrack {
    std::string name;
    TrackId track;
    int channel = -1;          ///< breakout: the channel of the Wabcut track; -1 otherwise
    std::vector<ClipRef> clips; ///< sorted, not overlapping
    std::vector<Fade> fades;    ///< sorted by cut
    Gain volume;                ///< track volume over [0, plan length)
    Pan pan;
};

struct VideoSource {
    std::string name;
    std::array<std::uint8_t, 16> material{};
    std::string path;          ///< absolute UTF-8 path
    std::int32_t width = 0;
    std::int32_t height = 0;
    Rate frameRate = kRate30;
    std::int64_t lengthFrames = 0; ///< in the sequence's frames
};

struct VideoClip {
    std::string name;
    std::int64_t start = 0;  ///< sequence frames
    std::int64_t length = 0;
    std::size_t source = 0;
    std::int64_t sourceStart = 0;
    std::string where;
};

struct VideoTrack {
    std::string name;
    std::vector<VideoClip> clips;
};

struct PlanMarker {
    std::int64_t frame = 0;
    std::int64_t lengthFrames = 0;
    std::string name;
    std::string comment;
    std::int32_t colorIndex = 0;
};

struct PlanOptions {
    bool breakout = true;
    bool clipEffects = false;
    bool cleanCopies = false;
    bool video = true;          ///< plan the video tracks (AAF)
    bool trackAutomation = true; ///< volume / pan keyframes as points (AAF); off: their value at the start (OMF)
    AudioPostAudio audio;
    std::filesystem::path projectFile;
};

struct Plan {
    std::string sequenceName;
    Rate frameRate = kRate30;
    bool dropFrame = false;
    std::int64_t startTimecodeFrames = 0;
    std::int64_t lengthFrames = 0;
    int sampleRate = 48000;
    int bits = 24;
    std::int64_t length = 0;    ///< samples
    std::vector<Source> sources;
    std::vector<RenderGroup> groups;
    std::vector<PlanTrack> tracks;
    std::vector<VideoSource> videoSources;
    std::vector<VideoTrack> videoTracks;
    std::vector<PlanMarker> markers;

    std::int64_t samples(Ticks t) const { return ticksToSamples(t, Rate{sampleRate, 1}); }
    std::int64_t frames(Ticks t) const { return ticksToFrames(t, frameRate); }
};

/// The plan of `sequence` (no IO). Report entries (locations name the sequence, track and clip) go to `report`.
/// Errors: NotFound (no such sequence).
std::expected<Plan, Error> makePlan(const ProjectState& project, SequenceId sequence, const PlanOptions& options,
                                    ImportReport& report, const std::string& fileLabel);

/// Where rendered files go.
struct RenderTarget {
    FileKind kind = FileKind::Wave;
    /// Separate audio: the folder of the final files; Embed: a temporary folder (deleted by the caller).
    std::filesystem::path folder;
    bool separate = false;
    std::string originator = "Wabcut";
    /// WAV files get a Broadcast WAV bext chunk (AAF); off: plain WAV (OMF's WAVE data).
    bool writeBext = true;
    Bext bextTemplate;          ///< description and UMID are filled per file
    std::int64_t timecodeOffsetSamples = 0; ///< the sequence's start timecode, for bext time references
    /// MobID material -> the UMID written into bext (the file source mob's MobID).
    std::function<std::array<std::uint8_t, 32>(const Source&)> umidOf;
};

/// Renders every group of `plan` through `renderer` into files under `target.folder`, filling the sources' file,
/// header and sizes. A group whose media is offline (NotFound, DecoderError) marks its sources failed and is reported
/// Dropped; Cancelled (the progress callback or the renderer) stops with nothing left behind. `progress` gets 0..1.
std::expected<void, Error> renderPlan(const ProjectState& project, Plan& plan, AudioPostRenderer& renderer,
                                      const RenderTarget& target, const std::function<bool(double)>& progress,
                                      ImportReport& report, const std::string& fileLabel);

/// Removes the clips whose sources failed (renderPlan), with the fades that need them.
void dropFailedClips(Plan& plan);

/// A file name of at most 120 UTF-8 bytes without characters Windows forbids.
std::string safeFileName(std::string_view name);

/// Linear amplitude of a dB value, as a rational for AAF/OMF (denominator 2^24, clamped to +-128x).
std::pair<std::int32_t, std::int32_t> amplitudeRational(double db);

} // namespace wabcut::interchange::detail::audiopost
