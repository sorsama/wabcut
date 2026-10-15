#include <wabcut/ai/roughcut/MediaTakeProbe.h>

#include <wabcut/ai/edit/MediaServices.h>

#include <map>

namespace wabcut::ai::roughcut {

ServiceTakeProbe::ServiceTakeProbe(std::shared_ptr<AudioEnergyService> service, std::filesystem::path projectFile)
    : m_service(std::move(service)), m_projectFile(std::move(projectFile)) {}

AudioStats ServiceTakeProbe::stats(const MediaItem& media, int audioStream, TimeRange source) const {
    AudioStats out;
    const auto path = edit::mediaFilePath(media, m_projectFile);
    if (!m_service || !path || source.duration <= Ticks{}) {
        return out;
    }
    const int stream = edit::audioContainerStream(media, audioStream);
    std::optional<EnergyMeasure> measure = m_service->cached(*path, stream, source);
    if (!measure) {
        auto run = m_service->run(EnergyRequest{*path, stream, {source}, false});
        if (run && !run->ranges.empty()) {
            measure = run->ranges.front();
        }
    }
    if (measure) {
        out.rmsDb = measure->rmsDb;
        out.peakDb = measure->peakDb;
    }
    return out;
}

void ServiceTakeProbe::prefetch(std::span<const edit::AudioEnergyProbe::Range> ranges) const {
    if (!m_service) {
        return;
    }
    std::map<std::pair<std::string, int>, std::vector<TimeRange>> byStream;
    for (const auto& r : ranges) {
        if (r.media == nullptr || r.source.duration <= Ticks{}) {
            continue;
        }
        if (const auto path = edit::mediaFilePath(*r.media, m_projectFile)) {
            const int stream = edit::audioContainerStream(*r.media, r.audioStream);
            if (!m_service->cached(*path, stream, r.source)) {
                byStream[{*path, stream}].push_back(r.source);
            }
        }
    }
    for (auto& [key, list] : byStream) {
        [[maybe_unused]] const auto measured = m_service->run(EnergyRequest{key.first, key.second, std::move(list), false});
    }
}

} // namespace wabcut::ai::roughcut
