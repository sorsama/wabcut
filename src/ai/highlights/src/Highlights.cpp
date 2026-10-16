#include <wabcut/ai/highlights/Highlights.h>

#include <wabcut/ai/analysis/AnalysisCommands.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace wabcut::ai::highlights {

using nlohmann::json;

namespace {

constexpr std::size_t kBaselineHalfWindow = 300; // +-30 s
constexpr std::size_t kChatBaselineHalfWindow = 600; // +-60 s
constexpr std::size_t kAroundHalf = 25;          // the "5 s around" counts: +-2.5 s

float smooth01(double x) {
    const double t = std::clamp(x, 0.0, 1.0);
    return static_cast<float>(t * t * (3.0 - 2.0 * t));
}

double seconds(Ticks t) { return t.toSeconds(); }
Ticks ticksOf(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

/// A moving median over +-`half` samples, computed on one-second blocks (exact enough for a baseline, and linear).
std::vector<float> movingMedian(const std::vector<float>& x, std::size_t half) {
    const std::size_t n = x.size();
    std::vector<float> out(n, 0.0f);
    if (n == 0) {
        return out;
    }
    const std::size_t block = kFeatureRate;
    const std::size_t blocks = (n + block - 1) / block;
    std::vector<float> medians(blocks);
    std::vector<float> scratch;
    for (std::size_t b = 0; b < blocks; ++b) {
        scratch.assign(x.begin() + static_cast<std::ptrdiff_t>(b * block), x.begin() + static_cast<std::ptrdiff_t>(std::min(n, (b + 1) * block)));
        auto mid = scratch.begin() + static_cast<std::ptrdiff_t>(scratch.size() / 2);
        std::nth_element(scratch.begin(), mid, scratch.end());
        medians[b] = *mid;
    }
    const std::size_t halfBlocks = std::max<std::size_t>(1, half / block);
    std::vector<float> blockBaseline(blocks);
    for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t from = b > halfBlocks ? b - halfBlocks : 0;
        const std::size_t to = std::min(blocks, b + halfBlocks + 1);
        scratch.assign(medians.begin() + static_cast<std::ptrdiff_t>(from), medians.begin() + static_cast<std::ptrdiff_t>(to));
        auto mid = scratch.begin() + static_cast<std::ptrdiff_t>(scratch.size() / 2);
        std::nth_element(scratch.begin(), mid, scratch.end());
        blockBaseline[b] = *mid;
    }
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = blockBaseline[i / block];
    }
    return out;
}

/// The largest value within +-`half` samples.
std::vector<float> dilate(const std::vector<float>& x, std::size_t half) {
    std::vector<float> out(x.size(), 0.0f);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const std::size_t from = i > half ? i - half : 0;
        const std::size_t to = std::min(x.size(), i + half + 1);
        out[i] = *std::max_element(x.begin() + static_cast<std::ptrdiff_t>(from), x.begin() + static_cast<std::ptrdiff_t>(to));
    }
    return out;
}

/// Sum over +-`half` samples.
std::vector<float> windowSum(const std::vector<float>& x, std::size_t half) {
    std::vector<double> prefix(x.size() + 1, 0.0);
    for (std::size_t i = 0; i < x.size(); ++i) {
        prefix[i + 1] = prefix[i] + x[i];
    }
    std::vector<float> out(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        const std::size_t from = i > half ? i - half : 0;
        const std::size_t to = std::min(x.size(), i + half + 1);
        out[i] = static_cast<float>(prefix[to] - prefix[from]);
    }
    return out;
}

double median(std::vector<float> v) {
    if (v.empty()) {
        return 0.0;
    }
    auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

/// Every series of `s` that exists is `n` long (zero padded); `length` = n.
void resizeAll(MediaSignals& s, std::size_t n) {
    for (std::vector<float>* v : {&s.game, &s.mic, &s.visual, &s.chat, &s.gameRiseDb, &s.voiceRiseDb, &s.pitchRise, &s.laugh,
                                  &s.motionRatio, &s.flash, &s.cuts, &s.chatRatio, &s.chatCount}) {
        if (!v->empty()) {
            v->resize(n, 0.0f);
        }
    }
    s.length = n;
}

/// Makes room for `n` samples and returns `v` sized to the signals' length (created when absent).
std::vector<float>& series(MediaSignals& s, std::vector<float>& v) {
    if (v.size() < s.length) {
        v.resize(s.length, 0.0f);
    }
    return v;
}

void grow(MediaSignals& s, std::size_t n) {
    if (n > s.length) {
        resizeAll(s, n);
    }
}

void maxInto(std::vector<float>& into, const std::vector<float>& from) {
    for (std::size_t i = 0; i < std::min(into.size(), from.size()); ++i) {
        into[i] = std::max(into[i], from[i]);
    }
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string fixed(double v, int decimals) {
    char text[32];
    std::snprintf(text, sizeof text, "%.*f", decimals, v);
    return text;
}

} // namespace

// --- Roles ---------------------------------------------------------------------------------------------------------

std::string_view toString(AudioRole role) {
    switch (role) {
    case AudioRole::Game:
        return "game";
    case AudioRole::Mic:
        return "mic";
    case AudioRole::Mixed:
        return "mixed";
    }
    return "mixed";
}

std::optional<AudioRole> audioRoleFromString(std::string_view text) {
    if (text == "game") {
        return AudioRole::Game;
    }
    if (text == "mic") {
        return AudioRole::Mic;
    }
    if (text == "mixed") {
        return AudioRole::Mixed;
    }
    return std::nullopt;
}

double voicedShare(const AudioFeatures& f) {
    double active = 0.0;
    double voiced = 0.0;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f.levelDb[i] > -50.0f) {
            active += 1.0;
            voiced += f.voiced[i];
        }
    }
    return active > 0.0 ? voiced / active : 0.0;
}

