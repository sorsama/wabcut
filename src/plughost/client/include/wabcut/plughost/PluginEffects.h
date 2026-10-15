#pragma once

// Plugin effects in the effect stacks (A.6, docs/plugins.md): the audio engine's ExternalEffectFactory for
// "plugin.<format>.<id>" effects, running them in wabcut-plughost through PluginHost.
//
// The processor sends the plugin fixed blocks of kBlockFrames frames, aligned to the chain's stream, so preview and
// export give it the same blocks (and a deterministic plugin the same samples). Its output lags its input by the
// pipeline (one block where rendering runs ahead of playback, kLivePipelineFrames on the master bus, which the player
// runs in the audio callback) plus the plugin's own latency; it reports both, and the engine's latency compensation
// (plugin delay compensation) keeps everything aligned to the sample in preview and export alike.
//   * Parameters: the instance's own ("p<id>": constants or keyframe curves), sent as sample-accurate parameter events
//     at the parameter grid's cell starts when their value changes.
//   * State: the plugin's chunk, base64, in the instance's EffectData ("plugin.state.000", "plugin.state.001", ...;
//     at most kMaxStateBytes); restored when an instance is created.
//   * Channels: the plugin takes the chain's first two channels (a mono chain's one channel twice) and gives back two;
//     every other channel of the chain (C, LFE, Ls, Rs of a 5.1 master) passes through, delayed by the same latency.
//   * A missed deadline on the audio callback (the helper did not return a block in time) plays that block's dry signal,
//     delayed by the same latency, and is counted (PluginInstance::missedBlocks); the next block in time fades back to
//     the plugin over one block. A crashed, failed or hung instance is bypassed the same way, so nothing moves. Its
//     badge comes from status(). Where rendering runs ahead of playback a block may take HostOptions::
//     playbackBlockTimeout (an offline render: blockTimeout) before the instance counts as hung.
//   * Invalid output (NaN, infinity) is replaced by the dry signal and counted (PluginInstance::invalidBlocks); after
//     kMaxInvalidBlocks such blocks the instance fails. Other samples are clamped to +-kMaxOutputLevel.
//   * A plugin's latency is what it reports, at most 1 s (more fails the instance). When it asks for a restart with
//     another latency, the helper reads it again: a clip or track processor then goes stale and its chain rebuilds it
//     (keeping the plugin instance), a master processor reports it and adopts it at the master's next restart.
//   * A plugin this computer does not have is not evaluable (the model keeps it as an unknown effect): bypassed, badged
//     "missing" with the name it was saved with ("plugin.name").
//
// Loading (HostOptions::deferInstantiation): a processor created outside an offline render (audio::
// ScopedOfflineRender: export) whose plugin has no idle instance ready is returned at once and loads its plugin on a
// worker: until it is ready it plays the dry signal, delayed by the latency it reports (the latency of the last instance
// of this effect, else the scanned one), and then takes the plugin over at a block boundary, fading in over one block.
// When the loaded plugin reports another latency a clip or track processor goes stale (its chain rebuilds it with the
// instance) and a master processor adopts it at the master's next restart. In an offline render the plugin is loaded
// before the processor is returned, as it always was.
//
// Instances are pooled per effect instance: a processor that goes away (a clip voice leaving the screen, a refill)
// leaves its plugin instance idle for the next processor of the same effect, so plugins are not created again and
// again during playback, and an open editor window stays with the sound it edits.

#include <wabcut/plughost/PluginCatalog.h>
#include <wabcut/plughost/PluginHost.h>

#include <wabcut/audio/effects/EffectChain.h>

#include <wabcut/model/Effects.h>
#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::plughost {

/// EffectData keys of a plugin effect.
inline constexpr std::string_view kStateKeyPrefix = "plugin.state."; ///< + "000", "001", ...: base64 parts
inline constexpr std::string_view kPluginNameKey = "plugin.name";    ///< "Vendor Name" (for the missing-plugin badge)
/// Most state bytes a plugin effect keeps (base64 parts within the model's EffectData bounds).
inline constexpr std::size_t kMaxStateBytes = 45 * 1024;
/// Blocks with NaN or infinite output after which an instance fails (bypassed, badged).
inline constexpr std::uint64_t kMaxInvalidBlocks = 8;
/// Plugin output is clamped to this level (+24 dBFS).
inline constexpr float kMaxOutputLevel = 16.0f;

