#include <wabcut/ai/edit/TranscriptSources.h>

#include <wabcut/ai/TranscriptJson.h>
#include <wabcut/model/MediaLocator.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace wabcut::ai::edit {

std::expected<Transcript, Error> readTranscriptFile(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return std::unexpected(Error{ErrorCode::NotFound, "no transcript file " + pathToUtf8(file)});
    }
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    if (!in) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read " + pathToUtf8(file)});
    }
    const std::string content = text.str();
    // An analysis-cache entry wraps the transcript in {"payload": ...}.
    const nlohmann::json j = nlohmann::json::parse(content, nullptr, false);
    if (j.is_object() && j.contains("payload") && j["payload"].is_object()) {
        return transcriptFromJson(j["payload"].dump());
    }
    return transcriptFromJson(content);
}

std::shared_ptr<const Transcript> TranscriptProviderChain::transcript(const MediaItem& media, int audioStream) const {
    for (const auto& provider : m_providers) {
        if (auto t = provider->transcript(media, audioStream)) {
            return t;
        }
    }
    return nullptr;
}

} // namespace wabcut::ai::edit