std::vector<AudioRole> classifyStreams(const std::vector<const AudioFeatures*>& streams) {
    std::vector<AudioRole> roles(streams.size(), AudioRole::Game);
    if (streams.size() == 1) {
        roles[0] = AudioRole::Mixed;
        return roles;
    }
    std::size_t best = streams.size();
    double bestShare = 0.0;
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const double share = streams[i] ? voicedShare(*streams[i]) : 0.0;
        if (share > bestShare) {
            bestShare = share;
            best = i;
        }
    }
    if (best < streams.size() && bestShare >= 0.25) {
        roles[best] = AudioRole::Mic;
    }
    return roles;
}

// --- Signals -------------------------------------------------------------------------------------------------------

void addAudio(MediaSignals& s, const AudioFeatures& f, AudioRole role) {
    const std::size_t n = f.size();
    if (n == 0) {
        return;
    }
    grow(s, n);
    if (role == AudioRole::Game || role == AudioRole::Mixed) {
        const std::vector<float> base = movingMedian(f.levelDb, kBaselineHalfWindow);
        std::vector<float> act(n, 0.0f);
        std::vector<float> rise(n, 0.0f);
        for (std::size_t i = 0; i < n; ++i) {
            if (f.levelDb[i] > -45.0f) {
                rise[i] = std::max(0.0f, f.levelDb[i] - std::max(base[i], -70.0f));
                act[i] = smooth01((rise[i] - 6.0) / 12.0);
            }
        }
        maxInto(series(s, s.game), act);
        maxInto(series(s, s.gameRiseDb), rise);
    }
    if (role == AudioRole::Mic || role == AudioRole::Mixed) {
        std::vector<float> speakingLevels;
        std::vector<float> pitches;
        for (std::size_t i = 0; i < n; ++i) {
            if (f.levelDb[i] > -50.0f && f.voiced[i] >= 0.4f) {
                speakingLevels.push_back(f.levelDb[i]);
                if (f.pitchHz[i] > 0.0f) {
                    pitches.push_back(f.pitchHz[i]);
                }
            }
        }
        if (speakingLevels.size() >= 50) { // 5 s of speech, or this is not a voice
            const double usualLevel = median(speakingLevels);
            const double usualPitch = pitches.empty() ? 0.0 : median(pitches);
            std::vector<float> act(n, 0.0f);
            std::vector<float> rise(n, 0.0f);
            std::vector<float> pitchUp(n, 0.0f);
            std::vector<float> laugh(n, 0.0f);
            for (std::size_t i = 0; i < n; ++i) {
                if (f.levelDb[i] <= -50.0f) {
                    continue;
                }
                const bool voice = f.voiced[i] >= 0.2f || f.burst[i] >= 0.4f;
                if (!voice && role == AudioRole::Mixed) {
                    continue; // a mixed track's loud game sound is not the player's voice
                }
                rise[i] = static_cast<float>(std::max(0.0, f.levelDb[i] - usualLevel));
                if (f.pitchHz[i] > 0.0f && usualPitch > 0.0) {
                    pitchUp[i] = static_cast<float>(std::max(0.0, 12.0 * std::log2(f.pitchHz[i] / usualPitch)));
                }
                // Talking pulses at the syllable rate too: a laugh is also louder than the speaker's usual level.
                laugh[i] = f.burst[i] * smooth01((rise[i] - 2.0) / 6.0);
                const float loud = smooth01((rise[i] - 5.0) / 10.0) * (voice ? 1.0f : 0.5f);
                const float high = smooth01((pitchUp[i] - 2.0) / 5.0) * smooth01((rise[i] + 3.0) / 6.0);
                act[i] = std::max({loud, high, 0.9f * laugh[i]});
            }
            // Speech pauses between words: excitement spans them.
            maxInto(series(s, s.mic), dilate(act, 5));
            maxInto(series(s, s.voiceRiseDb), dilate(rise, 5));
            maxInto(series(s, s.pitchRise), dilate(pitchUp, 5));
            maxInto(series(s, s.laugh), dilate(laugh, 5));
        }
    }
}

