#pragma once

// Transcripts from files and from several providers (no media access: the analysis cache lookup by source
// fingerprint is CachedTranscriptProvider in MediaServices.h).

#include <wabcut/ai/Transcript.h>
#include <wabcut/ai/edit/SequenceTranscript.h>
#include <wabcut/model/Error.h>

#include <expected>
#include <filesystem>
#include <memory>
#include <vector>

namespace wabcut::ai::edit {

/// A transcript file: the JSON of ai::toJson(Transcript), or an analysis-cache entry ({"payload": ...}).
/// Errors: NotFound, IoError, ParseError (as transcriptFromJson).
std::expected<Transcript, Error> readTranscriptFile(const std::filesystem::path& file);

/// The first provider that has a transcript.
class TranscriptProviderChain : public TranscriptProvider {
public:
    void add(std::shared_ptr<const TranscriptProvider> provider) { m_providers.push_back(std::move(provider)); }
    std::shared_ptr<const Transcript> transcript(const MediaItem& media, int audioStream) const override;

private:
    std::vector<std::shared_ptr<const TranscriptProvider>> m_providers;
};

} // namespace wabcut::ai::edit
