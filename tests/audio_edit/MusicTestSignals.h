#pragma once

// A synthetic song with a known bar structure for the Music Remix tests (A.7): 4/4 at a fixed tempo, kick on beats 1
// and 3 (the downbeat's louder, with a bass note: the bar line is audible), snare on 2 and 4, hi-hats on the eighths,
// one chord per bar, and sections that sound different (the chorus has a lead line and other chords, the intro and the
// outro only pads and a kick). Deterministic, and like a drum machine's: every hit of an instrument is the same
// sample, so bars that are the same music are the same audio.

#include <wabcut/ai/analysis/BeatDetection.h>
#include <wabcut/audio/edit/Remix.h>
#include <wabcut/model/Time.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

namespace wabcut::test {

struct SyntheticSong {
    std::vector<float> samples; ///< mono
    int rate = 22050;
    double bpm = 120.0;
    std::string sections;       ///< one letter per bar: I intro, A verse, B chorus, O outro
    double beatSeconds() const { return 60.0 / bpm; }
    double barSeconds() const { return 4.0 * beatSeconds(); }
    double seconds() const { return static_cast<double>(samples.size()) / rate; }
    /// The true beat times (beat k at k beats).
    std::vector<Ticks> beats() const {
        std::vector<Ticks> out;
        for (int k = 0; k < static_cast<int>(sections.size()) * 4; ++k) {
            out.push_back(Ticks{static_cast<std::int64_t>(std::llround(k * beatSeconds() * static_cast<double>(kTicksPerSecond)))});
        }
        return out;
    }
};

/// The song of `sections` (default: 4 intro bars, verse, chorus, verse, chorus of 8 bars each, 4 outro bars: 40 bars,
/// 80 s at 120 BPM).
inline SyntheticSong synthesizeSong(std::string sections = "IIIIAAAAAAAABBBBBBBBAAAAAAAABBBBBBBBOOOO", double bpm = 120.0,
                                    int rate = 22050) {
    SyntheticSong song;
    song.rate = rate;
    song.bpm = bpm;
    song.sections = std::move(sections);
    const double beat = 60.0 / bpm;
    const std::size_t total = static_cast<std::size_t>(std::llround(song.sections.size() * 4 * beat * rate));
    song.samples.assign(total, 0.0f);
    std::uint32_t seed = 12345;
    const auto rnd = [&seed] {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return static_cast<double>(seed) / 4294967295.0 * 2.0 - 1.0;
    };
    const auto midi = [](double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); };
    // Chords (root, third, fifth as MIDI notes), one per bar, cycling within the section.
    const std::vector<std::vector<int>> verse{{60, 64, 67}, {55, 59, 62}, {57, 60, 64}, {53, 57, 60}};  // C G Am F
    const std::vector<std::vector<int>> chorus{{53, 57, 60}, {55, 59, 62}, {52, 55, 59}, {57, 60, 64}}; // F G Em Am
    const std::vector<std::vector<int>> pad{{57, 60, 64}, {57, 60, 64}, {53, 57, 60}, {55, 59, 62}};    // Am Am F G
    const std::vector<int> lead{72, 74, 76, 79, 76, 74, 72, 71};
    const auto add = [&](std::size_t at, float v) {
        if (at < song.samples.size()) {
            song.samples[at] += v;
        }
    };
    for (std::size_t bar = 0; bar < song.sections.size(); ++bar) {
        const char s = song.sections[bar];
        const auto& chords = s == 'B' ? chorus : (s == 'A' ? verse : pad);
        // The bar's place in its section picks the chord (sections start on bar multiples of 4 here).
        std::size_t inSection = 0;
        for (std::size_t b = bar; b > 0 && song.sections[b - 1] == s; --b) {
            ++inSection;
        }
        const auto& chord = chords[inSection % chords.size()];
        const double barStart = static_cast<double>(bar) * 4.0 * beat;
        // Sample indices are integers (an index from a rounded time would write some samples twice and skip others).
        const auto barSample = static_cast<std::size_t>(std::llround(barStart * rate));
        // Chord: sustained over the bar with a gentle swell.
        for (std::size_t i = 0; i < static_cast<std::size_t>(std::llround(4.0 * beat * rate)); ++i) {
            const double t = static_cast<double>(i) / rate;
            const double env = 0.6 + 0.4 * std::sin(std::numbers::pi * t / (4.0 * beat));
            double v = 0.0;
            for (const int note : chord) {
                v += std::sin(2.0 * std::numbers::pi * midi(note) * (barStart + t));
            }
            add(barSample + i, static_cast<float>(0.06 * env * v));
        }
        for (int b = 0; b < 4; ++b) {
            const double at = barStart + b * beat;
            const auto start = static_cast<std::size_t>(std::llround(at * rate));
            // Kick on 1 and 3 (every section); snare on 2 and 4 and hats on the eighths (verse and chorus).
            if (b == 0 || b == 2) {
                const double level = b == 0 ? 0.8 : 0.45;
                for (std::size_t i = 0; i < static_cast<std::size_t>(0.15 * rate); ++i) {
                    const double t = static_cast<double>(i) / rate;
                    const double f = 50.0 + 90.0 * std::exp(-t * 30.0);
                    add(start + i, static_cast<float>(level * std::exp(-t * 18.0) * std::sin(2.0 * std::numbers::pi * f * t)));
                }
            }
            if (b == 0) {
                // The bass plays the chord's root an octave down on the downbeat.
                const double f = midi(chord.front() - 12);
                for (std::size_t i = 0; i < static_cast<std::size_t>(beat * rate); ++i) {
                    const double t = static_cast<double>(i) / rate;
                    add(start + i, static_cast<float>(0.25 * std::exp(-t * 3.0) * std::sin(2.0 * std::numbers::pi * f * t)));
                }
            }
            if ((s == 'A' || s == 'B') && (b == 1 || b == 3)) {
                seed = 12345; // the snare sample
                for (std::size_t i = 0; i < static_cast<std::size_t>(0.1 * rate); ++i) {
                    const double t = static_cast<double>(i) / rate;
                    add(start + i, static_cast<float>(std::exp(-t * 30.0) * (0.3 * rnd() + 0.2 * std::sin(2.0 * std::numbers::pi * 190.0 * t))));
                }
            }
            if (s == 'A' || s == 'B') {
                for (int e = 0; e < 2; ++e) {
                    const auto h = static_cast<std::size_t>(std::llround((at + e * beat / 2.0) * rate));
                    seed = 54321; // the hi-hat sample
                    double previous = 0.0;
                    for (std::size_t i = 0; i < static_cast<std::size_t>(0.03 * rate); ++i) {
                        const double n = rnd();
                        add(h + i, static_cast<float>(0.12 * std::exp(-static_cast<double>(i) / rate * 120.0) * (n - previous)));
                        previous = n;
                    }
                }
            }
            // The chorus's lead: an eighth-note line, a bright (odd-harmonic) tone.
            if (s == 'B') {
                for (int e = 0; e < 2; ++e) {
                    const double f = midi(lead[static_cast<std::size_t>((b * 2 + e) % 8)]);
                    const auto n0 = static_cast<std::size_t>(std::llround((at + e * beat / 2.0) * rate));
                    for (std::size_t i = 0; i < static_cast<std::size_t>(std::llround(beat / 2.0 * rate)); ++i) {
                        const double t = static_cast<double>(i) / rate;
                        const double w = 2.0 * std::numbers::pi * f * t;
                        add(n0 + i,
                            static_cast<float>(0.08 * std::exp(-t * 4.0) * (std::sin(w) + std::sin(3 * w) / 3.0 + std::sin(5 * w) / 5.0)));
                    }
                }
            }
        }
    }
    return song;
}

/// The song analysed as the media analyzer does it: the beat tracker, then the beat features.
inline audio::edit::MusicAnalysis analyseSong(const SyntheticSong& song) {
    const ai::analysis::MonoAudio audio{song.samples, song.rate, Ticks{}};
    auto beats = ai::analysis::detectBeats(audio);
    REQUIRE(beats);
    audio::edit::MusicAnalysis a;
    a.bpm = beats->bpm;
    a.beatConfidence = beats->confidence;
    a.beatsPerBar = beats->beatsPerBar;
    a.beats = beats->beats;
    a.downbeats = beats->downbeats;
    a.duration = Ticks{static_cast<std::int64_t>(std::llround(song.seconds() * static_cast<double>(kTicksPerSecond)))};
    a.features = audio::edit::beatFeatures(song.samples, song.rate, a.beats);
    return a;
}

/// The default song and its analysis, made once.
inline const SyntheticSong& defaultSong() {
    static const SyntheticSong s = synthesizeSong();
    return s;
}
inline const audio::edit::MusicAnalysis& defaultSongAnalysis() {
    static const audio::edit::MusicAnalysis a = analyseSong(defaultSong());
    return a;
}

} // namespace wabcut::test
