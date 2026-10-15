#pragma once

// Synthetic gameplay for the highlight tests (G.2): a recording with scripted moments and decoys, made of sound and
// pictures that the real measurements have to find (nothing here hands over activations).
//
//   game audio  a noise bed with small random hits; at each scripted event a loud blast (+16..20 dB, 1.5 s)
//   mic         speech-like voiced syllables (harmonic tones at ~120 Hz) in sentences and pauses; at events the
//               player shouts (+12 dB, a fifth higher) or laughs (5 Hz voiced bursts)
//   picture     small frames of slowly drifting texture; at events 3 s of fast action and a flash
//   chat        a Twitch VOD log (TwitchDownloader JSON): ~0.5 messages a second, 25 reactions 4-7 s after events
//   markers     chapter markers 3 s after some events (the player pressed the hotkey)
//   decoys      a game sound at +8 dB alone, a lone flash (a menu), a short burst of chat
//
// Deterministic (fixed seeds). Events can leave signals out, to test what each signal adds.

#include <wabcut/ai/analysis/ShotDetection.h>
#include <wabcut/ai/highlights/Highlights.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace wabcut::test::gameplay {

struct Event {
    double at = 0.0;   ///< seconds
    bool game = true;  ///< a blast in the game audio
    bool shout = true; ///< the player shouts (else laughs when `laugh`)
    bool laugh = false;
    bool visual = true;
    bool chat = true;
    bool marker = false;
};

struct Script {
    double seconds = 480.0;
    std::vector<Event> events;
    std::vector<double> decoyGame;  ///< lone +8 dB game sounds
    std::vector<double> decoyFlash; ///< lone flashes
    std::vector<double> decoyChat;  ///< short chat bursts (8 messages, no keywords)
};

inline Script defaultScript() {
    Script s;
    s.seconds = 480.0;
    s.events = {
        {30.0, true, true, false, true, true, false},   // everything
        {75.0, true, true, false, true, false, true},   // no chat, marked
        {118.0, true, false, true, false, true, false}, // a laugh, no picture
        {160.0, true, true, false, true, true, false},
        {205.0, false, true, false, true, true, true},  // a shout over a quiet game, marked
        {250.0, true, false, false, true, true, false}, // game + picture + chat, the player silent
        {300.0, true, true, false, true, false, false},
        {352.0, false, false, true, false, true, false}, // a laugh and chat only
        {410.0, true, true, false, true, true, false},
    };
    s.decoyGame = {52.0, 140.0, 330.0, 440.0};
    s.decoyFlash = {96.0, 275.0};
    s.decoyChat = {185.0, 385.0};
    return s;
}

inline std::vector<Ticks> eventTimes(const Script& s) {
    std::vector<Ticks> out;
    for (const Event& e : s.events) {
        out.push_back(Ticks{static_cast<std::int64_t>(std::llround(e.at * static_cast<double>(kTicksPerSecond)))});
    }
    return out;
}

/// Game audio at `rate` (mono).
inline std::vector<float> gameAudio(const Script& s, int rate = ai::highlights::kAudioRate) {
    const auto n = static_cast<std::size_t>(s.seconds * rate);
    std::vector<float> out(n);
    std::mt19937 rng(11);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    float low = 0.0f;
    const double bed = std::pow(10.0, -32.0 / 20.0) * 2.0;
    for (std::size_t i = 0; i < n; ++i) {
        low = 0.97f * low + 0.03f * noise(rng); // a duller noise bed
        const double t = static_cast<double>(i) / rate;
        const double drift = 1.0 + 0.25 * std::sin(t * 0.21);
        out[i] = static_cast<float>(bed * drift * (0.6 * low * 4.0 + 0.4 * noise(rng) * 0.5));
    }
    const auto blast = [&](double at, double db, double length) {
        const auto from = static_cast<std::size_t>(at * rate);
        const auto count = static_cast<std::size_t>(length * rate);
        const double gain = std::pow(10.0, db / 20.0);
        for (std::size_t k = 0; k < count && from + k < n; ++k) {
            const double env = std::exp(-3.0 * static_cast<double>(k) / static_cast<double>(count)) * std::min(1.0, k / (0.01 * rate));
            out[from + k] += static_cast<float>(bed * gain * env * noise(rng) * 0.7);
        }
    };
    std::mt19937 hits(5);
    std::uniform_real_distribution<double> when(0.0, s.seconds);
    for (int k = 0; k < 60; ++k) {
        blast(when(hits), 4.0, 0.3); // ordinary gunfire / footsteps
    }
    for (const Event& e : s.events) {
        if (e.game) {
            blast(e.at - 0.3, 18.0, 1.5);
            blast(e.at + 0.5, 16.0, 1.0);
        }
    }
    for (double d : s.decoyGame) {
        blast(d, 8.0, 0.6);
    }
    return out;
}

