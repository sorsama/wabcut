#include "AudioEffectCatalog.h"

// p02 audio: Premiere Pro's Delay and Echo, Modulation, Reverb and Noise Reduction/Restoration effects (the DSP is
// wabcut_audio's src/effects/TimeEffects.cpp). Names, parameter names and order follow Premiere Pro 26 and the
// Audition effects it shares; percentages are Amount 0..100; choices and switches are constant Amount parameters
// with integer ranges whose names list the choices.

#include <array>

namespace wabcut::audiocatalog {

namespace {

using C = EffectCapability;
using U = ParamUnit;

// ---- Delay and Echo ---------------------------------------------------------------------------------------------

constexpr std::array<EffectParamInfo, 3> kDelayParams{{
    {"delay", "Delay", U::Milliseconds, 1000.0, 0.0, 2000.0},
    {"feedback", "Feedback", U::Amount, 0.0, 0.0, 100.0},
    {"mix", "Mix", U::Amount, 50.0, 0.0, 100.0},
}};

constexpr std::array<EffectParamInfo, 7> kAnalogDelayParams{{
    {"mode", "Mode (0 Tape, 1 Tape and Tube, 2 Analog)", U::Amount, 0.0, 0.0, 2.0, false},
    {"dryOutput", "Dry Output", U::Amount, 100.0, 0.0, 100.0},
    {"wetOutput", "Wet Output", U::Amount, 50.0, 0.0, 100.0},
    {"delay", "Delay", U::Milliseconds, 300.0, 0.0, 8000.0},
    {"feedback", "Feedback", U::Amount, 40.0, 0.0, 200.0},
    {"trash", "Trash", U::Amount, 0.0, 0.0, 100.0},
    {"spread", "Spread", U::Amount, 100.0, 0.0, 200.0},
}};

constexpr std::array<EffectParamInfo, 13> kMultitapDelayParams{{
    {"delay1", "Delay 1", U::Milliseconds, 250.0, 0.0, 2000.0},
    {"feedback1", "Feedback 1", U::Amount, 0.0, 0.0, 100.0},
    {"level1", "Level 1", U::Decibels, 0.0, -96.0, 0.0},
    {"delay2", "Delay 2", U::Milliseconds, 500.0, 0.0, 2000.0},
    {"feedback2", "Feedback 2", U::Amount, 0.0, 0.0, 100.0},
    {"level2", "Level 2", U::Decibels, -3.0, -96.0, 0.0},
    {"delay3", "Delay 3", U::Milliseconds, 750.0, 0.0, 2000.0},
    {"feedback3", "Feedback 3", U::Amount, 0.0, 0.0, 100.0},
    {"level3", "Level 3", U::Decibels, -6.0, -96.0, 0.0},
    {"delay4", "Delay 4", U::Milliseconds, 1000.0, 0.0, 2000.0},
    {"feedback4", "Feedback 4", U::Amount, 0.0, 0.0, 100.0},
    {"level4", "Level 4", U::Decibels, -9.0, -96.0, 0.0},
    {"mix", "Mix", U::Amount, 50.0, 0.0, 100.0},
}};

// ---- Modulation -------------------------------------------------------------------------------------------------

constexpr std::array<EffectParamInfo, 5> kChorusFlangerParams{{
    {"mode", "Mode (0 Chorus, 1 Flanger)", U::Amount, 0.0, 0.0, 1.0, false},
    {"speed", "Speed", U::Hertz, 1.0, 0.1, 10.0},
    {"width", "Width", U::Amount, 50.0, 0.0, 100.0},
    {"intensity", "Intensity", U::Amount, 50.0, 0.0, 100.0},
    {"transience", "Transience", U::Amount, 0.0, 0.0, 100.0},
}};

constexpr std::array<EffectParamInfo, 10> kFlangerParams{{
    {"initialDelay", "Initial Delay Time", U::Milliseconds, 1.0, 0.0, 20.0},
    {"finalDelay", "Final Delay Time", U::Milliseconds, 5.0, 0.0, 20.0},
    {"stereoPhasing", "Stereo Phasing", U::Degrees, 90.0, 0.0, 360.0},
    {"feedback", "Feedback", U::Amount, 30.0, 0.0, 100.0},
    {"modulationRate", "Modulation Rate", U::Hertz, 0.3, 0.01, 10.0},
    {"inverted", "Inverted", U::Amount, 0.0, 0.0, 1.0, false},
    {"specialEffects", "Special Effects", U::Amount, 0.0, 0.0, 1.0, false},
    {"sinusoidal", "Sinusoidal", U::Amount, 1.0, 0.0, 1.0, false},
    {"mix", "Mix", U::Amount, 50.0, 0.0, 100.0},
    {"outputGain", "Output Gain", U::Decibels, 0.0, -20.0, 20.0},
}};

constexpr std::array<EffectParamInfo, 9> kPhaserParams{{
    {"stages", "Stages", U::Amount, 4.0, 1.0, 12.0, false},
    {"intensity", "Intensity", U::Amount, 100.0, 0.0, 100.0},
    {"depth", "Depth", U::Amount, 60.0, 0.0, 100.0},
    {"modulationRate", "Modulation Rate", U::Hertz, 0.4, 0.01, 10.0},
    {"phaseDifference", "Phase Difference", U::Degrees, 0.0, -180.0, 180.0},
    {"upperFrequency", "Upper Frequency", U::Hertz, 2000.0, 100.0, 10000.0},
    {"feedback", "Feedback", U::Amount, 0.0, -100.0, 100.0},
    {"mix", "Mix", U::Amount, 50.0, 0.0, 100.0},
    {"outputGain", "Output Gain", U::Decibels, 0.0, -20.0, 20.0},
}};

// ---- Reverb -----------------------------------------------------------------------------------------------------

constexpr std::array<EffectParamInfo, 8> kConvolutionReverbParams{{
    {"impulse", "Impulse (0 Small Room, 1 Medium Room, 2 Large Hall, 3 Plate, 4 Cathedral)", U::Amount, 1.0, 0.0, 4.0,
     false},
    {"mix", "Mix", U::Amount, 50.0, 0.0, 100.0},
    {"roomSize", "Room Size", U::Amount, 100.0, 0.0, 100.0, false},
    {"dampingLF", "Damping LF", U::Amount, 0.0, 0.0, 100.0, false},
    {"dampingHF", "Damping HF", U::Amount, 0.0, 0.0, 100.0, false},
    {"preDelay", "Pre-Delay", U::Milliseconds, 0.0, 0.0, 200.0, false},
    {"width", "Width", U::Amount, 100.0, 0.0, 200.0},
    {"gain", "Gain", U::Decibels, 0.0, -20.0, 20.0},
}};

constexpr std::array<EffectParamInfo, 10> kStudioReverbParams{{
    {"roomSize", "Room Size", U::Amount, 50.0, 0.0, 100.0, false},
    {"decay", "Decay", U::Milliseconds, 1500.0, 100.0, 10000.0},
    {"earlyReflections", "Early Reflections", U::Amount, 50.0, 0.0, 100.0},
    {"width", "Width", U::Amount, 100.0, 0.0, 100.0},
    {"highFrequencyCut", "High Frequency Cut", U::Hertz, 8000.0, 200.0, 20000.0},
    {"lowFrequencyCut", "Low Frequency Cut", U::Hertz, 100.0, 20.0, 2000.0},
    {"damping", "Damping", U::Amount, 50.0, 0.0, 100.0},
    {"diffusion", "Diffusion", U::Amount, 50.0, 0.0, 100.0},
    {"dry", "Dry", U::Amount, 100.0, 0.0, 100.0},
    {"wet", "Wet", U::Amount, 30.0, 0.0, 100.0},
}};

constexpr std::array<EffectParamInfo, 12> kSurroundReverbParams{{
    {"centerInput", "Center Input", U::Amount, 100.0, 0.0, 100.0},
    {"lfeInput", "LFE Input", U::Amount, 0.0, 0.0, 100.0},
    {"roomSize", "Room Size", U::Amount, 50.0, 0.0, 100.0, false},
    {"decay", "Decay", U::Milliseconds, 1500.0, 100.0, 10000.0},
    {"earlyReflections", "Early Reflections", U::Amount, 50.0, 0.0, 100.0},
    {"width", "Width", U::Amount, 100.0, 0.0, 100.0},
    {"highFrequencyCut", "High Frequency Cut", U::Hertz, 8000.0, 200.0, 20000.0},
    {"lowFrequencyCut", "Low Frequency Cut", U::Hertz, 100.0, 20.0, 2000.0},
    {"damping", "Damping", U::Amount, 50.0, 0.0, 100.0},
    {"diffusion", "Diffusion", U::Amount, 50.0, 0.0, 100.0},
    {"dry", "Dry", U::Amount, 100.0, 0.0, 100.0},
    {"wet", "Wet", U::Amount, 30.0, 0.0, 100.0},
}};

// ---- Noise Reduction/Restoration --------------------------------------------------------------------------------

constexpr std::array<EffectParamInfo, 4> kDeReverbParams{{
    {"processingFocus", "Processing Focus (0 All, 1 Low, 2 Mid, 3 High, 4 Low and High)", U::Amount, 0.0, 0.0, 4.0,
     false},
    {"amount", "Amount", U::Amount, 40.0, 0.0, 100.0},
    {"gain", "Gain", U::Decibels, 0.0, -20.0, 20.0},
    {"autoGain", "Auto Gain", U::Amount, 0.0, 0.0, 1.0, false},
}};

constexpr std::array<EffectParamInfo, 4> kDeNoiseParams{{
    {"processingFocus", "Processing Focus (0 All, 1 Low, 2 Mid, 3 High, 4 Low and High)", U::Amount, 0.0, 0.0, 4.0,
     false},
    {"amount", "Amount", U::Amount, 40.0, 0.0, 100.0},
    {"gain", "Gain", U::Decibels, 0.0, -20.0, 20.0},
    {"outputNoiseOnly", "Output Noise Only", U::Amount, 0.0, 0.0, 1.0, false},
}};

constexpr std::array<EffectParamInfo, 7> kAdaptiveNoiseReductionParams{{
    {"reduceNoiseBy", "Reduce Noise By", U::Decibels, 10.0, 0.0, 60.0},
    {"noisiness", "Noisiness", U::Amount, 20.0, 0.0, 100.0},
    {"noiseFloor", "Noise Floor", U::Decibels, 0.0, -20.0, 20.0},
    {"signalThreshold", "Signal Threshold", U::Decibels, 0.0, -20.0, 20.0},
    {"spectralDecayRate", "Spectral Decay Rate", U::Milliseconds, 200.0, 0.0, 2000.0},
    {"broadbandPreservation", "Broadband Preservation", U::Hertz, 100.0, 0.0, 500.0},
    {"fftSize", "FFT Size", U::Amount, 512.0, 128.0, 16384.0, false},
}};

constexpr std::array<EffectParamInfo, 2> kClickRemoverParams{{
    {"threshold", "Threshold", U::Amount, 30.0, 1.0, 100.0},
    {"complexity", "Complexity", U::Amount, 16.0, 1.0, 100.0},
}};

} // namespace

void appendTimeEffects(std::vector<EffectDescriptor>& out) {
    const auto audio = EffectKind::Audio;
    const EffectCapabilities stateful = C::AudioStateful;
    const EffectCapabilities lookahead = C::AudioStateful | C::AudioLookahead;

    // Delay and Echo.
    out.push_back({"wabcut.audio.delay", 1, "Delay", "Delay and Echo", audio, false, kDelayParams, stateful,
                   "An echo of the sound after Delay (at most 2 s); Feedback sends that share of the echo back in for "
                   "repeats that fade away; Mix is the echo's share of the output (0 = dry)."});
    out.push_back({"wabcut.audio.analogDelay", 1, "Analog Delay", "Delay and Echo", audio, false, kAnalogDelayParams,
                   stateful,
                   "A vintage delay unit: Tape, Tape and Tube or Analog (bucket-brigade) character in the feedback loop "
                   "(its tone and saturation). Dry and Wet Output are the levels of the original and the echoes; Feedback "
                   "above 100 % makes the repeats grow until the loop saturates; Trash adds drive and low-end warmth; "
                   "Spread is the stereo width of the echoes (0 = mono, 100 = as recorded, 200 = wider)."});
    out.push_back({"wabcut.audio.multitapDelay", 1, "Multitap Delay", "Delay and Echo", audio, false,
                   kMultitapDelayParams, stateful,
                   "Up to four echoes of the sound, each with its own Delay (at most 2 s), Feedback (its own decaying "
                   "repeats) and Level; Mix is the echoes' share of the output (0 = dry)."});
    // Modulation.
    out.push_back({"wabcut.audio.chorusFlanger", 1, "Chorus/Flanger", "Modulation", audio, false, kChorusFlangerParams,
                   stateful,
                   "Chorus: three slightly delayed voices whose delays drift (a section of several players); Flanger: "
                   "one short swept delay with feedback (a jet-like comb sweep). Speed is the sweep rate, Width the "
                   "largest delay change, Intensity the processed share (0 = dry), Transience sharpens the attacks."});
    out.push_back({"wabcut.audio.flanger", 1, "Flanger", "Modulation", audio, false, kFlangerParams, stateful,
                   "A delay swept between Initial and Final Delay Time at Modulation Rate (triangle, or sine when "
                   "Sinusoidal), mixed with the original: moving comb-filter notches. Stereo Phasing offsets the right "
                   "channel's sweep; Inverted flips the delayed signal; Special Effects mixes normal and inverted "
                   "flanging; Mix is the delayed share (0 = dry)."});
    out.push_back({"wabcut.audio.phaser", 1, "Phaser", "Modulation", audio, false, kPhaserParams, stateful,
                   "Stages of first-order all-pass filters whose corner sweeps from Upper Frequency down by Depth (100 % "
                   "= to 0 Hz) at Modulation Rate; mixed with the original it makes moving notches. Intensity and Mix set "
                   "the phased share, Feedback (negative: inverted) deepens the notches, Phase Difference offsets the "
                   "right channel's sweep."});
    // Reverb.
    out.push_back({"wabcut.audio.convolutionReverb", 1, "Convolution Reverb", "Reverb", audio, false,
                   kConvolutionReverbParams, lookahead,
                   "Convolves the sound with a built-in impulse response (synthesised: small and medium rooms, a large "
                   "hall, a plate, a cathedral; at most 4 s). Room Size scales the space (100 % = the full impulse), "
                   "Damping LF and HF shorten and soften the lows and highs of the tail, Pre-Delay delays it, Width is its "
                   "stereo spread, Mix the reverb's share and Gain the output level. Partitioned FFT convolution: a few "
                   "milliseconds of latency, compensated."});
    out.push_back({"wabcut.audio.studioReverb", 1, "Studio Reverb", "Reverb", audio, false, kStudioReverbParams,
                   stateful,
                   "An algorithmic room (a feedback delay network): Room Size, Decay (the tail's RT60), Early "
                   "Reflections (their level), Width (the tail's stereo spread), High and Low Frequency Cut (the "
                   "reverb's band), Damping (the highs die away faster), Diffusion (smooth, dense echoes rather than "
                   "distinct ones) and the Dry and Wet output levels."});
    out.push_back({"wabcut.audio.surroundReverb", 1, "Surround Reverb", "Reverb", audio, false, kSurroundReverbParams,
                   stateful,
                   "Studio Reverb for 5.1 (and 7.1): one room feeds decorrelated tails to the front and the surround "
                   "pairs, the centre gets the front tail's middle and the LFE stays dry. Center Input and LFE Input set "
                   "how much of those channels reach the room. With mono or stereo it is Studio Reverb."});
    // Noise Reduction/Restoration.
    out.push_back({"wabcut.audio.deReverb", 1, "DeReverb", "Noise Reduction/Restoration", audio, false,
                   kDeReverbParams, lookahead,
                   "Reduces a room's reverberation: the late reverb in each frequency band is estimated from the "
                   "decaying recent spectrum and subtracted. Amount sets how much is removed, Processing Focus where in "
                   "the spectrum, Auto Gain makes up the level lost, Gain is the output level."});
    out.push_back({"wabcut.audio.spectralDenoise", 1, "DeNoise", "Noise Reduction/Restoration", audio, false,
                   kDeNoiseParams, lookahead,
                   "Reduces steady background noise (hiss, hum, air conditioning): the noise floor of each frequency band "
                   "is tracked continuously and attenuated where the sound does not rise above it. Amount sets how much, "
                   "Processing Focus where in the spectrum, Output Noise Only plays what is removed, Gain is the output "
                   "level."});
    out.push_back({"wabcut.audio.adaptiveNoiseReduction", 1, "Adaptive Noise Reduction", "Noise Reduction/Restoration",
                   audio, false, kAdaptiveNoiseReductionParams, lookahead,
                   "Learns broadband noise continuously and attenuates it: Reduce Noise By is the most the noise goes "
                   "down, Noisiness how much of the sound is taken for noise, Noise Floor and Signal Threshold fine-tune "
                   "the estimated noise and the level that counts as wanted sound, Spectral Decay Rate how fast the "
                   "reduction closes in after sound (ms per 60 dB), Broadband Preservation keeps that many Hz around "
                   "wanted sound, FFT Size trades frequency for time resolution."});
    out.push_back({"wabcut.audio.automaticClickRemover", 1, "Automatic Click Remover", "Noise Reduction/Restoration",
                   audio, false, kClickRemoverParams, lookahead,
                   "Finds clicks and pops (short impulses that stand out of the sound's local high-frequency level) and "
                   "replaces them with an interpolation of the sound on either side. Lower Threshold finds more clicks; "
                   "higher Complexity repairs longer ones with a more detailed model."});
}

} // namespace wabcut::audiocatalog
