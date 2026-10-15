#pragma once

// The media-backed services of the transcript commands (full builds only: wabcut_ai_edit_media links wabcut_media).
//
//   CachedTranscriptProvider  the transcripts AI-2's TranscriptionService stored in the analysis cache, found by the
//                             media's source fingerprint and container stream (the newest entry of any model).
//   DecoderEnergyProbe        RMS level of a source range, decoded at 16 kHz mono with media::AudioDecoder.
//   ServiceMediaTranscriber   transcript.transcribe over ai::TranscribeService (local / cloud / cache).
//   ServiceEnergyProbe        the silence check over ai::AudioEnergyService: what a measure() job found, else measured
//                             on the spot (and cached).
//
// Media files are found through their locator (absolute path, else the path relative to the project file).

#include <wabcut/ai/AudioEnergy.h>
#include <wabcut/ai/TranscribeService.h>
#include <wabcut/ai/edit/Review.h>
#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/edit/TranscriptSources.h>

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace wabcut::ai::edit {

class CachedTranscriptProvider : public TranscriptProvider {
public:
    /// `cacheRoot`: the analysis cache (AnalysisCache::defaultRoot()); `projectFile` resolves relative media paths.
    CachedTranscriptProvider(std::filesystem::path cacheRoot, std::filesystem::path projectFile);
    std::shared_ptr<const Transcript> transcript(const MediaItem& media, int audioStream) const override;

private:
    std::filesystem::path m_root;
    std::filesystem::path m_projectFile;
    mutable std::mutex m_mutex;
    struct Found {
        std::shared_ptr<const Transcript> transcript; ///< null: none when last looked
        std::uint64_t writes = 0;                     ///< AnalysisCache::writeCount() then
        std::filesystem::path file;                   ///< the entry it came from
        std::filesystem::file_time_type fileTime{};
    };
    /// By media file and stream (a relinked media item finds the new file's transcripts).
    mutable std::map<std::pair<std::string, int>, Found> m_found;
};

class DecoderEnergyProbe : public AudioEnergyProbe {
public:
    explicit DecoderEnergyProbe(std::filesystem::path projectFile);
    ~DecoderEnergyProbe() override;
    std::optional<double> rmsDb(const MediaItem& media, int audioStream, TimeRange source) const override;

private:
    struct Decoders;
    std::filesystem::path m_projectFile;
    std::unique_ptr<Decoders> m_decoders;
};

class ServiceMediaTranscriber : public MediaTranscriber {
public:
    /// `allowOnce`: the host has the user's "Allow once" for cloud uploads of this session (CallOptions). It is host
    /// policy, never a command parameter, so an agent cannot grant itself consent.
    ServiceMediaTranscriber(std::shared_ptr<TranscribeService> service, std::filesystem::path projectFile,
                            bool allowOnce = false);
    std::expected<Result, Error> transcribe(const MediaItem& media, int audioStream, const Options& options,
                                            const std::function<bool(double)>& progress) override;
    bool hasTranscript(const MediaItem& media, int audioStream) const override;
    std::vector<Provider> providers() const override;
    std::expected<std::string, Error> resolve(std::string_view provider) const override;

private:
    std::shared_ptr<TranscribeService> m_service;
    std::filesystem::path m_projectFile;
    bool m_allowOnce = false;
};

class ServiceEnergyProbe : public AudioEnergyProbe {
public:
    ServiceEnergyProbe(std::shared_ptr<AudioEnergyService> service, std::filesystem::path projectFile);
    std::optional<double> rmsDb(const MediaItem& media, int audioStream, TimeRange source) const override;
    /// Measures the ranges with one AudioEnergyService run per media file and stream (cached for rmsDb()).
    void prefetch(std::span<const Range> ranges) const override;

private:
    std::shared_ptr<AudioEnergyService> m_service;
    std::filesystem::path m_projectFile;
};

/// The media file of a media item (its locator: absolute, else relative to `projectFile`; else the probed path),
/// UTF-8, or nullopt when it is missing.
std::optional<std::string> mediaFilePath(const MediaItem& media, const std::filesystem::path& projectFile);
/// The container index of the media's audio stream `audioStream` (MediaInfo::audio), -1 when not recorded.
int audioContainerStream(const MediaItem& media, int audioStream);

} // namespace wabcut::ai::edit
