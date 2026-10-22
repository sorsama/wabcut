#pragma once

// The host side of AAF and OMF export (interchange/AudioPostExport.h, P.2): what the pure-C++ writers cannot do
// themselves.
//
//  * ClipAudioRenderer renders one clip's audio through the playback path the preview and every export use
//    (playback::TrackRenderer over MediaAudioFeed): its media at its speed (reverse, time remaps), channel map and
//    pan, Enhance Speech takes and redaction bleeps, and - when asked - its effect stack. Its Volume is left out (the
//    AAF carries it as clip gain), and so are the track's effects, fader and panner (the mix happens downstream).
//    Handles: the clip's media continues past its edges for the range asked.
//  * ReferenceVideoRenderer renders Mixdown Video: the sequence as a DNxHR LB QuickTime with its stereo mix as 24-bit
//    PCM, "<export name> Reference.mov" next to the AAF (Premiere writes an MXF with DNxHD/DNxHR; a MOV opens in every
//    audio application that takes a reference video).

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/interchange/AudioPostExport.h>

#include <filesystem>
#include <functional>
#include <memory>

namespace wabcut::exporter {

class ClipAudioRenderer final : public interchange::AudioPostRenderer {
public:
    /// `projectFile` resolves media paths relative to the project (MediaLocator::relativePath); may be empty.
    explicit ClipAudioRenderer(std::filesystem::path projectFile = {});
    std::expected<void, Error> render(const ProjectState& project, const interchange::AudioPostRenderRequest& request,
                                      const std::function<bool(std::span<const float>)>& sink) override;

private:
    std::filesystem::path m_projectFile;
};

class ReferenceVideoRenderer final : public interchange::AudioPostVideoRenderer {
public:
    /// `videoSource` empty: the production renderer (defaultVideoSourceFactory).
    explicit ReferenceVideoRenderer(std::filesystem::path projectFile = {}, VideoSourceFactory videoSource = {});
    std::expected<std::filesystem::path, Error> render(const ProjectState& project, SequenceId sequence,
                                                       const std::filesystem::path& exportFile,
                                                       const std::function<bool(double)>& progress) override;

private:
    std::filesystem::path m_projectFile;
    VideoSourceFactory m_videoSource;
};

/// The settings of the reference video (DNxHR LB, 4:2:2, MOV, PCM 24-bit stereo at 48 kHz).
ExportSettings referenceVideoSettings();

/// What the interchange commands need from a host with this module (the app, wabcut-cli): AudioPostServices with the
/// two renderers above for interchange.export's AAF and OMF, and CaptionFileServices for captions.export (redacted
/// words replaced as in exports, captionTrackForFile). Bundle it with the engine's other services.
std::shared_ptr<command::CommandServices> interchangeServices(const std::filesystem::path& projectFile);

} // namespace wabcut::exporter
