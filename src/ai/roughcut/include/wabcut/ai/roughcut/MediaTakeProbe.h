#pragma once

// Take audio for full builds (wabcut_ai_roughcut_media): level and peak of each take, and of the pauses around it for
// the noise floor, measured by AI-7's AudioEnergyService (16 kHz mono decode, one pass per media file and stream,
// cached in memory and in the analysis cache).

#include <wabcut/ai/AudioEnergy.h>
#include <wabcut/ai/roughcut/Speech.h>

#include <filesystem>
#include <memory>

namespace wabcut::ai::roughcut {

class ServiceTakeProbe final : public TakeAudioProbe {
public:
    ServiceTakeProbe(std::shared_ptr<AudioEnergyService> service, std::filesystem::path projectFile);
    AudioStats stats(const MediaItem& media, int audioStream, TimeRange source) const override;
    /// One AudioEnergyService run per media file and stream for every range asked for later.
    void prefetch(std::span<const edit::AudioEnergyProbe::Range> ranges) const override;

private:
    std::shared_ptr<AudioEnergyService> m_service;
    std::filesystem::path m_projectFile;
};

} // namespace wabcut::ai::roughcut
