// A minimal VST3 effect for the plugin-host tests (A.6), built like the VST3 SDK's AGain example: a processor
// (Steinberg::Vst::AudioEffect), an edit controller (EditController) and the SDK's plugin factory macros.
//
//   param 0 "Gain"   normalised 0..1 = -60..12 dB, automatable, sample-accurate (parameter changes at their offsets)
//   param 1 "Crash"  on > 0.5 the next process() dereferences null
//   latency          frames, from the state (getLatencySamples): a plain delay
// State (component): gain, crash, latency.

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <base/source/fstreamer.h>
#include <pluginterfaces/base/funknownimpl.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/ustring.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <public.sdk/source/vst/vstaudioeffect.h>
#include <public.sdk/source/main/pluginfactory.h>
#include <public.sdk/source/vst/vsteditcontroller.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;

const FUID kProcessorUID(0x5A1C2B3D, 0x4E5F4061, 0x8273A4B5, 0xC6D7E8F9);
const FUID kControllerUID(0x6B2D3C4E, 0x5F607182, 0x93A4B5C6, 0xD7E8F90A);

constexpr ParamID kGain = 0;
constexpr ParamID kCrash = 1;
constexpr double kGainDefault = 60.0 / 72.0; // 0 dB

double gainDb(double normalised) { return -60.0 + 72.0 * std::clamp(normalised, 0.0, 1.0); }

struct Stored {
    double gain = kGainDefault;
    double crash = 0.0;
    std::int32_t latency = 0;
};

bool readState(IBStream* stream, Stored& out) {
    IBStreamer s(stream, kLittleEndian);
    return s.readDouble(out.gain) && s.readDouble(out.crash) && s.readInt32(out.latency);
}

class Processor final : public AudioEffect {
public:
    Processor() { setControllerClass(kControllerUID); }

    static FUnknown* make(void*) { return static_cast<IAudioProcessor*>(new Processor()); }

    tresult PLUGIN_API initialize(FUnknown* context) override {
        const tresult r = AudioEffect::initialize(context);
        if (r != kResultOk) {
            return r;
        }
        addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
        return kResultOk;
    }

    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == kSample32 ? kResultTrue : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return static_cast<uint32>(m_state.latency); }

    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        for (auto& d : m_delay) {
            d.assign(static_cast<std::size_t>(std::max(m_state.latency, 0)) + 1, 0.0f);
        }
        m_pos = 0;
        return AudioEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setProcessing(TBool state) override {
        if (state) {
            for (auto& d : m_delay) {
                std::fill(d.begin(), d.end(), 0.0f);
            }
            m_pos = 0;
        }
        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData& data) override {
        // Parameter changes, sample-accurate: (offset, id, value) in offset order.
        struct Change {
            int32 offset;
            ParamID id;
            double value;
        };
        Change changes[64];
        int count = 0;
        if (IParameterChanges* in = data.inputParameterChanges) {
            for (int32 q = 0; q < in->getParameterCount() && count < 64; ++q) {
                IParamValueQueue* queue = in->getParameterData(q);
                for (int32 p = 0; queue != nullptr && p < queue->getPointCount() && count < 64; ++p) {
                    int32 offset = 0;
                    ParamValue value = 0;
                    if (queue->getPoint(p, offset, value) == kResultOk) {
                        changes[count++] = Change{offset, queue->getParameterId(), value};
                    }
                }
            }
        }
        std::sort(changes, changes + count, [](const Change& a, const Change& b) { return a.offset < b.offset; });
        if (data.numInputs < 1 || data.numOutputs < 1 || data.inputs[0].numChannels < 2 || data.outputs[0].numChannels < 2) {
            return kResultOk;
        }
        float** in = data.inputs[0].channelBuffers32;
        float** out = data.outputs[0].channelBuffers32;
        const std::size_t length = m_delay[0].size();
        int next = 0;
        for (int32 k = 0; k < data.numSamples; ++k) {
            while (next < count && changes[next].offset <= k) {
                if (changes[next].id == kGain) {
                    m_state.gain = changes[next].value;
                } else if (changes[next].id == kCrash) {
                    m_state.crash = changes[next].value;
                }
                ++next;
            }
            if (m_state.crash > 0.5) {
                volatile int* nowhere = nullptr;
                *nowhere = 42; // the deliberate crash
            }
            const auto g = static_cast<float>(std::pow(10.0, gainDb(m_state.gain) / 20.0));
            for (int c = 0; c < 2; ++c) {
                m_delay[c][m_pos] = in[c][k];
                out[c][k] = m_delay[c][(m_pos + 1) % length] * g;
            }
            m_pos = (m_pos + 1) % length;
        }
        return kResultOk;
    }

    tresult PLUGIN_API setState(IBStream* state) override {
        Stored s;
        if (!readState(state, s)) {
            return kResultFalse;
        }
        m_state = s;
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream* state) override {
        IBStreamer s(state, kLittleEndian);
        s.writeDouble(m_state.gain);
        s.writeDouble(m_state.crash);
        s.writeInt32(m_state.latency);
        return kResultOk;
    }

private:
    Stored m_state;
    std::vector<float> m_delay[2]{std::vector<float>(1, 0.0f), std::vector<float>(1, 0.0f)};
    std::size_t m_pos = 0;
};

class Controller final : public EditController {
public:
    static FUnknown* make(void*) { return static_cast<IEditController*>(new Controller()); }

    tresult PLUGIN_API initialize(FUnknown* context) override {
        const tresult r = EditController::initialize(context);
        if (r != kResultOk) {
            return r;
        }
        parameters.addParameter(STR16("Gain"), STR16("dB"), 0, kGainDefault, ParameterInfo::kCanAutomate, kGain);
        parameters.addParameter(STR16("Crash"), nullptr, 1, 0.0, ParameterInfo::kCanAutomate, kCrash);
        return kResultOk;
    }

    tresult PLUGIN_API setComponentState(IBStream* state) override {
        Stored s;
        if (state == nullptr || !readState(state, s)) {
            return kResultFalse;
        }
        setParamNormalized(kGain, s.gain);
        setParamNormalized(kCrash, s.crash);
        return kResultOk;
    }
};

} // namespace

// The factory, with the SDK's macros (public.sdk/source/main/pluginfactory.h, as the AGain example has it).
using namespace Steinberg;
using namespace Steinberg::Vst;

BEGIN_FACTORY_DEF("Wabcut", "https://github.com/wabcut", "mailto:none")
DEF_CLASS2(INLINE_UID_FROM_FUID(kProcessorUID), PClassInfo::kManyInstances, kVstAudioEffectClass, "Wabcut Test Gain VST3",
           Vst::kDistributable, "Fx", "1.0.0", kVstVersionString, Processor::make)
DEF_CLASS2(INLINE_UID_FROM_FUID(kControllerUID), PClassInfo::kManyInstances, kVstComponentControllerClass,
           "Wabcut Test Gain VST3 Controller", 0, "", "1.0.0", kVstVersionString, Controller::make)
END_FACTORY

extern "C" {
SMTG_EXPORT_SYMBOL bool PLUGIN_API InitDll() { return true; }
SMTG_EXPORT_SYMBOL bool PLUGIN_API ExitDll() { return true; }
}
