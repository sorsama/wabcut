#pragma once

// OpenFX effects in the video effect stacks (0.7, docs/plugins.md, "OpenFX video plug-ins"): the playback layer's
// ExternalEffectProcessor for "plugin.ofx.<id>" effects, running them in wabcut-ofxhost through OfxPluginHost.
//
// For a layer with OpenFX effects the processor takes the layer's decoded frame (a CPU copy is decoded when the preview
// keeps decoder surfaces), makes the float RGBA a plug-in takes, runs each effect of the stack in order through its helper
// instance and hands back the 8-bit raster the compositor draws (OfxPictures.h says what colour it is in).
//   * Results are cached (a few seconds of pictures, bounded in bytes) by the source frame and the effects' parameters, so
//     a paused frame, a seek back, a loop and the export of a preview do not run the plug-ins again.
//   * One helper instance per effect instance of the project (the clip's own settings), pooled and closed when idle.
//   * A plug-in that crashes, hangs or keeps failing is bypassed: its effect shows the badge (status()), the layer shows
//     the picture without it, and nothing else is touched. retry() takes the badge off and tries again.
//   * A plug-in this computer does not have is not evaluable (the model keeps it as an unknown effect, saved unchanged):
//     bypassed, badged "missing" with the name it was saved with ("plugin.name").

#include <wabcut/ofxhost/OfxCatalog.h>
#include <wabcut/ofxhost/OfxPluginHost.h>

#include <wabcut/playback/video/ExternalEffects.h>

#include <wabcut/model/Effects.h>
#include <wabcut/model/Error.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace wabcut::ofxhost {

enum class OfxEffectStatus : std::uint8_t {
    NotOfx,  ///< not an OpenFX effect
    Ready,   ///< scanned; runs (or will when it is played)
    Missing, ///< not on this computer: kept, bypassed
    Crashed,
    Failed,
    Hung,
};
std::string_view toString(OfxEffectStatus status);

struct OfxEffectReport {
    OfxEffectStatus status = OfxEffectStatus::NotOfx;
    std::string message; ///< for the badge: "Missing plug-in: Invert (Examples)", "Crashed: the plug-in crashed (...)"
    double lastMilliseconds = 0.0; ///< the helper's time for the effect's last picture
};

/// A new OpenFX effect instance of `plugin` (its effect id, its name for the missing-plug-in badge).
EffectInstance makeOfxEffect(const OfxPluginInfo& plugin, EffectInstanceId id);

struct OfxCacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t pictures = 0; ///< pictures run through plug-ins
    std::size_t entries = 0;
    std::size_t bytes = 0;
};

class OfxEffects final : public playback::ExternalEffectProcessor, public std::enable_shared_from_this<OfxEffects> {
public:
    static std::shared_ptr<OfxEffects> create(std::shared_ptr<OfxPluginHost> host, std::shared_ptr<OfxCatalog> catalog);
    ~OfxEffects() override;

    bool implements(std::string_view effectId) const override;
    std::expected<render::FrameView, Error> process(const playback::ExternalSource& source,
                                                    const std::vector<playback::ExternalEffect>& effects) override;

    /// The badge of `effect` (any effect: NotOfx for others).
    OfxEffectReport status(const EffectInstance& effect) const;
    /// Missing on this computer, or failed to load (an export warns about these before it renders); empty otherwise.
    std::string problem(const EffectInstance& effect) const override;
    /// Takes a crashed, hung or failed effect's helper instance away, so the next picture makes a new one (and a new helper
    /// when its process died).
    void retry(EffectInstanceId effect);
    /// Bumped whenever an effect's status may have changed (a crash, a retry, the catalog): poll it to refresh badges.
    std::uint64_t statusRevision() const noexcept;
    /// Called, on whatever thread it happened on, each time the revision is bumped (the UI posts it to its own thread).
    void setChangeListener(std::function<void()> listener);

    OfxCacheStats cacheStats() const;
    void clearCache();
    /// The most bytes of processed pictures the cache keeps (default 192 MiB).
    void setCacheBudget(std::size_t bytes);

    std::shared_ptr<OfxPluginHost> host() const;
    std::shared_ptr<OfxCatalog> catalog() const;

    struct State; ///< internal (OfxEffects.cpp)

private:
    OfxEffects(std::shared_ptr<OfxPluginHost> host, std::shared_ptr<OfxCatalog> catalog);
    std::unique_ptr<State> m_state;
};

} // namespace wabcut::ofxhost
