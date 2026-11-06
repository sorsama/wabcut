#include <wabcut/playback/video/ExternalEffects.h>

#include <mutex>

namespace wabcut::playback {

namespace {

std::mutex gMutex;
std::shared_ptr<ExternalEffectProcessor> gProcessor;
thread_local bool tlsOffline = false;

} // namespace

void setExternalEffectProcessor(std::shared_ptr<ExternalEffectProcessor> processor) {
    std::lock_guard lock(gMutex);
    gProcessor = std::move(processor);
}

std::shared_ptr<ExternalEffectProcessor> externalEffectProcessor() {
    std::lock_guard lock(gMutex);
    return gProcessor;
}

ScopedOfflineVideoRender::ScopedOfflineVideoRender() : m_previous(tlsOffline) { tlsOffline = true; }
ScopedOfflineVideoRender::~ScopedOfflineVideoRender() { tlsOffline = m_previous; }

bool offlineVideoRender() { return tlsOffline; }

} // namespace wabcut::playback
