#pragma once

// A synthetic podcast for the shorts tests (AI-9): topics of distinct vocabulary, one opening with a hook question,
// one full of fillers and pauses, spoken at a steady 2.5 words a second on a linked V1/A1 clip of media A (25 fps).

#include "../EditTestUtil.h"

#include <cmath>
#include <string>
#include <vector>

namespace wabcut::ai::shorts::test {

using edit::test::W;
using wabcut::test::Timeline;

struct Topic {
    std::string name;
    std::vector<std::string> words;
    std::string opening; ///< first sentence, verbatim ("" = generated)
    bool messy = false;  ///< fillers and long pauses
};

inline std::vector<Topic> podcastTopics() {
    return {
        {"intro", {"welcome", "show", "episode", "guest", "today", "studio", "listeners", "season", "host", "friends", "chat", "journey"}, "", false},
        {"pricing", {"pricing", "price", "customers", "subscription", "plan", "revenue", "discount", "value", "monthly", "annual", "tier", "cost"},
         "Why did we double our price overnight?", false},
        {"hiring", {"hiring", "team", "engineers", "interview", "culture", "candidates", "recruiting", "manager", "onboarding", "salary", "remote", "roles"},
         "", true},
        {"product", {"product", "feature", "users", "design", "launch", "feedback", "roadmap", "prototype", "release", "bugs", "metrics", "build"}, "", false},
    };
}

struct Podcast {
    std::vector<W> words;                        ///< source frames at 25 fps
    std::vector<std::pair<double, double>> topics; ///< seconds, per topic
    double seconds = 0.0;
};

/// `sentencesPerTopic` sentences of 9 words per topic, `rounds` times over the topics.
inline Podcast makePodcast(int sentencesPerTopic = 12, int rounds = 1) {
    Podcast p;
    double t = 1.0;
    const auto add = [&](const std::string& text, double length = 0.4) {
        p.words.push_back(W{text, t * 25.0, (t + length) * 25.0});
        t += length + 0.1;
    };
    const std::vector<std::string> common{"we", "really", "think", "the", "about", "our", "with"};
    const auto topics = podcastTopics();
    for (int round = 0; round < rounds; ++round) {
        for (std::size_t k = 0; k < topics.size(); ++k) {
            const Topic& topic = topics[k];
            const double start = t;
            for (int s = 0; s < sentencesPerTopic; ++s) {
                if (s == 0 && !topic.opening.empty()) {
                    std::string word;
                    for (const char ch : topic.opening) {
                        if (ch == ' ') {
                            add(word);
                            word.clear();
                        } else {
                            word += ch;
                        }
                    }
                    add(word);
                    t += 0.4;
                    continue;
                }
                for (int w = 0; w < 9; ++w) {
                    std::string word = w % 3 == 1 ? common[static_cast<std::size_t>((s + w + round) % common.size())]
                                                  : topic.words[static_cast<std::size_t>((s * 5 + w * 7 + round) % topic.words.size())];
                    if (topic.messy && w == 4) {
                        add("um");
                        t += 0.5; // a hesitation (0.6 s, under a sentence break)
                    }
                    if (w == 8) {
                        word += '.';
                    }
                    add(word);
                }
                t += 0.4; // between sentences
            }
            p.topics.emplace_back(start, t);
            t += 0.6;
        }
    }
    p.seconds = t + 1.0;
    return p;
}

inline std::string avPicture(int frames) {
    const std::string cells = "A" + std::string(static_cast<std::size_t>(frames - 1), 'a');
    return "V1 |" + cells + "|\nA1 |" + cells + "|";
}

} // namespace wabcut::ai::shorts::test
