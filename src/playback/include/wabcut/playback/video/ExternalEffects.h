#pragma once

// External video effects (0.7, docs/plugins.md, "OpenFX video plug-ins"): effects of a clip's stack that a plug-in host runs
// on the clip's own decoded picture instead of the compositor's shaders.
//
// The evaluator (Evaluate.cpp) collects them per layer at the frame's curve time (LayerSpec::external) and
// buildCompositeLayers hands the layer's decoded frame to the installed processor, which returns the picture the plug-ins
// made: CPU pixels the compositor uploads like a title's (render::CompositeLayer::view). The processed picture replaces the
// frame before the layer's Motion, opacity and built-in effects, in the stack order of the plug-in effects among
// themselves. Preview and export build their composites with the same function, so both show the same picture.
//
// A processor that cannot run an effect (a plug-in that crashed, hung or is missing) returns an error, and the layer shows
// its frame as it was: the effect is bypassed.

#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>
#include <wabcut/render/FrameView.h>

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut::playback {

/// One external effect of a clip's stack at the frame's curve time.
struct ExternalEffect {
    EffectInstanceId id;
    std::string effect; ///< "plugin.ofx.<plug-in id>"
    /// The descriptor's numeric parameters by key at the frame's curve time (constants and keyframe curves alike).
    std::vector<std::pair<std::string, double>> params;
    /// The descriptor's text settings by key (EffectData text, else the default).
    std::vector<std::pair<std::string, std::string>> texts;
    double timeFrames = 0.0; ///< the clip's time at its sequence's frame rate, in frames (the plug-in's time)
    double fps = 24.0;       ///< the sequence's frame rate
};

/// The picture a stack of external effects runs on.
struct ExternalSource {
    const media::VideoFrame* frame = nullptr; ///< the decoded frame (CPU planes, or a decoder surface)
    /// To decode a CPU copy of a frame that is a decoder surface (zero-copy preview): the file, its container stream and
    /// the source time the frame shows.
    std::string path;
    int stream = -1;
    Ticks time{};
    /// The pixel aspect the frame is shown with (Interpret Footage's when set).
    double pixelAspect = 1.0;
};

class ExternalEffectProcessor {
public:
    virtual ~ExternalEffectProcessor() = default;
    /// Whether `effectId` is an effect this processor runs (a plug-in this computer has).
    virtual bool implements(std::string_view effectId) const = 0;
    /// Why `effect` will not show as the project asks (a plug-in this computer does not have, one that failed to load); empty
    /// when it will. An export warns about these before it renders.
    virtual std::string problem(const EffectInstance& effect) const {
        (void)effect;
        return {};
    }
    /// Runs `effects` in order on the picture of `source` and returns the result: CPU RGBA pixels (a FrameView that keeps
    /// them alive and carries the picture's colour tags). Errors: Unsupported (a picture the plug-ins cannot take, as HDR),
    /// IoError (a plug-in that crashed or hung), NotFound (a plug-in that is gone).
    virtual std::expected<render::FrameView, Error> process(const ExternalSource& source,
                                                            const std::vector<ExternalEffect>& effects) = 0;
};

/// Installs the process's processor (null: none, every external effect is bypassed). Thread-safe.
void setExternalEffectProcessor(std::shared_ptr<ExternalEffectProcessor> processor);
std::shared_ptr<ExternalEffectProcessor> externalEffectProcessor();

/// While one is alive on a thread, that thread's composites are an offline render (export, Render In to Out, review
/// frames): a plug-in may take much longer per frame than the player would wait for it.
class ScopedOfflineVideoRender {
public:
    ScopedOfflineVideoRender();
    ~ScopedOfflineVideoRender();
    ScopedOfflineVideoRender(const ScopedOfflineVideoRender&) = delete;
    ScopedOfflineVideoRender& operator=(const ScopedOfflineVideoRender&) = delete;

private:
    bool m_previous = false;
};
/// Whether the calling thread is rendering offline.
bool offlineVideoRender();

} // namespace wabcut::playback
