#pragma once

// Fixtures for the AI-11 creator tests: recordings whose transcripts hold several takes of the same lines (clean,
// messy, false starts), across media items, in English and Thai, plus a timing-accurate helper to lay words out.
// Media are the Timeline fixture's letters (25 fps, 1920x1080 video, two audio streams).

#include "../EditTestUtil.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/roughcut/RoughCutCommands.h>
#include <wabcut/command/Engine.h>

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wabcut::ai::roughcut::test {

using edit::test::W;
using wabcut::test::Timeline;

/// Lays out spoken words: each word lasts `perWord` seconds with `gap` between words, from `start` seconds. Words are
/// split at spaces (a Thai line passes its word units separated by spaces). Returns the time after the last word.
inline double say(std::vector<W>& words, double start, const std::string& text, double perWord = 0.32, double gap = 0.08,
                  std::optional<int> speaker = std::nullopt) {
    double t = start;
    std::string word;
    const auto flush = [&] {
        if (!word.empty()) {
            words.push_back(W{word, t * 25.0, (t + perWord) * 25.0, speaker});
            t += perWord + gap;
            word.clear();
        }
    };
    for (const char ch : text) {
        if (ch == ' ') {
            flush();
        } else {
            word += ch;
        }
    }
    flush();
    return t;
}

/// A take: what is said, where.
struct Said {
    char media = 'A';
    double start = 0.0; ///< seconds
    std::string text;
};

/// A project whose media letters have transcripts; the sequence is a short clip of 'Z' (no transcript).
struct Studio {
    explicit Studio(const std::vector<Said>& takes, std::string language = "en", std::map<char, std::int64_t> frames = {})
        : tl("V1 |Zzzz|\nA1 |Zzzz|", withDefaults(takes, frames)) {
        std::map<char, std::vector<W>> words;
        for (const Said& s : takes) {
            say(words[s.media], s.start, s.text);
        }
        transcripts = std::make_shared<edit::MapTranscriptProvider>();
        for (auto& [letter, list] : words) {
            std::ranges::sort(list, [](const W& a, const W& b) { return a.from < b.from; });
            transcripts->add(tl.media(letter), 0, edit::test::transcript(language, list));
            letters.push_back(letter);
        }
        services = std::make_shared<command::ServiceBundle>(
            std::vector<std::shared_ptr<command::CommandServices>>{std::make_shared<edit::TranscriptServices>(transcripts)});
    }

    std::vector<MediaId> media() const {
        std::vector<MediaId> out;
        for (const char c : letters) {
            out.push_back(tl.media(c));
        }
        return out;
    }
    command::Json mediaJson() const {
        command::Json out = command::Json::array();
        for (const char c : letters) {
            out.push_back(tl.media(c).value);
        }
        return out;
    }
    SpeechCorpus corpus() const { return mediaCorpus(tl.state, media(), *transcripts, nullptr); }

    static std::map<char, std::int64_t> withDefaults(const std::vector<Said>& takes, std::map<char, std::int64_t> frames) {
        for (const Said& s : takes) {
            const auto end = static_cast<std::int64_t>(std::ceil((s.start + 30.0) * 25.0));
            frames[s.media] = std::max(frames.contains(s.media) ? frames[s.media] : 1000, end);
        }
        return frames;
    }

    Timeline tl;
    std::vector<char> letters;
    std::shared_ptr<edit::MapTranscriptProvider> transcripts;
    std::shared_ptr<command::ServiceBundle> services;
};

/// Two recordings of a short launch video: several takes per line, fillers, a false start, an unrelated aside.
inline std::vector<Said> launchTakes() {
    return {
        {'A', 1.0, "Hi everyone, welcome to the channel."},
        {'A', 4.0, "Um, today we are, uh, looking at the new editor."},
        {'A', 9.0, "Today we are looking at the new editor."},
        {'A', 13.0, "It cuts a whole video in seconds."},
        {'A', 17.0, "It cuts a, it cuts a whole video in seconds."},
        {'A', 23.0, "Let me grab some water quickly."},
        {'A', 26.0, "Thanks for watching, see you next time."},
        {'B', 1.0, "So the best part is the price."},
        {'B', 5.0, "The best part is the price, it is free."},
        {'B', 10.0, "Today we are looking at the new editor!"},
    };
}

inline const char* kLaunchScript = R"(# Launch video
## Intro
Hi everyone, welcome to the channel.
Today we're looking at the new editor.
## Demo
It cuts a whole video in seconds.
The best part is the price.
[B-roll: price tag close-up]
Thanks for watching!
)";

} // namespace wabcut::ai::roughcut::test