void addVideo(MediaSignals& s, const VisualFeatures& f) {
    const std::size_t n = f.size();
    if (n == 0) {
        return;
    }
    grow(s, n);
    const std::vector<float> base = movingMedian(f.motion, kBaselineHalfWindow);
    const std::vector<float> cuts = windowSum(f.cut, kAroundHalf);
    std::vector<float> act(n, 0.0f);
    std::vector<float> ratio(n, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        ratio[i] = f.motion[i] / std::max(base[i], 0.004f);
        const float motion = f.motion[i] > 0.01f ? smooth01((ratio[i] - 1.6) / 2.4) : 0.0f;
        const float changes = smooth01((cuts[i] - 1.0) / 3.0);
        act[i] = std::max(std::min(1.0f, 0.6f * motion + 0.4f * changes), f.flash[i]);
    }
    maxInto(series(s, s.visual), act);
    maxInto(series(s, s.motionRatio), ratio);
    maxInto(series(s, s.flash), f.flash);
    maxInto(series(s, s.cuts), cuts);
}

void addChat(MediaSignals& s, const ChatLog& chat, const ChatOptions& options) {
    if (chat.messages.empty()) {
        return;
    }
    std::unordered_set<std::string> keywords;
    for (const std::string& k : options.keywords) {
        keywords.insert(lower(k));
    }
    const auto indexOf = [&](double t) -> std::optional<std::size_t> {
        const double media = t + options.offsetSeconds - options.reactionSeconds;
        if (media < 0.0) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(media * kFeatureRate);
    };
    std::size_t needed = s.length;
    if (const auto last = indexOf(chat.messages.back().seconds)) {
        needed = std::max(needed, *last + 1);
    }
    if (s.length == 0) {
        grow(s, needed); // only chat: it sets the length
    }
    std::vector<float> counts(s.length, 0.0f);
    std::vector<float> hits(s.length, 0.0f);
    std::vector<KeywordHit> found;
    for (const ChatMessage& m : chat.messages) {
        const auto index = indexOf(m.seconds);
        if (!index || *index >= s.length) {
            continue;
        }
        counts[*index] += 1.0f;
        std::set<std::string> words;
        for (const std::string& e : m.emotes) {
            if (keywords.contains(lower(e))) {
                words.insert(e);
            }
        }
        std::string word;
        for (std::size_t i = 0; i <= m.text.size(); ++i) {
            const char c = i < m.text.size() ? m.text[i] : ' ';
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                word += c;
                continue;
            }
            if (!word.empty() && keywords.contains(lower(word))) {
                words.insert(word);
            }
            word.clear();
        }
        if (!words.empty()) {
            hits[*index] += 1.0f;
            for (const std::string& w : words) {
                found.push_back({*index, w});
            }
        }
    }
    const std::vector<float> around = windowSum(counts, kAroundHalf);
    const std::vector<float> keywordAround = windowSum(hits, kAroundHalf);
    std::vector<float> rate(s.length);
    for (std::size_t i = 0; i < s.length; ++i) {
        rate[i] = around[i] / 5.0f;
    }
    const std::vector<float> base = movingMedian(rate, kChatBaselineHalfWindow);
    std::vector<float> act(s.length, 0.0f);
    std::vector<float> ratio(s.length, 0.0f);
    for (std::size_t i = 0; i < s.length; ++i) {
        ratio[i] = rate[i] / std::max(base[i], 0.1f);
        const float busy = around[i] >= 3.0f ? smooth01((ratio[i] - 1.8) / 3.2) : 0.0f;
        const float words = smooth01((keywordAround[i] - 2.0) / 6.0);
        act[i] = std::max(busy, words);
    }
    maxInto(series(s, s.chat), act);
    maxInto(series(s, s.chatRatio), ratio);
    maxInto(series(s, s.chatCount), around);
    s.keywords.insert(s.keywords.end(), found.begin(), found.end());
    std::stable_sort(s.keywords.begin(), s.keywords.end(), [](const KeywordHit& a, const KeywordHit& b) { return a.index < b.index; });
}

void addHints(MediaSignals& s, std::vector<Hint> hints) {
    s.hints.insert(s.hints.end(), std::make_move_iterator(hints.begin()), std::make_move_iterator(hints.end()));
    std::stable_sort(s.hints.begin(), s.hints.end(), [](const Hint& a, const Hint& b) { return a.at < b.at; });
}

// --- On a sequence -------------------------------------------------------------------------------------------------