/// The player's microphone at `rate` (mono).
inline std::vector<float> micAudio(const Script& s, int rate = ai::highlights::kAudioRate) {
    const auto n = static_cast<std::size_t>(s.seconds * rate);
    std::vector<float> out(n, 0.0f);
    std::mt19937 rng(23);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    const double speech = std::pow(10.0, -26.0 / 20.0);
    double phase = 0.0;
    const auto voice = [&](double from, double length, double f0, double gain, double pulseHz) {
        const auto a = static_cast<std::size_t>(from * rate);
        const auto count = static_cast<std::size_t>(length * rate);
        for (std::size_t k = 0; k < count && a + k < n; ++k) {
            const double t = static_cast<double>(k) / rate;
            const double f = f0 * (1.0 + 0.03 * std::sin(t * 9.0));
            phase += 2.0 * 3.14159265358979 * f / rate;
            double v = 0.0;
            for (int h = 1; h <= 8; ++h) {
                v += std::sin(phase * h) / (h * 1.3);
            }
            double env = std::min(1.0, std::min(t, length - t) / 0.02);
            if (pulseHz > 0.0) {
                env *= std::pow(std::sin(3.14159265358979 * pulseHz * t), 2.0);
            }
            out[a + k] += static_cast<float>(speech * gain * env * v * 0.5);
        }
    };
    // Talking: sentences of syllables, then pauses.
    double t = 1.0;
    while (t < s.seconds - 3.0) {
        const double sentence = 1.5 + 2.5 * uni(rng);
        const double end = t + sentence;
        const double pitch = 110.0 + 25.0 * uni(rng);
        while (t < end) {
            const double syllable = 0.12 + 0.12 * uni(rng);
            voice(t, syllable, pitch * (0.92 + 0.16 * uni(rng)), 0.8 + 0.4 * uni(rng), 0.0);
            t += syllable + 0.04 + 0.08 * uni(rng);
        }
        t += 1.0 + 2.5 * uni(rng);
    }
    for (const Event& e : s.events) {
        if (e.shout) {
            voice(e.at + 0.4, 1.3, 185.0, 4.0, 0.0); // +12 dB, a fifth up
        } else if (e.laugh) {
            voice(e.at + 0.3, 1.6, 160.0, 2.2, 5.0);
        }
    }
    // Room noise, quiet.
    for (std::size_t i = 0; i < n; ++i) {
        out[i] += 0.0006f * noise(rng);
    }
    return out;
}

