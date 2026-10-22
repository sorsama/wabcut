#pragma once

// Hand-off to audio post: a sequence as an AAF (Pro Tools, Nuendo, Resolve / Fairlight, Media Composer) or an OMF 2.0
// file (older Pro Tools sessions), with its audio rendered to new files the way Premiere Pro's File > Export > AAF and
// File > Export > OMF do it.
//
//     interchange::AafExportOptions options;           // Premiere's AAF Export Settings
//     options.audio.files = interchange::AudioPostFiles::Separate;
//     auto result = interchange::exportAafToFile(project, sequence, "Mix.aaf", options, host);
//     // result->report: carried / approximated / dropped; result->audioFiles: the WAV/AIFF files written.
//
// Wabcut's model is pure C++ and cannot decode media, so the audio itself comes from the host (AudioPostHost): the app
// and wabcut-cli render each clip through the same audio engine preview and export use (exporter::makeAudioPostHost).
// Without a host, an AAF links the original media files instead (Breakout to Mono off) and says so in the report;
// OMF needs the host.
//
// What goes into the file (both formats unless noted):
//  * the sequence's start timecode, its markers (AAF), clip names, and its audio tracks in order;
//  * clips with their position, length and handles; disabled clips are left out (reported);
//  * audio crossfades and fades as the format's transitions (at the edit point, one-sided ones against filler);
//  * clip volume as clip gain (keyframes as gain automation), track volume and pan as track automation (AAF;
//    OMF: constant volume and pan only). Clip pan, speed, reverse, time remaps and (with Render Audio Clip Effects)
//    the clip's effects are rendered into the new audio;
//  * AAF: video tracks as clips linked to their original media, or with Mixdown Video one reference video file.
// Everything else (track effects, the master bus, nested sequences' structure, ...) is reported Approximated or Dropped.

#include <wabcut/interchange/ImportReport.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::interchange {

/// Premiere's "Files": the audio inside the file, or in a folder next to it.
enum class AudioPostFiles : std::uint8_t { Embed, Separate };
/// Premiere's "Format": Broadcast WAV (.wav with a bext chunk) or AIFF (.aif).
enum class AudioPostFormat : std::uint8_t { BroadcastWave, Aiff };
/// Premiere's "Render": whole media files, or only the parts the sequence uses plus handles.
enum class AudioPostRender : std::uint8_t { CopyCompleteAudioFiles, TrimAudioFiles };

std::string_view toString(AudioPostFiles files);     ///< "embed", "separate"
std::string_view toString(AudioPostFormat format);   ///< "wav", "aiff"
std::string_view toString(AudioPostRender render);   ///< "complete", "trim"
std::optional<AudioPostFiles> audioPostFilesFromString(std::string_view text);
std::optional<AudioPostFormat> audioPostFormatFromString(std::string_view text);
std::optional<AudioPostRender> audioPostRenderFromString(std::string_view text);

/// The new audio files (Premiere: Sample Rate, Bits per Sample, Files, Format, Render, Handle Frames).
struct AudioPostAudio {
    int sampleRate = 48000;      ///< 48000 or 96000
    int bitsPerSample = 24;      ///< 16 or 24
    AudioPostFiles files = AudioPostFiles::Embed;
    AudioPostFormat format = AudioPostFormat::BroadcastWave;
    AudioPostRender render = AudioPostRender::TrimAudioFiles;
    int handleFrames = 24;       ///< Trim: frames of the sequence's rate before and after each clip (0..1000)
};

/// Premiere's AAF Export Settings.
struct AafExportOptions {
    /// Mixdown Video: the host renders the video tracks to one reference video next to the AAF (the AAF's V1 plays it).
    /// Off: video clips link to their original files.
    bool mixdownVideo = false;
    /// Breakout to Mono (Enable): new mono audio files, stereo and 5.1 tracks split into one AAF track per channel
    /// ("Dialogue Left", "Dialogue Right"). Off: clips link to the original media, channels as they are.
    bool breakoutToMono = true;
    /// Render audio clip effects into the new files (else they are left out, and reported).
    bool renderAudioClipEffects = false;
    /// With renderAudioClipEffects: a clean copy of every track, without effects ("<track> No FX"; the rendered tracks
    /// become "<track> FX").
    bool includeClipCopiesWithoutEffects = false;
    AudioPostAudio audio;
    /// The project file, for media whose absolute path is missing (MediaLocator::relativePath). Empty: absolute only.
    std::filesystem::path projectFile;
    /// Creation and modification time written into the file; default: now (tests fix it).
    std::optional<std::chrono::system_clock::time_point> timestamp;
    /// The product version the file names (Identification); empty: this build's (tests fix it).
    std::string productVersion;
};

