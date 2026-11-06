#pragma once

// The AI video effects on the command surface (0.7; docs/ai.md "AI video effects"):
//
//   effect.aiStatus    Where AI Restore / AI Denoise stand: per clip with AI effects, the chain, the models it needs (and
//                      whether they are on this computer), how many of its pictures are processed. A read.
//   effect.processAi   Process: makes the pictures of one clip (or every clip of the sequence that is incomplete) into the
//                      processed cache - a job with progress and cancel, like effect.analyzeAll. It changes nothing in the
//                      project (the pictures are derived data, kept in the cache), so it is no undo step.
//
// Services (findService): VideoServices gives the cache, the model backend (wabcut-embed's HelperBackend in the app and the
// CLI), the decoder-backed frame sources and the model store; without them effect.processAi is refused and effect.aiStatus
// reports the models and pictures as missing.

#include <wabcut/ai/video/Backend.h>
#include <wabcut/ai/video/ProcessedCache.h>
#include <wabcut/ai/video/Runner.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace wabcut::ai::video {

class VideoServices : public command::CommandServices {
public:
    using SourceFactory = std::function<std::unique_ptr<IFrameSource>(const std::string& path, int containerStream)>;
    VideoServices(std::shared_ptr<ProcessedCache> cache, std::shared_ptr<IVideoBackend> backend, SourceFactory sources,
                  std::filesystem::path modelRoot = {})
        : m_cache(std::move(cache)), m_backend(std::move(backend)), m_sources(std::move(sources)), m_modelRoot(std::move(modelRoot)) {}

    ProcessedCache* cache() const { return m_cache.get(); }
    IVideoBackend* backend() const { return m_backend.get(); }
    const SourceFactory& sources() const { return m_sources; }
    /// The model store root; empty = ModelStore::defaultRoot().
    const std::filesystem::path& modelRoot() const { return m_modelRoot; }

private:
    std::shared_ptr<ProcessedCache> m_cache;
    std::shared_ptr<IVideoBackend> m_backend;
    SourceFactory m_sources;
    std::filesystem::path m_modelRoot;
};

/// Adds effect.aiStatus and effect.processAi.
void registerVideoCommands(command::CommandRegistry& registry);

} // namespace wabcut::ai::video