MediaSignals mapToSequence(const ProjectState& state, SequenceId sequenceId, const SignalsByMedia& media,
                           std::vector<std::string>* warnings) {
    MediaSignals out;
    const Sequence* seq = state.findSequence(sequenceId);
    if (!seq) {
        return out;
    }
    const Ticks end = std::max(seq->duration, seq->contentEnd());
    const std::size_t n = featureIndex(end) + (end > featureTime(featureIndex(end)) ? 1 : 0);
    out.length = n;
    bool any[4] = {false, false, false, false};
    for (const auto& [id, signals] : media) {
        if (!signals) {
            continue;
        }
        any[0] = any[0] || !signals->game.empty();
        any[1] = any[1] || !signals->mic.empty();
        any[2] = any[2] || !signals->visual.empty();
        any[3] = any[3] || !signals->chat.empty();
    }
    if (any[0]) {
        out.game.assign(n, 0.0f);
        out.gameRiseDb.assign(n, 0.0f);
    }
    if (any[1]) {
        out.mic.assign(n, 0.0f);
        out.voiceRiseDb.assign(n, 0.0f);
        out.pitchRise.assign(n, 0.0f);
        out.laugh.assign(n, 0.0f);
    }
    if (any[2]) {
        out.visual.assign(n, 0.0f);
        out.motionRatio.assign(n, 0.0f);
        out.flash.assign(n, 0.0f);
        out.cuts.assign(n, 0.0f);
    }
    if (any[3]) {
        out.chat.assign(n, 0.0f);
        out.chatRatio.assign(n, 0.0f);
        out.chatCount.assign(n, 0.0f);
    }
    std::set<std::string> notes;
    std::set<std::pair<std::size_t, std::string>> keywordsSeen;
    std::set<std::pair<std::int64_t, std::string>> hintsSeen;
    const auto copyAt = [](std::vector<float>& to, const std::vector<float>& from, std::size_t i, std::size_t j) {
        if (!to.empty() && j < from.size()) {
            to[i] = std::max(to[i], from[j]);
        }
    };
    // Bottom video track first: a higher track's picture replaces it.
    std::vector<std::pair<const Track*, bool>> tracks;
    for (const TrackPtr& t : seq->videoTracks) {
        tracks.emplace_back(t.get(), true);
    }
    for (const TrackPtr& t : seq->audioTracks) {
        tracks.emplace_back(t.get(), false);
    }
    for (const auto& [track, isVideo] : tracks) {
        if (!track || !track->enabled || (!isVideo && track->muted)) {
            continue;
        }
        for (const Clip& clip : track->clips) {
            if (!clip.enabled) {
                continue;
            }
            if (clip.isNested()) {
                notes.insert("nested and multicam clips are not analysed (open them to find their highlights)");
                continue;
            }
            const auto found = media.find(clip.media.value);
            if (found == media.end() || !found->second) {
                if (const MediaItem* item = state.findMedia(clip.media); item && !item->isGenerator()) {
                    notes.insert("'" + item->name + "' has no highlight analysis");
                }
                continue;
            }
            const MediaSignals& m = *found->second;
            const std::size_t first = featureIndex(clip.start());
            const std::size_t last = std::min(n, featureIndex(clip.end()) + 1);
            for (std::size_t i = first; i < last; ++i) {
                const Ticks t = featureTime(i) + Ticks{kTicksPerSecond / (2 * kFeatureRate)}; // the sample's middle
                if (t < clip.start() || t >= clip.end()) {
                    continue;
                }
                const Ticks source = clip.sourceTimeAt(t);
                if (source < Ticks{}) {
                    continue;
                }
                const std::size_t j = featureIndex(source);
                if (isVideo && !m.visual.empty() && j < m.visual.size()) {
                    // The higher track's picture is what shows.
                    out.visual[i] = m.visual[j];
                    out.motionRatio[i] = j < m.motionRatio.size() ? m.motionRatio[j] : 0.0f;
                    out.flash[i] = j < m.flash.size() ? m.flash[j] : 0.0f;
                    out.cuts[i] = j < m.cuts.size() ? m.cuts[j] : 0.0f;
                }
                copyAt(out.game, m.game, i, j);
                copyAt(out.gameRiseDb, m.gameRiseDb, i, j);
                copyAt(out.mic, m.mic, i, j);
                copyAt(out.voiceRiseDb, m.voiceRiseDb, i, j);
                copyAt(out.pitchRise, m.pitchRise, i, j);
                copyAt(out.laugh, m.laugh, i, j);
                copyAt(out.chat, m.chat, i, j);
                copyAt(out.chatRatio, m.chatRatio, i, j);
                copyAt(out.chatCount, m.chatCount, i, j);
            }
            // Keyword hits and hints where the clip shows them.
            const Ticks sourceFrom = std::min(clip.sourceTimeAt(clip.start()), clip.sourceTimeAt(clip.end() - Ticks{1}));
            const Ticks sourceTo = std::max(clip.sourceTimeAt(clip.start()), clip.sourceTimeAt(clip.end() - Ticks{1}));
            for (const KeywordHit& k : m.keywords) {
                const Ticks at = featureTime(k.index);
                if (at < sourceFrom || at > sourceTo) {
                    continue;
                }
                if (const auto where = analysis::timelineTimeOfSource(clip, at)) {
                    const std::size_t i = featureIndex(*where);
                    if (i < n && keywordsSeen.insert({i, k.word}).second) {
                        out.keywords.push_back({i, k.word});
                    }
                }
            }
            for (const Hint& h : m.hints) {
                if (h.at < sourceFrom || h.at > sourceTo) {
                    continue;
                }
                if (const auto where = analysis::timelineTimeOfSource(clip, h.at)) {
                    if (hintsSeen.insert({where->value, h.label}).second) {
                        out.hints.push_back({*where, h.label, h.source});
                    }
                }
            }
        }
    }
    std::stable_sort(out.keywords.begin(), out.keywords.end(), [](const KeywordHit& a, const KeywordHit& b) { return a.index < b.index; });
    std::stable_sort(out.hints.begin(), out.hints.end(), [](const Hint& a, const Hint& b) { return a.at < b.at; });
    if (warnings) {
        warnings->insert(warnings->end(), notes.begin(), notes.end());
    }
    return out;
}

