// p02 audio: the descriptors of Premiere Pro's Filter and EQ effects, the Stereo Expander and the simple channel effects
// (DSP: src/audio/src/effects/FilterEffects.cpp). Names, folders, ranges and defaults follow Premiere Pro 26's Effect
// Controls; where Premiere scales a range with the sequence's sample rate, the 48 kHz values are used (the DSP keeps
// every frequency below 0.49 of the actual rate).

#include "AudioEffectCatalog.h"

#include <array>

namespace wabcut::audiocatalog {

namespace {

using C = EffectCapability;
using U = ParamUnit;
constexpr EffectKind kAudio = EffectKind::Audio;

constexpr std::string_view kFilterEq = "Filter and EQ";
constexpr std::string_view kStereoImagery = "Stereo Imagery";
constexpr std::string_view kSpecial = "Special";
constexpr std::string_view kAmplitude = "Amplitude and Compression";
constexpr std::string_view kObsolete = "Obsolete Audio Effects";

constexpr double kMaxHz = 24000.0;

// ---- Graphic Equalizer (10 / 20 / 30 Bands) ------------------------------------------------------------------------
// Bands at 31.25 Hz x 2^(k / bands per octave); the sliders are clamped to +-Range / 2.

#define WABCUT_GEQ_BAND(n) {"band" #n, "EQ Band " #n, U::Decibels, 0.0, -70.0, 70.0}
#define WABCUT_GEQ_COMMON                                                                                              \
    {"accuracy", "Accuracy", U::Amount, 1000.0, 10.0, 8192.0, false}, {"gain", "Gain", U::Decibels, 0.0, -64.0, 64.0}, \
        {"range", "Range", U::Decibels, 48.0, 1.5, 120.0, false}

constexpr std::array<EffectParamInfo, 13> kGraphicEq10Params{{
    WABCUT_GEQ_COMMON,
    WABCUT_GEQ_BAND(1), WABCUT_GEQ_BAND(2), WABCUT_GEQ_BAND(3), WABCUT_GEQ_BAND(4), WABCUT_GEQ_BAND(5),
    WABCUT_GEQ_BAND(6), WABCUT_GEQ_BAND(7), WABCUT_GEQ_BAND(8), WABCUT_GEQ_BAND(9), WABCUT_GEQ_BAND(10),
}};

constexpr std::array<EffectParamInfo, 23> kGraphicEq20Params{{
    WABCUT_GEQ_COMMON,
    WABCUT_GEQ_BAND(1),  WABCUT_GEQ_BAND(2),  WABCUT_GEQ_BAND(3),  WABCUT_GEQ_BAND(4),  WABCUT_GEQ_BAND(5),
    WABCUT_GEQ_BAND(6),  WABCUT_GEQ_BAND(7),  WABCUT_GEQ_BAND(8),  WABCUT_GEQ_BAND(9),  WABCUT_GEQ_BAND(10),
    WABCUT_GEQ_BAND(11), WABCUT_GEQ_BAND(12), WABCUT_GEQ_BAND(13), WABCUT_GEQ_BAND(14), WABCUT_GEQ_BAND(15),
    WABCUT_GEQ_BAND(16), WABCUT_GEQ_BAND(17), WABCUT_GEQ_BAND(18), WABCUT_GEQ_BAND(19), WABCUT_GEQ_BAND(20),
}};

constexpr std::array<EffectParamInfo, 33> kGraphicEq30Params{{
    WABCUT_GEQ_COMMON,
    WABCUT_GEQ_BAND(1),  WABCUT_GEQ_BAND(2),  WABCUT_GEQ_BAND(3),  WABCUT_GEQ_BAND(4),  WABCUT_GEQ_BAND(5),
    WABCUT_GEQ_BAND(6),  WABCUT_GEQ_BAND(7),  WABCUT_GEQ_BAND(8),  WABCUT_GEQ_BAND(9),  WABCUT_GEQ_BAND(10),
    WABCUT_GEQ_BAND(11), WABCUT_GEQ_BAND(12), WABCUT_GEQ_BAND(13), WABCUT_GEQ_BAND(14), WABCUT_GEQ_BAND(15),
    WABCUT_GEQ_BAND(16), WABCUT_GEQ_BAND(17), WABCUT_GEQ_BAND(18), WABCUT_GEQ_BAND(19), WABCUT_GEQ_BAND(20),
    WABCUT_GEQ_BAND(21), WABCUT_GEQ_BAND(22), WABCUT_GEQ_BAND(23), WABCUT_GEQ_BAND(24), WABCUT_GEQ_BAND(25),
    WABCUT_GEQ_BAND(26), WABCUT_GEQ_BAND(27), WABCUT_GEQ_BAND(28), WABCUT_GEQ_BAND(29), WABCUT_GEQ_BAND(30),
}};

#undef WABCUT_GEQ_BAND
#undef WABCUT_GEQ_COMMON

// ---- Parametric Equalizer ------------------------------------------------------------------------------------------

#define WABCUT_PEQ_BAND(n, hz, width)                                                                                  \
    {"band" #n "Enabled", "EQ Band " #n " Enable", U::Amount, 1.0, 0.0, 1.0, false},                                  \
        {"band" #n "Frequency", "EQ Band " #n " Center Frequency", U::Hertz, hz, 20.0, kMaxHz},                        \
        {"band" #n "Gain", "EQ Band " #n " Gain", U::Decibels, 0.0, -48.0, 48.0},                                      \
        {"band" #n "Q", "EQ Band " #n " Q", U::Ratio, 2.0, 0.1, 100.0},                                                \
        {"band" #n "Bandwidth", "EQ Band " #n " Bandwidth", U::Hertz, width, 1.0, 10000.0}

constexpr std::array<EffectParamInfo, 42> kParametricEqParams{{
    {"gain", "Gain", U::Decibels, 0.0, -96.0, 48.0},
    {"highPassEnabled", "High-Pass Filter Enable", U::Amount, 0.0, 0.0, 1.0, false},
    {"highPassFrequency", "High-Pass Cutoff Frequency", U::Hertz, 20.0, 20.0, kMaxHz},
    {"highPassSlope", "High-Pass Slope (order: 1 = 6 dB/Oct ... 8 = 48 dB/Oct)", U::Amount, 4.0, 1.0, 8.0, false},
    {"lowShelfEnabled", "Low Shelf Enable", U::Amount, 1.0, 0.0, 1.0, false},
    {"lowShelfFrequency", "Low Shelf Frequency", U::Hertz, 40.0, 20.0, kMaxHz},
    {"lowShelfGain", "Low Shelf Gain", U::Decibels, 0.0, -48.0, 48.0},
    {"lowShelfOrder", "Low Shelf Filter Order (1 First Order, 2 Second Order)", U::Amount, 1.0, 1.0, 2.0, false},
    WABCUT_PEQ_BAND(1, 50.0, 25.0),
    WABCUT_PEQ_BAND(2, 200.0, 100.0),
    WABCUT_PEQ_BAND(3, 800.0, 400.0),
    WABCUT_PEQ_BAND(4, 3200.0, 1600.0),
    WABCUT_PEQ_BAND(5, 12800.0, 6400.0),
    {"highShelfEnabled", "High Shelf Enable", U::Amount, 1.0, 0.0, 1.0, false},
    {"highShelfFrequency", "High Shelf Frequency", U::Hertz, 18000.0, 20.0, kMaxHz},
    {"highShelfGain", "High Shelf Gain", U::Decibels, 0.0, -48.0, 48.0},
    {"highShelfOrder", "High Shelf Filter Order (1 First Order, 2 Second Order)", U::Amount, 1.0, 1.0, 2.0, false},
    {"lowPassEnabled", "Low-Pass Filter Enable", U::Amount, 0.0, 0.0, 1.0, false},
    {"lowPassFrequency", "Low-Pass Cutoff Frequency", U::Hertz, 20000.0, 20.0, kMaxHz},
    {"lowPassSlope", "Low-Pass Slope (order: 1 = 6 dB/Oct ... 8 = 48 dB/Oct)", U::Amount, 4.0, 1.0, 8.0, false},
    {"constantQ", "Constant Q (1 Constant Q, 0 Constant Width)", U::Amount, 1.0, 0.0, 1.0, false},
    {"ultraQuiet", "Ultra-Quiet Mode", U::Amount, 0.0, 0.0, 1.0, false},
}};

#undef WABCUT_PEQ_BAND

// ---- Simple filters, Bass, Treble ----------------------------------------------------------------------------------

constexpr std::array<EffectParamInfo, 3> kSimpleParametricEqParams{{
    {"center", "Center", U::Hertz, 1495.0, 10.0, kMaxHz},
    {"q", "Q", U::Ratio, 10.05, 0.1, 20.0},
    {"boost", "Boost", U::Decibels, 0.0, -24.0, 24.0},
}};
constexpr std::array<EffectParamInfo, 1> kCutoffParams{{{"cutoff", "Cutoff", U::Hertz, 1495.0, 10.0, kMaxHz}}};
constexpr std::array<EffectParamInfo, 2> kCutoffQParams{{
    {"cutoff", "Cutoff", U::Hertz, 1495.0, 10.0, kMaxHz},
    {"q", "Q", U::Ratio, 2.575, 0.1, 10.0},
}};
constexpr std::array<EffectParamInfo, 1> kBoostParams{{{"boost", "Boost", U::Decibels, 0.0, -24.0, 24.0}}};

// ---- Notch Filter --------------------------------------------------------------------------------------------------

#define WABCUT_NOTCH(n, hz)                                                                                            \
    {"notch" #n "Enabled", "Enable " #n, U::Amount, 0.0, 0.0, 1.0, false},                                             \
        {"notch" #n "Frequency", "Frequency " #n, U::Hertz, hz, 0.5, kMaxHz},                                          \
        {"notch" #n "Gain", "Gain " #n, U::Decibels, -30.0, -96.0, 96.0}

constexpr std::array<EffectParamInfo, 22> kNotchFilterParams{{
    WABCUT_NOTCH(1, 60.0),
    WABCUT_NOTCH(2, 120.0),
    WABCUT_NOTCH(3, 180.0),
    WABCUT_NOTCH(4, 240.0),
    WABCUT_NOTCH(5, 300.0),
    WABCUT_NOTCH(6, 360.0),
    {"notchWidth", "Notch Width (0 Narrow, 1 Very Narrow, 2 Super Narrow)", U::Amount, 0.0, 0.0, 2.0, false},
    {"ultraQuiet", "Ultra Quiet", U::Amount, 0.0, 0.0, 1.0, false},
    {"fixGains", "Amplify All", U::Amount, 0.0, 0.0, 1.0, false},
    {"fixedGain", "Fixed Gain", U::Decibels, -30.0, -96.0, 96.0},
}};

#undef WABCUT_NOTCH

// ---- Scientific Filter ---------------------------------------------------------------------------------------------

constexpr std::array<EffectParamInfo, 11> kScientificFilterParams{{
    {"type", "Filter Type (0 Bessel, 1 Butterworth, 2 Chebychev, 3 Elliptical)", U::Amount, 1.0, 0.0, 3.0, false},
    {"mode", "Filter Mode (0 Low Pass, 1 High Pass, 2 Band Pass, 3 Band Stop)", U::Amount, 0.0, 0.0, 3.0, false},
    {"gain", "Gain", U::Decibels, 0.0, -50.0, 50.0},
    {"cutoff", "Cutoff", U::Hertz, 1440.0, 1.0, kMaxHz},
    {"highCutoff", "High Cutoff", U::Hertz, 1920.0, 1.0, kMaxHz},
    {"orderMode", "Order Mode Enabled (1 Order Value, 0 from Transition Bandwidth and Stop Attn)", U::Amount, 1.0, 0.0, 1.0, false},
    {"order", "Order Value", U::Amount, 10.0, 1.0, 32.0, false},
    {"transitionBandwidth", "Transition Bandwidth", U::Hertz, 2400.0, 1.0, kMaxHz, false},
    {"highWidth", "High Width", U::Hertz, 2880.0, 1.0, kMaxHz, false},
    {"passRipple", "Pass Ripple", U::Decibels, 3.0, 0.0, 120.0, false},
    {"stopAttenuation", "Stop Attn", U::Decibels, 10.0, 0.0, 192.0, false},
}};

// ---- FFT Filter ----------------------------------------------------------------------------------------------------

#define WABCUT_FFT_PAIR(n, hz)                                                                                         \
    {"x" #n, "X " #n, U::Hertz, hz, 0.0, kMaxHz}, {"y" #n, "Y " #n, U::Decibels, 0.0, -120.0, 30.0}

constexpr std::array<EffectParamInfo, 29> kFftFilterParams{{
    {"scale", "Scale (0 Linear, 1 Logarithmic)", U::Amount, 1.0, 0.0, 1.0, false},
    {"fftSize", "FFT Size", U::Amount, 4096.0, 32.0, 32768.0, false},
    {"windowType", "Window Type (0 Triangular, 1 Hamming, 2 Hann, 3 Blackman, 4 Blackman-Harris, 5 Gaussian, 6 Kaiser (140 dB), 7 Kaiser (180 dB))",
     U::Amount, 3.0, 0.0, 7.0, false},
    {"splines", "Splines", U::Amount, 0.0, 0.0, 1.0, false},
    {"pairs", "Number of X/Y pairs", U::Amount, 2.0, 2.0, 12.0, false},
    WABCUT_FFT_PAIR(1, 20.0),
    WABCUT_FFT_PAIR(2, 20000.0),
    WABCUT_FFT_PAIR(3, 100.0),
    WABCUT_FFT_PAIR(4, 250.0),
    WABCUT_FFT_PAIR(5, 500.0),
    WABCUT_FFT_PAIR(6, 1000.0),
    WABCUT_FFT_PAIR(7, 2000.0),
    WABCUT_FFT_PAIR(8, 4000.0),
    WABCUT_FFT_PAIR(9, 6000.0),
    WABCUT_FFT_PAIR(10, 8000.0),
    WABCUT_FFT_PAIR(11, 12000.0),
    WABCUT_FFT_PAIR(12, 16000.0),
}};

#undef WABCUT_FFT_PAIR

// ---- Stereo Expander and the channel effects -----------------------------------------------------------------------

constexpr std::array<EffectParamInfo, 2> kStereoExpanderParams{{
    {"centerPan", "Center Channel Pan", U::Amount, 0.0, -100.0, 100.0},
    {"stereoExpand", "Stereo Expander", U::Amount, 100.0, 0.0, 300.0},
}};
constexpr std::array<EffectParamInfo, 1> kBalanceParams{{{"balance", "Balance", U::Amount, 0.0, -100.0, 100.0}}};
constexpr std::array<EffectParamInfo, 6> kChannelVolumeParams{{
    {"left", "Left", U::Decibels, 0.0, -144.0, 15.0},
    {"right", "Right", U::Decibels, 0.0, -144.0, 15.0},
    {"center", "Center", U::Decibels, 0.0, -144.0, 15.0},
    {"lfe", "LFE", U::Decibels, 0.0, -144.0, 15.0},
    {"leftSurround", "Left Surround", U::Decibels, 0.0, -144.0, 15.0},
    {"rightSurround", "Right Surround", U::Decibels, 0.0, -144.0, 15.0},
}};

} // namespace

void appendFilterEffects(std::vector<EffectDescriptor>& out) {
    const EffectCapabilities stateful = C::AudioStateful;
    out.insert(out.end(), {
        {"wabcut.audio.graphicEq10", 1, "Graphic Equalizer (10 Bands)", kFilterEq, kAudio, false, kGraphicEq10Params, stateful,
         "Ten octave bands, 31.25 Hz to 16 kHz. Each EQ Band slider is the gain at its band's centre: the overlapping "
         "bands are solved so that their sum meets every slider. Range limits the sliders to +-Range/2; higher Accuracy "
         "refines the solution (more processing when a slider moves); Gain sets the output level."},
        {"wabcut.audio.graphicEq20", 1, "Graphic Equalizer (20 Bands)", kFilterEq, kAudio, false, kGraphicEq20Params, stateful,
         "Twenty half-octave bands, 31.25 Hz to 22.6 kHz (bands at or above 0.45 of the sample rate are left out). "
         "Range, Accuracy and Gain as in the 10-band equalizer."},
        {"wabcut.audio.graphicEq30", 1, "Graphic Equalizer (30 Bands)", kFilterEq, kAudio, false, kGraphicEq30Params, stateful,
         "Thirty third-octave bands, 31.25 Hz to 25.4 kHz (bands at or above 0.45 of the sample rate are left out). "
         "Range, Accuracy and Gain as in the 10-band equalizer."},
        {"wabcut.audio.parametricEq", 1, "Parametric Equalizer", kFilterEq, kAudio, false, kParametricEqParams, stateful,
         "High-pass and low-pass filters (Butterworth, 6 to 48 dB per octave), low and high shelves (first or second "
         "order), five peaking bands (their width a Q, or a bandwidth in Hz with Constant Width) and Gain. Ultra-Quiet "
         "Mode is accepted and changes nothing: the filters already run in double precision."},
        {"wabcut.audio.simpleParametricEq", 1, "Simple Parametric EQ", kFilterEq, kAudio, false, kSimpleParametricEqParams, stateful,
         "One peaking band: Boost at Center, its width set by Q."},
        {"wabcut.audio.highpass", 1, "Highpass", kFilterEq, kAudio, false, kCutoffParams, stateful,
         "Removes the frequencies below Cutoff (Butterworth, 12 dB per octave, -3 dB at Cutoff)."},
        {"wabcut.audio.lowpass", 1, "Lowpass", kFilterEq, kAudio, false, kCutoffParams, stateful,
         "Removes the frequencies above Cutoff (Butterworth, 12 dB per octave, -3 dB at Cutoff)."},
        {"wabcut.audio.bandpass", 1, "Bandpass", kFilterEq, kAudio, false, kCutoffQParams, stateful,
         "Keeps the band around Cutoff (0 dB there, -3 dB at the edges of a band Cutoff / Q wide) and removes the rest."},
        {"wabcut.audio.simpleNotch", 1, "Simple Notch Filter", kFilterEq, kAudio, false, kCutoffQParams, stateful,
         "Removes the frequencies near Cutoff (-3 dB at the edges of a band Cutoff / Q wide)."},
        {"wabcut.audio.bass", 1, "Bass", kFilterEq, kAudio, false, kBoostParams, stateful,
         "Boosts or cuts 200 Hz and below: a shelf with half its Boost at 200 Hz and all of it well below."},
        {"wabcut.audio.treble", 1, "Treble", kFilterEq, kAudio, false, kBoostParams, stateful,
         "Boosts or cuts 4000 Hz and above: a shelf with half its Boost at 4 kHz and all of it well above."},
        {"wabcut.audio.notchFilter", 1, "Notch Filter", kFilterEq, kAudio, false, kNotchFilterParams, stateful,
         "Up to six notches (each Enable, Frequency and Gain; a positive gain is a narrow boost). Notch Width: Narrow "
         "(2nd order, -3 dB edges an eighth of the frequency apart), Very Narrow (4th order, a sixteenth) or Super "
         "Narrow (6th order, a thirty-second). Amplify All gives every notch the Fixed Gain. Ultra Quiet is accepted and "
         "changes nothing (double precision)."},
        {"wabcut.audio.scientificFilter", 1, "Scientific Filter", kFilterEq, kAudio, false, kScientificFilterParams, stateful,
         "Classic analog designs (Bessel, Butterworth, Chebychev, Elliptical) as low pass, high pass, band pass or band "
         "stop, by the bilinear transform. Cutoff is the -3 dB point (Bessel, Butterworth) or the passband edge "
         "(Chebychev, Elliptical: Pass Ripple down there); band filters run from Cutoff to High Cutoff. With Order Mode "
         "off, the order is the lowest that is Stop Attn down a Transition Bandwidth beyond Cutoff (and High Width beyond "
         "High Cutoff), fixed when the effect is built; Bessel always uses Order Value. Elliptical uses both Pass Ripple "
         "and Stop Attn."},
        {"wabcut.audio.fftFilter", 1, "FFT Filter", kFilterEq, kAudio, false, kFftFilterParams, C::AudioStateful | C::AudioLookahead,
         "A gain curve through the first Number of X/Y pairs points (X in Hz, Y in dB), straight or with Splines, on a "
         "linear or logarithmic frequency Scale, applied to the spectrum (short-time Fourier transform, sqrt-Hann "
         "windows whatever the Window Type). Delays the audio by FFT Size samples (a power of two, 64 to 32768)."},
        {"wabcut.audio.stereoExpander", 1, "Stereo Expander", kStereoImagery, kAudio, false, kStereoExpanderParams, C::None,
         "Center Channel Pan moves the centre of the stereo image from hard left (-100) to hard right (100); Stereo "
         "Expander sets the width: Narrow (0, mono), Normal (100, unchanged) to Wide (300, three times the side "
         "signal). Mono and the channels after the front pair are untouched."},
        {"wabcut.audio.balance", 1, "Balance", kObsolete, kAudio, false, kBalanceParams, C::None,
         "Positive values turn the left channel down (more of the right), negative values the right channel."},
        {"wabcut.audio.channelVolume", 1, "Channel Volume", kAmplitude, kAudio, false, kChannelVolumeParams, C::None,
         "The level of each channel in dB (-144 is off): Left and Right, and Center, LFE, Left Surround and Right "
         "Surround of a 5.1 clip (channels 3 to 6). A mono clip takes Left."},
        {"wabcut.audio.fillLeftWithRight", 1, "Fill Left with Right", kSpecial, kAudio, false, {}, C::None,
         "The right channel plays on both sides (the left is discarded)."},
        {"wabcut.audio.fillRightWithLeft", 1, "Fill Right with Left", kSpecial, kAudio, false, {}, C::None,
         "The left channel plays on both sides (the right is discarded)."},
        {"wabcut.audio.invert", 1, "Invert", kSpecial, kAudio, false, {}, C::None, "Inverts the phase of every channel."},
        {"wabcut.audio.swapChannels", 1, "Swap Channels", kSpecial, kAudio, false, {}, C::None,
         "Swaps the left and right channels."},
    });
}

} // namespace wabcut::audiocatalog