/// Premiere's OMF Export Settings.
struct OmfExportOptions {
    std::string title;           ///< OMF Title (the composition's name); empty: the sequence's name
    /// Files: Encapsulate (Embed: inside the OMF file) or Separate Audio (AIFF or WAV next to it). Handles as for AAF.
    AudioPostAudio audio;
    bool includePan = true;      ///< Include Pan: the tracks' pan as OMF pan values
    std::filesystem::path projectFile;
    std::optional<std::chrono::system_clock::time_point> timestamp;
    std::string productVersion;
};

/// One clip's audio, as the host should render it.
struct AudioPostRenderRequest {
    SequenceId sequence;
    TrackId track;
    ClipId clip;
    /// Timeline range to render: the clip and its handles, which may reach past the clip's edges (the clip's media,
    /// at its speed, continues there).
    TimeRange range;
    /// Channels of the result: the clip as its track hears it (its channel map, clip pan; the track's channel count).
    int channels = 2;
    int sampleRate = 48000;
    /// Run the clip's effect stack (Render Audio Clip Effects); its Volume never (the file carries it as clip gain).
    bool clipEffects = false;
};

/// Renders clips' audio for an export (the host's audio engine).
class AudioPostRenderer {
public:
    virtual ~AudioPostRenderer() = default;
    /// Renders `request`: exactly frames = ticksToSamples(range.duration) interleaved float frames (-1..1), handed to
    /// `sink` in order, in blocks. `sink` returns false to stop (cancel). Errors: NotFound (offline media), DecoderError,
    /// Cancelled.
    virtual std::expected<void, Error> render(const ProjectState& project, const AudioPostRenderRequest& request,
                                              const std::function<bool(std::span<const float>)>& sink) = 0;
};

/// Renders the reference video (Mixdown Video): the sequence's picture (and mix) to a file next to `exportFile`.
class AudioPostVideoRenderer {
public:
    virtual ~AudioPostVideoRenderer() = default;
    /// Returns the file written. `progress` (0..1) returns false to cancel.
    virtual std::expected<std::filesystem::path, Error> render(const ProjectState& project, SequenceId sequence,
                                                               const std::filesystem::path& exportFile,
                                                               const std::function<bool(double)>& progress) = 0;
};

struct AudioPostHost {
    AudioPostRenderer* audio = nullptr;
    AudioPostVideoRenderer* video = nullptr;
    /// Progress 0..1 of the whole export; false cancels it (Cancelled, nothing is left behind).
    std::function<bool(double)> progress;
    /// Plan only: the report and the track and clip counts, nothing rendered or written.
    bool dryRun = false;
};

struct AudioPostResult {
    std::filesystem::path file;
    std::uint64_t bytes = 0;
    /// Carried / approximated / dropped, like an import's.
    ImportReport report;
    /// Separate Audio: the files written (Embed: none).
    std::vector<std::filesystem::path> audioFiles;
    /// Mixdown Video: the reference video.
    std::optional<std::filesystem::path> referenceVideo;
    std::size_t tracks = 0;  ///< audio tracks written (after breakout)
    std::size_t clips = 0;   ///< audio clips written
};

/// Writes `sequence` of `project` as an AAF file (and, with Separate Audio, its audio files). Errors: NotFound (no such
/// sequence), InvalidArgument (options out of range), Unsupported, IoError, Cancelled, and the renderers' errors.
std::expected<AudioPostResult, Error> exportAafToFile(const ProjectState& project, SequenceId sequence,
                                                      const std::filesystem::path& file,
                                                      const AafExportOptions& options = {},
                                                      const AudioPostHost& host = {});

/// Writes `sequence` as an OMF 2.0 file (Bento container). The audio is always rendered: without host.audio the export
/// fails (Unsupported).
std::expected<AudioPostResult, Error> exportOmfToFile(const ProjectState& project, SequenceId sequence,
                                                      const std::filesystem::path& file,
                                                      const OmfExportOptions& options = {},
                                                      const AudioPostHost& host = {});

} // namespace wabcut::interchange