// --- Moments -------------------------------------------------------------------------------------------------------

namespace {

struct Kind {
    const char* name;
    const std::vector<float>* activation;
    double weight;
};

/// The reasons and title for a moment peaking at sample `p`, with each signal's activation there.
void explain(const MediaSignals& s, std::size_t p, Moment& m, const std::vector<Kind>& kinds, double hintWeight) {
    const std::size_t from = p > 15 ? p - 15 : 0;
    const std::size_t to = std::min(s.length, p + 16);
    const auto peakOf = [&](const std::vector<float>& v) {
        float best = 0.0f;
        for (std::size_t i = from; i < std::min(to, v.size()); ++i) {
            best = std::max(best, v[i]);
        }
        return best;
    };
    struct Reason {
        double weight;
        std::string text;
        std::string title;
    };
    std::vector<Reason> reasons;
    for (const Kind& k : kinds) {
        const double a = peakOf(*k.activation);
        m.levels[k.name] = std::round(a * 1000.0) / 1000.0;
        if (a < 0.3) {
            continue;
        }
        const double w = a * k.weight;
        const std::string name = k.name;
        if (name == "game") {
            reasons.push_back({w, "game audio +" + fixed(peakOf(s.gameRiseDb), 0) + " dB over the moments before", "Game audio spike"});
        } else if (name == "mic") {
            const double laugh = peakOf(s.laugh);
            const double rise = peakOf(s.voiceRiseDb);
            const double pitch = peakOf(s.pitchRise);
            if (rise >= 6.0 && pitch >= 3.0) {
                reasons.push_back({w, "you shouted (+" + fixed(rise, 0) + " dB, pitch +" + fixed(pitch, 0) + " semitones)", "You shouted"});
            } else if (laugh >= 0.4) {
                reasons.push_back({w, "you laughed", "You laughed"});
            } else if (rise >= 5.0) {
                reasons.push_back({w, "your voice +" + fixed(rise, 0) + " dB louder than usual", "You got loud"});
            } else {
                reasons.push_back({w, "your voice rose " + fixed(pitch, 0) + " semitones", "Excited voice"});
            }
        } else if (name == "visual") {
            int flashes = 0;
            bool inFlash = false;
            for (std::size_t i = from; i < std::min(to, s.flash.size()); ++i) {
                const bool on = s.flash[i] >= 0.4f;
                flashes += on && !inFlash ? 1 : 0;
                inFlash = on;
            }
            const double motion = peakOf(s.motionRatio);
            const double cuts = peakOf(s.cuts);
            std::vector<std::string> parts;
            if (flashes > 0) {
                parts.push_back(std::to_string(flashes) + (flashes == 1 ? " flash" : " flashes"));
            }
            if (motion >= 1.8) {
                parts.push_back("fast action (" + fixed(motion, 1) + "x the usual motion)");
            }
            if (cuts >= 3.0) {
                parts.push_back(fixed(cuts, 0) + " picture changes in 5 s");
            }
            if (parts.empty()) {
                parts.push_back("the picture got busy");
            }
            std::string text = parts.front();
            for (std::size_t i = 1; i < parts.size(); ++i) {
                text += ", " + parts[i];
            }
            reasons.push_back({w, text, flashes > 0 ? "Flashes" : "Fast action"});
        } else if (name == "chat") {
            std::map<std::string, int> counts;
            for (const KeywordHit& hit : s.keywords) {
                if (hit.index + 30 >= p && hit.index <= p + 30) {
                    ++counts[hit.word];
                }
            }
            std::vector<std::pair<std::string, int>> top(counts.begin(), counts.end());
            std::sort(top.begin(), top.end(), [](const auto& lhs, const auto& rhs) { return lhs.second != rhs.second ? lhs.second > rhs.second : lhs.first < rhs.first; });
            std::string text;
            const double ratio = peakOf(s.chatRatio);
            if (ratio >= 2.0) {
                text = "chat " + fixed(ratio, 0) + "x busier (" + fixed(peakOf(s.chatCount), 0) + " messages in 5 s)";
            }
            for (std::size_t i = 0; i < std::min<std::size_t>(2, top.size()); ++i) {
                text += (text.empty() ? "" : ", ") + std::string("'") + top[i].first + "' x" + std::to_string(top[i].second);
            }
            if (text.empty()) {
                text = "chat reacted";
            }
            reasons.push_back({w, text, "Chat reaction"});
        }
    }
    // Hints: a marker set during or just after the moment (a hotkey is pressed after the play).
    for (const Hint& h : s.hints) {
        const std::size_t at = featureIndex(h.at);
        if (at + 20 >= p && at <= p + 150) {
            const std::string what = h.source == "chapter" ? "OBS chapter marker" : "marker";
            reasons.push_back({hintWeight + 1.0, what + (h.label.empty() ? "" : " '" + h.label + "'"),
                               h.label.empty() ? "Marked" : "Marked: " + h.label});
            m.levels["hint"] = 1.0;
            break;
        }
    }
    std::stable_sort(reasons.begin(), reasons.end(), [](const Reason& a, const Reason& b) { return a.weight > b.weight; });
    for (const Reason& r : reasons) {
        m.reasons.push_back(r.text);
    }
    if (!reasons.empty()) {
        m.title = reasons[0].title;
        if (reasons.size() > 1 && reasons[1].title != reasons[0].title) {
            m.title += " + " + reasons[1].title;
        }
    } else {
        m.title = "Highlight";
    }
}

} // namespace