/// Small frames (64x36) at `fps`.
inline std::vector<ai::analysis::AnalysisFrame> frames(const Script& s, int fps = 30, int w = 64, int h = 36) {
    std::vector<ai::analysis::AnalysisFrame> out;
    const auto count = static_cast<std::size_t>(s.seconds * fps);
    out.reserve(count);
    std::mt19937 rng(37);
    std::uniform_int_distribution<int> byte(0, 255);
    const auto within = [](double t, double at, double before, double after) { return t >= at - before && t < at + after; };
    for (std::size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / fps;
        bool action = false;
        bool flash = false;
        for (const Event& e : s.events) {
            action = action || (e.visual && within(t, e.at, 1.0, 2.0));
            flash = flash || (e.visual && within(t, e.at + 0.2, 0.0, 0.1));
        }
        for (double d : s.decoyFlash) {
            flash = flash || within(t, d, 0.0, 0.1);
        }
        ai::analysis::AnalysisFrame f;
        f.pts = Ticks{static_cast<std::int64_t>(i) * (kTicksPerSecond / fps)};
        f.duration = Ticks{kTicksPerSecond / fps};
        f.width = w;
        f.height = h;
        f.rgb.resize(static_cast<std::size_t>(w * h * 3));
        const double drift = t * 2.0; // the camera pans slowly
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int v = static_cast<int>(90.0 + 40.0 * std::sin((x + drift) * 0.3) * std::cos(y * 0.25 + t * 0.2));
                if (action) {
                    v = std::clamp(v + byte(rng) / 2 - 64, 0, 255); // fast, noisy action
                }
                if (flash) {
                    v = 245;
                }
                unsigned char* p = &f.rgb[static_cast<std::size_t>((y * w + x) * 3)];
                p[0] = static_cast<unsigned char>(v);
                p[1] = static_cast<unsigned char>(std::clamp(v - 10, 0, 255));
                p[2] = static_cast<unsigned char>(std::clamp(v + 10, 0, 255));
            }
        }
        out.push_back(std::move(f));
    }
    return out;
}

/// A TwitchDownloader-style chat log.
inline std::string chatJson(const Script& s) {
    std::mt19937 rng(41);
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    const std::vector<std::string> chatter{"hi", "what rank is this", "nice", "how long have you played", "lol ok", "love the stream",
                                           "what settings", "first time here", "gl", "which map"};
    const std::vector<std::string> reactions{"KEKW", "POGGERS", "Pog", "OMEGALUL", "no way", "CLIP IT", "insane"};
    nlohmann::json comments = nlohmann::json::array();
    const auto message = [&](double at, const std::string& text, bool emote) {
        nlohmann::json fragments = nlohmann::json::array();
        fragments.push_back({{"text", text}, {"emoticon", emote ? nlohmann::json{{"emoticon_id", "1"}} : nlohmann::json(nullptr)}});
        comments.push_back({{"content_offset_seconds", at}, {"message", {{"body", text}, {"fragments", fragments}}}});
    };
    for (double t = 0.0; t < s.seconds; t += -std::log(1.0 - uni(rng)) / 0.5) {
        message(t, chatter[static_cast<std::size_t>(uni(rng) * static_cast<double>(chatter.size())) % chatter.size()], false);
    }
    for (const Event& e : s.events) {
        if (!e.chat) {
            continue;
        }
        for (int k = 0; k < 25; ++k) {
            const std::string& r = reactions[static_cast<std::size_t>(k) % reactions.size()];
            message(e.at + 4.0 + 3.0 * uni(rng), r, r != "no way" && r != "CLIP IT" && r != "insane");
        }
    }
    for (double d : s.decoyChat) {
        for (int k = 0; k < 8; ++k) {
            message(d + 2.0 * uni(rng), chatter[static_cast<std::size_t>(k) % chatter.size()], false);
        }
    }
    std::stable_sort(comments.begin(), comments.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a["content_offset_seconds"].get<double>() < b["content_offset_seconds"].get<double>();
    });
    return nlohmann::json{{"comments", comments}, {"video", {{"title", "synthetic"}}}}.dump();
}

/// Chapter markers 3 s after the marked events.
inline std::vector<ai::highlights::Hint> markers(const Script& s) {
    std::vector<ai::highlights::Hint> out;
    int k = 0;
    for (const Event& e : s.events) {
        if (e.marker) {
            out.push_back({Ticks{static_cast<std::int64_t>(std::llround((e.at + 3.0) * static_cast<double>(kTicksPerSecond)))},
                           "Clip " + std::to_string(++k), "chapter"});
        }
    }
    return out;
}

} // namespace wabcut::test::gameplay
