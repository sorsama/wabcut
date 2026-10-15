#include "AppAudioServices.h"

#include "AiController.h"
#include "TranscriptBridge.h"

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/HttpDownload.h>
#include <wabcut/ai/audio/AudioJobs.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/audio/edit/AudioSync.h>
#include <wabcut/audio/edit/MediaAnalysis.h>
#include <wabcut/command/Command.h>
#include <wabcut/command/ExecutionContext.h>

#include <vector>

namespace wabcut::app {

std::filesystem::path projectMediaDirectory(const std::filesystem::path& projectFile, std::string_view leaf) {
    if (projectFile.empty())
        return {};
    std::filesystem::path dir = projectFile.parent_path() / projectFile.stem();
    dir += ".wabcut-media";
    return dir / std::filesystem::path(std::string(leaf));
}

std::shared_ptr<command::CommandServices> appAudioServices(const AppAudioServiceOptions& options) {
    std::shared_ptr<ai::edit::TranscriptServices> transcripts =
        options.transcripts ? options.transcripts->services() : nullptr;
    auto audio = audio::edit::makeMediaAudioServices(transcripts ? transcripts->transcripts : nullptr,
                                                     std::make_shared<ai::AnalysisCache>(ai::AnalysisCache::defaultRoot()));
    ai::audio::LocalAudioOptions local;
    if (options.ai)
        local.transport = std::make_shared<ai::HttpDownloadTransport>(options.ai->transport());
    local.takesDirectory = projectMediaDirectory(options.projectFile, "enhanced");
    local.stemsDirectory = projectMediaDirectory(options.projectFile, "stems");
    audio->enhancer = ai::audio::makeSpeechEnhancer(local);
    audio->separator = ai::audio::makeStemSeparator(local);
    std::vector<std::shared_ptr<command::CommandServices>> parts;
    parts.push_back(std::make_shared<command::MulticamServices>(std::make_shared<audio::edit::MulticamAudioSync>(audio->sync)));
    if (transcripts)
        parts.push_back(transcripts);
    parts.push_back(std::move(audio));
    return std::make_shared<command::ServiceBundle>(std::move(parts));
}

} // namespace wabcut::app