std::vector<Moment> findMoments(const MediaSignals& s, Ticks duration, const FindOptions& options) {
    std::vector<Moment> moments;
    const std::size_t n = std::min(s.length, featureIndex(duration) + 1);
    if (n == 0 || options.maxMoments == 0) {
        return moments;
    }
    std::vector<Kind> kinds;
    if (!s.game.empty()) {
        kinds.push_back({"game", &s.game, options.weightGame});
    }
    if (!s.mic.empty()) {
        kinds.push_back({"mic", &s.mic, options.weightMic});
    }
    if (!s.visual.empty()) {
        kinds.push_back({"visual", &s.visual, options.weightVisual});
    }
    if (!s.chat.empty()) {
        kinds.push_back({"chat", &s.chat, options.weightChat});
    }
    double totalWeight = 0.0;
    for (const Kind& k : kinds) {
        totalWeight += k.weight;
    }
    constexpr double kHintWeight = 0.3;
    // A hint lifts the 15 s before it (the play) and the second after it.
    std::vector<float> hint(n, 0.0f);
    for (const Hint& h : s.hints) {
        const auto at = static_cast<std::ptrdiff_t>(featureIndex(h.at));
        for (std::ptrdiff_t i = at - 150; i <= at + 10; ++i) {
            if (i < 0 || i >= static_cast<std::ptrdiff_t>(n)) {
                continue;
            }
            const std::ptrdiff_t before = at - i;
            const float v = before < 0 ? static_cast<float>(1.0 + static_cast<double>(before) / 10.0)
                            : before <= 80 ? 1.0f
                                           : static_cast<float>(1.0 - static_cast<double>(before - 80) / 70.0);
            hint[static_cast<std::size_t>(i)] = std::max(hint[static_cast<std::size_t>(i)], v);
        }
    }
    std::vector<float> fused(n, 0.0f);
    for (std::size_t i = 0; i < n; ++i) {
        double sum = 0.0;
        int agree = 0;
        for (const Kind& k : kinds) {
            const double a = i < k.activation->size() ? (*k.activation)[i] : 0.0;
            sum += a * k.weight;
            agree += a >= 0.35 ? 1 : 0;
        }
        double v = totalWeight > 0.0 ? sum / totalWeight : 0.0;
        if (agree >= 2) {
            v += 0.15 * (agree - 1);
        }
        v += kHintWeight * hint[i];
        fused[i] = static_cast<float>(v);
    }
    // 1.5 s moving average: one moment, not one per word or explosion.
    const std::vector<float> summed = windowSum(fused, 7);
    std::vector<float> smoothed(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t from = i > 7 ? i - 7 : 0;
        const std::size_t to = std::min(n, i + 8);
        smoothed[i] = summed[i] / static_cast<float>(to - from);
    }
    std::vector<std::size_t> peaks;
    for (std::size_t i = 0; i < n; ++i) {
        if (smoothed[i] < options.minScore) {
            continue;
        }
        const std::size_t from = i > 5 ? i - 5 : 0;
        const std::size_t to = std::min(n, i + 6);
        bool top = true;
        for (std::size_t j = from; j < to && top; ++j) {
            top = smoothed[j] < smoothed[i] || (smoothed[j] == smoothed[i] && j >= i);
        }
        if (top) {
            peaks.push_back(i);
        }
    }
    std::stable_sort(peaks.begin(), peaks.end(), [&](std::size_t a, std::size_t b) { return smoothed[a] > smoothed[b]; });
    const auto gap = static_cast<std::size_t>(std::max(0.0, options.minGapSeconds) * kFeatureRate);
    std::vector<std::size_t> chosen;
    for (std::size_t p : peaks) {
        const bool far = std::all_of(chosen.begin(), chosen.end(), [&](std::size_t q) { return (p > q ? p - q : q - p) >= gap; });
        if (far) {
            chosen.push_back(p);
            if (chosen.size() >= options.maxMoments) {
                break;
            }
        }
    }
    const auto samplesOf = [](double secondsValue) { return static_cast<std::size_t>(std::max(0.0, secondsValue) * kFeatureRate); };
    for (std::size_t p : chosen) {
        // The exact peak: the strongest unsmoothed sample within a second.
        std::size_t peak = p;
        for (std::size_t j = p > 10 ? p - 10 : 0; j < std::min(n, p + 11); ++j) {
            if (fused[j] > fused[peak]) {
                peak = j;
            }
        }
        const float threshold = 0.35f * smoothed[p];
        std::size_t start = peak;
        while (start > 0 && smoothed[start - 1] >= threshold && peak - start < samplesOf(options.maxLeadSeconds)) {
            --start;
        }
        start = std::min(start, peak > samplesOf(options.minLeadSeconds) ? peak - samplesOf(options.minLeadSeconds) : 0);
        std::size_t end = peak;
        while (end + 1 < n && smoothed[end + 1] >= threshold && end - peak < samplesOf(options.maxTailSeconds)) {
            ++end;
        }
        end = std::max(end, std::min(n - 1, peak + samplesOf(options.minTailSeconds)));
        Moment m;
        const Ticks a = featureTime(start);
        const Ticks b = std::min(duration, featureTime(end + 1));
        m.range = TimeRange::fromStartEnd(a, std::max(b, a + Ticks{1}));
        m.peak = std::clamp(featureTime(peak) + Ticks{kTicksPerSecond / (2 * kFeatureRate)}, m.range.start, m.range.end() - Ticks{1});
        m.score = std::clamp(static_cast<double>(smoothed[p]), 0.0, 1.0);
        explain(s, peak, m, kinds, kHintWeight);
        int agree = 0;
        for (const auto& [name, value] : m.levels) {
            agree += value >= 0.35 ? 1 : 0;
        }
        m.confidence = std::clamp(0.25 + 0.2 * agree + 0.25 * m.score - (kinds.size() <= 1 && s.hints.empty() ? 0.1 : 0.0), 0.0, 1.0);
        m.score = std::round(m.score * 1000.0) / 1000.0;
        m.confidence = std::round(m.confidence * 1000.0) / 1000.0;
        moments.push_back(std::move(m));
    }
    // Ranges never overlap: two moments closer than their lead-ups split the time between their peaks.
    std::vector<std::size_t> byTime(moments.size());
    std::iota(byTime.begin(), byTime.end(), std::size_t{0});
    std::sort(byTime.begin(), byTime.end(), [&](std::size_t a, std::size_t b) { return moments[a].peak < moments[b].peak; });
    for (std::size_t k = 1; k < byTime.size(); ++k) {
        Moment& earlier = moments[byTime[k - 1]];
        Moment& later = moments[byTime[k]];
        if (earlier.range.end() > later.range.start) {
            const Ticks middle{(earlier.peak.value + later.peak.value) / 2};
            earlier.range = TimeRange::fromStartEnd(earlier.range.start, std::max(middle, earlier.peak + Ticks{1}));
            later.range = TimeRange::fromStartEnd(std::min(middle, later.peak), later.range.end());
        }
    }
    return moments;
}

