#pragma once

// p02 audio: Essential Sound auto-tagging (Premiere's automatic Dialogue / Music / SFX / Ambience tag of a clip): a
// local, deterministic classifier over a clip's sound. No model and nothing leaves the computer: a handful of features
// of the signal, each measuring one thing a listener hears, and a fixed scoring of them.
//
// Features (frames of 32 ms every 16 ms at 16 kHz; the input is decimated to about 16 kHz first):
//   * syllabic modulation: the share of the level envelope's modulation in 2.5..7 Hz (speech's syllable rate) against
//     0.5..16 Hz;
//   * voiced share: frames with a clear period in 80..400 Hz (normalised autocorrelation), and of consecutive voiced
//     frames, the share that glide (0.08..2 semitones apart: intonation) and the share that hold their pitch (notes);
//   * pause share: frames 30 dB under the loud ones (speech has short gaps, music few, ambience none);
//   * beat regularity: the strongest periodicity of the onset envelope between 0.25 and 1.5 s;
//   * spectral flatness: noise-like (rain, wind, room tone) against tonal;
//   * crest: how far the loudest moments stand above the typical level (impacts, hits);
//   * level spread: how much the level moves (steady beds against events).
// Each type's score is a weighted sum of soft thresholds of those features (scores in 0..1); the type is the highest,
// the confidence its margin over the next. The same samples always give the same tag.

#include <wabcut/audio/edit/EssentialSound.h>

#include <array>
#include <span>
#include <string>

namespace wabcut::audio::edit {

struct SoundFeatures {
    double seconds = 0.0;         ///< analysed
    double syllabicModulation = 0.0;
    double voicedShare = 0.0;
    double glideShare = 0.0;      ///< of consecutive voiced frames
    double steadyShare = 0.0;     ///< of consecutive voiced frames
    double pauseShare = 0.0;
    double beatRegularity = 0.0;
    double flatness = 0.0;        ///< mean over audible frames, 0 (tonal) .. 1 (white noise)
    double crestDb = 0.0;         ///< 99th percentile frame level over the median, dB
    double levelSpreadDb = 0.0;   ///< interquartile range of audible frame levels, dB
    double levelDb = -144.0;      ///< median audible frame level, dBFS
};

struct SoundGuess {
    SoundType type = SoundType::Sfx;
    double confidence = 0.0;           ///< 0..1: the winner's margin over the runner-up, scaled
    std::array<double, 4> scores{};    ///< dialogue, music, sfx, ambience (0..1)
    SoundFeatures features;
    bool silent = false;               ///< nothing audible: no tag proposed
};

/// The features of mono samples at `sampleRate` (8 kHz or more).
SoundFeatures soundFeatures(std::span<const float> mono, int sampleRate);
/// The tag the features suggest.
SoundGuess classifySound(const SoundFeatures& features);
/// Both, from samples.
SoundGuess classifySound(std::span<const float> mono, int sampleRate);

} // namespace wabcut::audio::edit
