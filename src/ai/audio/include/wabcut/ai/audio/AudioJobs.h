#pragma once

// The Enhance Speech and stem separation jobs (A.4; docs/ai.md "Enhance Speech" and "Stem separation"), behind the
// audio commands' SpeechEnhancer and StemSeparator (wabcut/audio/edit/AudioAnalysis.h). Offline, never real time:
//
// - Enhance Speech decodes a stream at 48 kHz from source time 0 with media::AudioDecoder (the decoder playback reads
//   the source and the take with, so the two line up sample for sample), enhances it in wabcut-embed chunk by chunk
//   (Plans.h: pre-roll, post-roll and a crossfade, so seams do not click; every request the same length, so DirectML
//   compiles its graphs once) and writes a float WAV take named after the file's fingerprint and the model: a second
//   request for the same file and model finds it and renders nothing.
// - Stem separation decodes at the model's 44.1 kHz stereo, separates 7.8 s segments with a quarter overlap and writes
//   one float WAV per stem. It first checks the computer can hold a segment's inference (AudioModels.h stemDevice) and
//   refuses with Unsupported otherwise.
//
// Models download on first use (ModelStore; DeepFilterNet3's archive is extracted and verified graph by graph). The
// helper runs below normal priority; a host hook (waitIdle) holds the jobs between requests while playback or an
// export runs (the editor's resource governor).

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/audio/AudioHelper.h>
#include <wabcut/ai/audio/AudioModels.h>
#include <wabcut/ai/audio/Plans.h>
#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/model/MediaLocator.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::audio {

struct LocalAudioOptions {
    std::filesystem::path modelRoot;                ///< the ModelStore's root; ModelStore::defaultRoot() when empty
    std::shared_ptr<IDownloadTransport> transport;  ///< first-use downloads; null: the models must be installed
    std::filesystem::path extractor;                ///< tar for DeepFilterNet3's archive; defaultExtractor() when empty
    std::filesystem::path takesDirectory;           ///< enhanced takes; <user cache>/enhanced when empty
    std::filesystem::path stemsDirectory;           ///< separated stems; <user cache>/stems when empty
    AudioHelperOptions helper;                      ///< the helper (its model paths are filled in)
    ChunkOptions chunks;                            ///< Enhance Speech's chunking
    /// What this computer has (stem separation's memory budget); llm::probeSystem when empty.
    std::function<llm::SystemProbe()> probe;
    /// Called before each helper request: blocks while playback or an export needs the machine (the resource
    /// governor); returns false to give up (the job ends Cancelled). Empty: never waits.
    std::function<bool(std::stop_token)> waitIdle;
};

/// The take of (`source` file, stream, model) in `directory`: <size>-<sample hash>-a<stream>-<model>.wav. The same file
/// and model always name the same take.
std::filesystem::path takeFileFor(const std::filesystem::path& directory, const SourceFingerprint& source, int streamOrdinal,
                                  std::string_view model);

/// Renders the enhanced take of a stream (`channels` channels, `duration` long, container index `streamIndex`) of the
/// file at `sourcePath` into `take` (written as <take>.part, renamed when complete). Returns its frames. Errors: the
/// decoder's, the helper's, IoError, Cancelled.
std::expected<std::int64_t, Error> renderEnhancedTake(const std::string& sourcePath, int streamIndex, int channels, Ticks duration,
                                                      const std::filesystem::path& take, AudioHelper& helper,
                                                      const ChunkOptions& chunks, std::stop_token stop,
                                                      const std::function<void(double)>& progress = {},
                                                      const std::function<bool(std::stop_token)>& waitIdle = {});

/// Separates a stream of the file at `sourcePath` into `spec.stems` float WAVs <directory>/<stem>.wav (44.1 kHz stereo).
/// Errors as renderEnhancedTake.
std::expected<std::vector<std::filesystem::path>, Error> separateStemsOf(const std::string& sourcePath, int streamIndex, Ticks duration,
                                                                         const std::filesystem::path& directory, AudioHelper& helper,
                                                                         const StemModelSpec& spec, std::stop_token stop,
                                                                         const std::function<void(double)>& progress = {},
                                                                         const std::function<bool(std::stop_token)>& waitIdle = {});

/// The local implementations (one helper each, started on first use, one job at a time).
std::shared_ptr<wabcut::audio::edit::SpeechEnhancer> makeSpeechEnhancer(LocalAudioOptions options);
std::shared_ptr<wabcut::audio::edit::StemSeparator> makeStemSeparator(LocalAudioOptions options);

} // namespace wabcut::ai::audio