std::string_view confidenceLabel(double confidence) {
    return confidence >= 0.75 ? "high" : confidence >= 0.5 ? "medium" : "low";
}

Accuracy measure(const std::vector<Moment>& moments, const std::vector<Ticks>& events, double toleranceSeconds) {
    Accuracy a;
    std::vector<bool> matched(events.size(), false);
    const Ticks tolerance = ticksOf(toleranceSeconds);
    for (const Moment& m : moments) {
        bool hit = false;
        for (std::size_t e = 0; e < events.size() && !hit; ++e) {
            if (matched[e]) {
                continue;
            }
            const bool inside = events[e] >= m.range.start && events[e] < m.range.end();
            const Ticks distance = events[e] > m.peak ? events[e] - m.peak : m.peak - events[e];
            if (inside || distance <= tolerance) {
                matched[e] = true;
                hit = true;
            }
        }
        if (hit) {
            ++a.truePositives;
        } else {
            ++a.falsePositives;
        }
    }
    a.falseNegatives = static_cast<std::size_t>(std::count(matched.begin(), matched.end(), false));
    const std::size_t found = a.truePositives + a.falsePositives;
    a.precision = found > 0 ? static_cast<double>(a.truePositives) / static_cast<double>(found) : 0.0;
    a.recall = events.empty() ? 0.0 : static_cast<double>(a.truePositives) / static_cast<double>(events.size());
    return a;
}

