// p02 audio: Premiere Pro's Amplitude and Compression, Special and Time and Pitch audio effects (AudioEffectCatalog.h).
// The DSP is wabcut_audio's src/effects/AmplitudeEffects.cpp. Names, units, ranges and defaults follow Premiere Pro 26
// (and Audition, whose effects Premiere shares) where they could be verified; the rest are documented choices.

#include "AudioEffectCatalog.h"

#include <array>

namespace wabcut::audiocatalog {

namespace {

using C = EffectCapability;
using U = ParamUnit;

constexpr std::string_view kAmplitude = "Amplitude and Compression";
constexpr std::string_view kSpecial = "Special";
constexpr std::string_view kTimeAndPitch = "Time and Pitch";

// --- Time and Pitch ------------------------------------------------------------------------------------------

constexpr std::array<EffectParamInfo, 7> kPitchShifterParams{{
    {"semitones", "Semi-tones", U::Amount, 0.0, -12.0, 12.0},
    {"cents", "Cents", U::Amount, 0.0, -100.0, 100.0},
    {"ratio", "Ratio", U::Ratio, 1.0, 0.5, 2.0},
    {"precision", "Precision (0 Low, 1 Medium, 2 High)", U::Amount, 2.0, 0.0, 2.0, false},
    {"splicingFrequency", "Splicing Frequency", U::Hertz, 30.0, 5.0, 200.0, false},
    {"overlapping", "Overlapping (%)", U::Amount, 25.0, 0.0, 50.0, false},
    {"useDefaultSettings", "Use Appropriate Default Settings", U::Amount, 1.0, 0.0, 1.0, false},
}};

// --- Amplitude and Compression -------------------------------------------------------------------------------

constexpr std::array<EffectParamInfo, 3> kAmplifyParams{{
    {"leftGain", "Left Gain", U::Decibels, 0.0, -96.0, 24.0},
    {"rightGain", "Right Gain", U::Decibels, 0.0, -96.0, 24.0},
    {"link", "Link Sliders", U::Amount, 0.0, 0.0, 1.0, false},
}};

constexpr std::array<EffectParamInfo, 6> kChannelMixerParams{{
    {"leftFromLeft", "Left Output: Left (%)", U::Amount, 100.0, -100.0, 100.0},
    {"leftFromRight", "Left Output: Right (%)", U::Amount, 0.0, -100.0, 100.0},
    {"invertLeft", "Left Output: Invert", U::Amount, 0.0, 0.0, 1.0, false},
    {"rightFromLeft", "Right Output: Left (%)", U::Amount, 0.0, -100.0, 100.0},
    {"rightFromRight", "Right Output: Right (%)", U::Amount, 100.0, -100.0, 100.0},
    {"invertRight", "Right Output: Invert", U::Amount, 0.0, 0.0, 1.0, false},
}};

constexpr std::array<EffectParamInfo, 18> kDynamicsParams{{
    {"gateEnable", "AutoGate", U::Amount, 0.0, 0.0, 1.0, false},
    {"gateThreshold", "AutoGate Threshold", U::Decibels, -50.0, -60.0, 0.0},
    {"gateAttack", "AutoGate Attack", U::Milliseconds, 2.0, 0.1, 100.0},
    {"gateRelease", "AutoGate Release", U::Milliseconds, 150.0, 50.0, 500.0},
    {"gateHold", "AutoGate Hold", U::Milliseconds, 50.0, 0.1, 1000.0},
    {"compressorEnable", "Compressor", U::Amount, 0.0, 0.0, 1.0, false},
    {"compressorThreshold", "Compressor Threshold", U::Decibels, -20.0, -60.0, 0.0},
    {"compressorRatio", "Compressor Ratio", U::Ratio, 3.0, 1.0, 8.0},
    {"compressorAttack", "Compressor Attack", U::Milliseconds, 10.0, 0.1, 100.0},
    {"compressorRelease", "Compressor Release", U::Milliseconds, 100.0, 10.0, 500.0},
    {"compressorAuto", "Compressor Auto (Release)", U::Amount, 0.0, 0.0, 1.0, false},
    {"makeUp", "MakeUp", U::Decibels, 0.0, -6.0, 18.0},
    {"expanderEnable", "Expander", U::Amount, 0.0, 0.0, 1.0, false},
    {"expanderThreshold", "Expander Threshold", U::Decibels, -50.0, -60.0, 0.0},
    {"expanderRatio", "Expander Ratio", U::Ratio, 2.0, 1.0, 5.0},
    {"limiterEnable", "Limiter", U::Amount, 0.0, 0.0, 1.0, false},
    {"limiterThreshold", "Limiter Threshold", U::Decibels, -1.0, -12.0, 0.0},
    {"softClip", "Limiter Soft Clip", U::Amount, 0.0, 0.0, 1.0, false},
}};

constexpr std::array<EffectParamInfo, 5> kHardLimiterParams{{
    {"maximumAmplitude", "Maximum Amplitude", U::Decibels, -0.1, -40.0, 0.0},
    {"inputBoost", "Input Boost", U::Decibels, 0.0, -20.0, 40.0},
    {"lookAheadTime", "Look-Ahead Time", U::Milliseconds, 7.0, 1.0, 20.0, false},
    {"releaseTime", "Release Time", U::Milliseconds, 100.0, 1.0, 1000.0},
    {"linkChannels", "Link Channels", U::Amount, 1.0, 0.0, 1.0, false},
}};

// Four bands: Threshold, Gain, Ratio, Attack, Release, Bypass, Solo each (7 x 4 = 28), the three crossovers, Output
// Gain and the output limiter: 37 parameters (kMaxEffectParams is 64).
#define WABCUT_MBC_BAND(n)                                                                                              \
    {"band" #n "Threshold", "Band " #n " Threshold", U::Decibels, -18.0, -60.0, 0.0},                                  \
        {"band" #n "Gain", "Band " #n " Gain", U::Decibels, 0.0, -18.0, 18.0},                                         \
        {"band" #n "Ratio", "Band " #n " Ratio", U::Ratio, 4.0, 1.0, 30.0},                                            \
        {"band" #n "Attack", "Band " #n " Attack", U::Milliseconds, 10.0, 0.0, 500.0},                                 \
        {"band" #n "Release", "Band " #n " Release", U::Milliseconds, 100.0, 0.0, 5000.0},                             \
        {"band" #n "Bypass", "Band " #n " Bypass", U::Amount, 0.0, 0.0, 1.0, false},                                   \
        {"band" #n "Solo", "Band " #n " Solo", U::Amount, 0.0, 0.0, 1.0, false}
constexpr std::array<EffectParamInfo, 37> kMultibandCompressorParams{{
    {"lowCrossover", "Low Crossover", U::Hertz, 120.0, 20.0, 20000.0},
    {"midCrossover", "Mid Crossover", U::Hertz, 2000.0, 20.0, 20000.0},
    {"highCrossover", "High Crossover", U::Hertz, 10000.0, 20.0, 20000.0},
    WABCUT_MBC_BAND(1),
    WABCUT_MBC_BAND(2),
    WABCUT_MBC_BAND(3),
    WABCUT_MBC_BAND(4),
    {"outputGain", "Output Gain", U::Decibels, 0.0, -18.0, 18.0},
    {"limiterThreshold", "Limiter Threshold", U::Decibels, 0.0, -60.0, 0.0},
    {"limiterAttack", "Limiter Attack", U::Milliseconds, 5.0, 0.0, 500.0},
    {"limiterRelease", "Limiter Release", U::Milliseconds, 100.0, 0.0, 5000.0},
    {"limiterMargin", "Margin", U::Decibels, -0.1, -20.0, 0.0},
    {"brickwall", "Brickwall Limiter", U::Amount, 1.0, 0.0, 1.0, false},
}};
#undef WABCUT_MBC_BAND

constexpr std::array<EffectParamInfo, 5> kSingleBandCompressorParams{{
    {"threshold", "Threshold", U::Decibels, -20.0, -60.0, 0.0},
    {"ratio", "Ratio", U::Ratio, 4.0, 1.0, 30.0},
    {"attack", "Attack", U::Milliseconds, 10.0, 0.0, 500.0},
    {"release", "Release", U::Milliseconds, 100.0, 0.0, 5000.0},
    {"outputGain", "Output Gain", U::Decibels, 0.0, -30.0, 30.0},
}};

constexpr std::array<EffectParamInfo, 5> kTubeModeledCompressorParams{{
    {"threshold", "Threshold", U::Decibels, -20.0, -60.0, 0.0},
    {"ratio", "Ratio", U::Ratio, 4.0, 1.0, 30.0},
    {"attack", "Attack", U::Milliseconds, 10.0, 0.0, 500.0},
    {"release", "Release", U::Milliseconds, 100.0, 0.0, 5000.0},
    {"outputGain", "Output Gain", U::Decibels, 0.0, -30.0, 30.0},
}};

// --- Special -------------------------------------------------------------------------------------------------

constexpr std::array<EffectParamInfo, 8> kDistortionParams{{
    {"symmetric", "Symmetric", U::Amount, 1.0, 0.0, 1.0, false},
    {"positive", "Positive Curve (%)", U::Amount, 30.0, 0.0, 100.0},
    {"negative", "Negative Curve (%)", U::Amount, 30.0, 0.0, 100.0},
    {"curveSmoothing", "Curve Smoothing", U::Amount, 0.0, 0.0, 1.0, false},
    {"timeSmoothing", "Time Smoothing (%)", U::Amount, 0.0, 0.0, 100.0},
    {"dbRange", "dB Range", U::Decibels, 48.0, 6.0, 96.0},
    {"linearScale", "Linear Scale", U::Amount, 0.0, 0.0, 1.0, false},
    {"postFilterDcOffset", "Post-filter DC Offset", U::Amount, 1.0, 0.0, 1.0, false},
}};

constexpr std::array<EffectParamInfo, 9> kGuitarSuiteParams{{
    {"compressor", "Compressor (%)", U::Amount, 40.0, 0.0, 100.0},
    {"filterType", "Filter (0 Off, 1 Lowpass, 2 Highpass, 3 Bandpass)", U::Amount, 0.0, 0.0, 3.0, false},
    {"filterFrequency", "Filter Frequency", U::Hertz, 1000.0, 40.0, 16000.0},
    {"filterResonance", "Filter Resonance (%)", U::Amount, 0.0, 0.0, 100.0},
    {"distortionType", "Distortion Type (0 Overdrive, 1 Crunch, 2 Fuzz)", U::Amount, 0.0, 0.0, 2.0, false},
    {"distortion", "Distortion (%)", U::Amount, 30.0, 0.0, 100.0},
    {"amplifier", "Amplifier (0 Off, 1 Combo, 2 Stack, 3 Small Amp)", U::Amount, 1.0, 0.0, 3.0, false},
    {"mix", "Mix (%)", U::Amount, 100.0, 0.0, 100.0},
    {"outputGain", "Output Gain", U::Decibels, 0.0, -24.0, 24.0},
}};

constexpr std::array<EffectParamInfo, 16> kMasteringParams{{
    {"lowShelfEnable", "Low Shelf Enable", U::Amount, 0.0, 0.0, 1.0, false},
    {"lowShelfFrequency", "Low Shelf Frequency", U::Hertz, 100.0, 20.0, 1000.0},
    {"lowShelfGain", "Low Shelf Gain", U::Decibels, 0.0, -15.0, 15.0},
    {"peakEnable", "Peak Enable", U::Amount, 0.0, 0.0, 1.0, false},
    {"peakFrequency", "Peak Frequency", U::Hertz, 1000.0, 40.0, 16000.0},
    {"peakGain", "Peak Gain", U::Decibels, 0.0, -15.0, 15.0},
    {"peakQ", "Peak Q", U::Ratio, 1.0, 0.1, 10.0},
    {"highShelfEnable", "High Shelf Enable", U::Amount, 0.0, 0.0, 1.0, false},
    {"highShelfFrequency", "High Shelf Frequency", U::Hertz, 8000.0, 1000.0, 20000.0},
    {"highShelfGain", "High Shelf Gain", U::Decibels, 0.0, -15.0, 15.0},
    {"reverbAmount", "Reverb Amount (%)", U::Amount, 0.0, 0.0, 100.0},
    {"exciterMode", "Exciter Mode (0 Retro, 1 Tape, 2 Tube)", U::Amount, 0.0, 0.0, 2.0, false},
    {"exciterAmount", "Exciter Amount (%)", U::Amount, 0.0, 0.0, 100.0},
    {"widener", "Widener (%)", U::Amount, 0.0, -100.0, 100.0},
    {"loudnessMaximizer", "Loudness Maximizer (%)", U::Amount, 0.0, 0.0, 100.0},
    {"outputGain", "Output Gain", U::Decibels, 0.0, -18.0, 18.0},
}};

constexpr std::array<EffectParamInfo, 1> kVocalEnhancerParams{{
    {"mode", "Mode (0 Male, 1 Female, 2 Music)", U::Amount, 0.0, 0.0, 2.0, false},
}};

} // namespace

void appendAmplitudeEffects(std::vector<EffectDescriptor>& out) {
    constexpr EffectKind audio = EffectKind::Audio;
    const EffectCapabilities stateful = C::AudioStateful;
    const EffectCapabilities lookahead = C::AudioStateful | C::AudioLookahead;

    // Amplitude and Compression.
    out.push_back({"wabcut.audio.amplify", 1, "Amplify", kAmplitude, audio, false, kAmplifyParams, C::None,
                   "Boosts or cuts the left and right channels (dB, sample-accurate ramps). Link Sliders: every channel "
                   "takes Left Gain. Channels past the first two keep their level unless linked."});
    out.push_back({"wabcut.audio.channelMixer", 1, "Channel Mixer", kAmplitude, audio, false, kChannelMixerParams, C::None,
                   "Builds each stereo output channel from percentages of the left and right inputs, optionally "
                   "phase-inverted. Mono and the channels past the first two pass unchanged."});
    out.push_back({"wabcut.audio.dynamics", 1, "Dynamics", kAmplitude, audio, false, kDynamicsParams, stateful,
                   "Premiere's four-stage dynamics strip, each stage switched on separately: AutoGate (mutes below its "
                   "threshold after Hold, opens in Attack, closes in Release), Compressor (Auto: program-dependent "
                   "release) with MakeUp gain, Expander (downward, below its threshold) and Limiter (no lookahead; Soft "
                   "Clip rounds peaks into the threshold instead of riding the gain)."});
    out.push_back({"wabcut.audio.hardLimiter", 1, "Hard Limiter", kAmplitude, audio, false, kHardLimiterParams, lookahead,
                   "Lookahead brick-wall limiter: no output sample exceeds Maximum Amplitude. Input Boost drives it "
                   "harder; the gain falls over the look-ahead window and recovers 12 dB per Release Time. Link Channels "
                   "keeps the stereo balance (one gain for all channels). Latency: the look-ahead time."});
    out.push_back({"wabcut.audio.multibandCompressor", 1, "Multiband Compressor", kAmplitude, audio, false,
                   kMultibandCompressorParams, lookahead,
                   "Four bands split by Linkwitz-Riley 24 dB/octave crossovers (the bands sum back flat), each with its own "
                   "compressor (Threshold, Ratio, Attack, Release), make-up Gain, Bypass and Solo, then Output Gain and a "
                   "limiter that holds the level to its Threshold and raises it to Margin. Brickwall Limiter: a 5 ms "
                   "lookahead limiter that never exceeds Margin (5 ms latency); off, a fast peak limiter that may overshoot."});
    out.push_back({"wabcut.audio.singleBandCompressor", 1, "Single-band Compressor", kAmplitude, audio, false,
                   kSingleBandCompressorParams, stateful,
                   "Feed-forward peak compressor (hard knee, channels linked): above Threshold the level rises 1/Ratio as "
                   "fast; Attack and Release are the gain's time constants; Output Gain after."});
    out.push_back({"wabcut.audio.tubeModeledCompressor", 1, "Tube-modeled Compressor", kAmplitude, audio, false,
                   kTubeModeledCompressorParams, stateful,
                   "Soft-knee compressor with the colour of a variable-mu tube stage: second and some third harmonic that "
                   "grow with the gain reduction (none while it is not compressing)."});
    // Special.
    out.push_back({"wabcut.audio.distortion", 1, "Distortion", kSpecial, audio, false, kDistortionParams, stateful,
                   "Waveshaping distortion with separate positive and negative transfer curves (Premiere draws them as "
                   "graphs; here each is the graph with its middle point dragged up and left, from the straight line at "
                   "0 % to full drive at 100 %). The graphs span the top dB Range of the level (full scale with Linear "
                   "Scale); Curve Smoothing joins the points with a curve instead of straight segments; Time Smoothing "
                   "reads the level from its low-frequency content for a softer result; Post-filter DC Offset removes "
                   "the offset asymmetric curves add."});
    out.push_back({"wabcut.audio.guitarSuite", 1, "Guitar Suite", kSpecial, audio, false, kGuitarSuiteParams, stateful,
                   "Guitar chain: Compressor, Filter (lowpass, highpass or bandpass with Resonance), Distortion (overdrive, "
                   "crunch or fuzz), an Amplifier cabinet model, Mix (processed share) and Output Gain."});
    out.push_back({"wabcut.audio.mastering", 1, "Mastering", kSpecial, audio, false, kMasteringParams, lookahead,
                   "Mastering chain: a low shelf, a peak and a high shelf, a touch of Reverb, an Exciter (Retro, Tape or "
                   "Tube harmonics above a few kHz), a mid/side Widener (-100 % mono .. +100 % twice as wide), a Loudness "
                   "Maximizer (up to +12 dB into a lookahead limiter, 3 ms latency) and Output Gain."});
    out.push_back({"wabcut.audio.vocalEnhancer", 1, "Vocal Enhancer", kSpecial, audio, false, kVocalEnhancerParams, stateful,
                   "One-knob voice clean-up: Male and Female cut rumble and mud, lift presence and air, and compress "
                   "gently; Music carves the voice band out of a music bed so it does not compete with dialogue."});
    // Time and Pitch.
    out.push_back({"wabcut.audio.pitchShifter", 1, "Pitch Shifter", kTimeAndPitch, audio, false, kPitchShifterParams,
                   lookahead,
                   "Changes pitch without changing duration: overlapping splices of the audio, each resampled by the "
                   "pitch ratio and spliced where the waveforms line up (Precision: how low a pitch it aligns, about 170, "
                   "85 or 50 Hz, and how well it interpolates). Semi-tones and Cents set the shift; Ratio sets it when "
                   "both are 0. Splicing Frequency and Overlapping size the splices unless Use Appropriate Default "
                   "Settings picks them. Latency about 20 ms (Low) to 45 ms (High)."});
}

} // namespace wabcut::audiocatalog