/// The plugin state stored in `effect` (base64; empty when none).
std::string pluginState(const EffectInstance& effect);
/// Stores `base64` as `effect`'s plugin state (replacing what was there). Fails when it is larger than kMaxStateBytes.
std::expected<void, Error> setPluginState(EffectInstance& effect, std::string_view base64);
/// A new plugin effect instance of `plugin` (its effect id, its name for the missing-plugin badge).
EffectInstance makePluginEffect(const PluginInfo& plugin, EffectInstanceId id);

enum class PluginEffectStatus : std::uint8_t {
    NotPlugin, ///< not a plugin effect
    Ready,     ///< scanned; runs (or will when it is played)
    Missing,   ///< not on this computer: kept, bypassed
    Crashed,
    Failed,
    Hung,
};
std::string_view toString(PluginEffectStatus status);

struct PluginEffectReport {
    PluginEffectStatus status = PluginEffectStatus::NotPlugin;
    std::string message; ///< for the badge: "Missing plugin: TDR Nova (Tokyo Dawn Labs)", "Crashed: ..."
    std::uint64_t missedBlocks = 0;
};

class PluginEffects final : public audio::ExternalEffectFactory, public std::enable_shared_from_this<PluginEffects> {
public:
    static std::shared_ptr<PluginEffects> create(std::shared_ptr<PluginHost> host, std::shared_ptr<PluginCatalog> catalog);
    ~PluginEffects() override;

    bool implements(std::string_view effectId) const override;
    std::unique_ptr<audio::EffectProcessor> create(const EffectInstance& instance, const EffectDescriptor& descriptor,
                                                   int channels, int sampleRate, audio::ChainContext context,
                                                   audio::EffectProcessor* replacing) override;
    /// Missing on this computer, or failed to load (an export warns about these before it renders); empty otherwise.
    std::string problem(const EffectInstance& instance) const override;

    /// Loads `effect`'s plugin now, into the pool, so its next processor starts with it at once (a project or sequence
    /// opening, on a worker; tests). Blocks on the helper: never on the GUI or audio thread.
    std::expected<void, Error> prepare(const EffectInstance& effect, int sampleRate = 48000);

    /// The live plugin instance playing `effect` (the most recent processor's), or null.
    std::shared_ptr<PluginInstance> instanceFor(EffectInstanceId effect) const;
    /// The badge of `effect` (any effect: NotPlugin for others).
    PluginEffectReport status(const EffectInstance& effect) const;
    /// Opens the plugin's editor for `effect` ("Edit plugin..."): on its live instance, or a new one from its state.
    /// Blocks on the helper (openEditorAsync() does not).
    std::expected<void, Error> openEditor(const EffectInstance& effect, double sampleRate = 48000.0);
    /// The current state of `effect`'s live instance (base64), to store with setPluginState(). The instance keeps
    /// playing (a processor rebuilt for the new data takes the same instance without reloading it). Blocks on the
    /// helper (captureStateAsync() does not).
    std::expected<std::string, Error> captureState(const EffectInstance& effect);
    /// openEditor() and captureState() on a worker: they return at once and `done` is called on the worker (post it to
    /// the GUI thread yourself) - the GUI never waits for a helper (a plugin in a modal dialog, a cold helper).
    void openEditorAsync(const EffectInstance& effect, double sampleRate, std::function<void(std::expected<void, Error>)> done);
    void captureStateAsync(const EffectInstance& effect, std::function<void(std::expected<std::string, Error>)> done);

    std::shared_ptr<PluginHost> host() const;
    std::shared_ptr<PluginCatalog> catalog() const;

    // Pool (PluginProcessor): `touched` are the parameters its processor may have sent (sorted).
    void release(EffectInstanceId effect, std::shared_ptr<PluginInstance> instance, std::vector<std::uint32_t> touched);

    struct State; ///< internal (PluginEffects.cpp)

private:
    PluginEffects(std::shared_ptr<PluginHost> host, std::shared_ptr<PluginCatalog> catalog);
    std::unique_ptr<State> m_state;
};

/// Makes `effects` the audio engine's external effect factory and its catalog's descriptors the model's plugin
/// descriptors (null: uninstalls the factory).
void installPluginEffects(const std::shared_ptr<PluginEffects>& effects);

} // namespace wabcut::plughost