// --- JSON ----------------------------------------------------------------------------------------------------------

namespace {

json rounded(const std::vector<float>& v, double scale) {
    json out = json::array();
    for (float x : v) {
        const double r = std::round(static_cast<double>(x) * scale) / scale;
        if (r == std::floor(r) && std::abs(r) < 1e15) {
            out.push_back(static_cast<std::int64_t>(r));
        } else {
            out.push_back(r);
        }
    }
    return out;
}

std::expected<std::vector<float>, Error> floats(const json& j, const char* key, std::size_t n) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != n) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, std::string("highlight features: bad '") + key + "'"));
    }
    std::vector<float> out;
    out.reserve(n);
    for (const json& v : *it) {
        if (!v.is_number()) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, std::string("highlight features: bad '") + key + "'"));
        }
        out.push_back(v.get<float>());
    }
    return out;
}

std::expected<json, Error> header(std::string_view text, std::string_view schema) {
    json j = json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("schema", std::string()) != schema || j.value("version", 0) != kVersion ||
        !j.contains("samples") || !j["samples"].is_number_unsigned()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "not " + std::string(schema) + " version " + std::to_string(kVersion)));
    }
    return j;
}

} // namespace

std::string toJson(const AudioFeatures& f) {
    return json{{"schema", "wabcut.highlight-audio"},
                {"version", kVersion},
                {"rate", kFeatureRate},
                {"samples", f.size()},
                {"levelDb", rounded(f.levelDb, 100.0)},
                {"voiced", rounded(f.voiced, 1000.0)},
                {"pitchHz", rounded(f.pitchHz, 10.0)},
                {"burst", rounded(f.burst, 1000.0)}}
        .dump();
}

std::string toJson(const VisualFeatures& f) {
    return json{{"schema", "wabcut.highlight-video"},
                {"version", kVersion},
                {"rate", kFeatureRate},
                {"samples", f.size()},
                {"motion", rounded(f.motion, 10000.0)},
                {"luma", rounded(f.luma, 1000.0)},
                {"flash", rounded(f.flash, 1000.0)},
                {"cut", rounded(f.cut, 1.0)}}
        .dump();
}

std::expected<AudioFeatures, Error> audioFeaturesFromJson(std::string_view text) {
    auto j = header(text, "wabcut.highlight-audio");
    if (!j) {
        return std::unexpected(j.error());
    }
    const auto n = (*j)["samples"].get<std::size_t>();
    AudioFeatures f;
    for (auto [key, into] : {std::pair{"levelDb", &f.levelDb}, std::pair{"voiced", &f.voiced}, std::pair{"pitchHz", &f.pitchHz},
                             std::pair{"burst", &f.burst}}) {
        auto v = floats(*j, key, n);
        if (!v) {
            return std::unexpected(v.error());
        }
        *into = std::move(*v);
    }
    return f;
}

std::expected<VisualFeatures, Error> visualFeaturesFromJson(std::string_view text) {
    auto j = header(text, "wabcut.highlight-video");
    if (!j) {
        return std::unexpected(j.error());
    }
    const auto n = (*j)["samples"].get<std::size_t>();
    VisualFeatures f;
    for (auto [key, into] : {std::pair{"motion", &f.motion}, std::pair{"luma", &f.luma}, std::pair{"flash", &f.flash}, std::pair{"cut", &f.cut}}) {
        auto v = floats(*j, key, n);
        if (!v) {
            return std::unexpected(v.error());
        }
        *into = std::move(*v);
    }
    return f;
}

std::string toJson(const std::vector<Moment>& moments) {
    json list = json::array();
    for (const Moment& m : moments) {
        list.push_back({{"start", std::round(seconds(m.range.start) * 1000.0) / 1000.0},
                        {"end", std::round(seconds(m.range.end()) * 1000.0) / 1000.0},
                        {"peak", std::round(seconds(m.peak) * 1000.0) / 1000.0},
                        {"score", m.score},
                        {"confidence", m.confidence},
                        {"title", m.title},
                        {"reasons", m.reasons},
                        {"signals", m.levels}});
    }
    return json{{"schema", "wabcut.highlights"}, {"version", kVersion}, {"moments", std::move(list)}}.dump();
}

} // namespace wabcut::ai::highlights
