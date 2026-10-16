#include <wabcut/ai/shorts/Shorts.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <set>
#include <unordered_set>

namespace wabcut::ai::shorts {

namespace {

using Sparse = std::vector<std::pair<std::uint32_t, float>>;

double seconds(Ticks t) { return t.toSeconds(); }
Ticks fromSeconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

/// Decodes the next UTF-8 code point (invalid bytes pass as themselves).
char32_t nextCodePoint(std::string_view s, std::size_t& i) {
    const auto b = static_cast<unsigned char>(s[i]);
    const int extra = b < 0x80 ? 0 : (b >> 5) == 0x6 ? 1 : (b >> 4) == 0xE ? 2 : (b >> 3) == 0x1E ? 3 : 0;
    char32_t cp = extra == 0 ? b : extra == 1 ? (b & 0x1F) : extra == 2 ? (b & 0x0F) : (b & 0x07);
    ++i;
    for (int k = 0; k < extra && i < s.size(); ++k, ++i) {
        cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    }
    return cp;
}

bool isWordChar(char32_t cp) {
    if (cp < 0x80) {
        return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || cp == '\'';
    }
    // Latin letters with accents, Thai, CJK, ...: everything beyond ASCII that is not general punctuation.
    return !(cp >= 0x2000 && cp <= 0x206F) && !(cp >= 0x3000 && cp <= 0x303F) && cp != 0xFF1F && cp != 0xFF01 && cp != 0x0E2F;
}

std::string lowerAscii(std::string s) {
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return s;
}

const std::unordered_set<std::string>& stopWords() {
    static const std::unordered_set<std::string> words{
        "a", "an", "the", "and", "or", "but", "so", "to", "of", "in", "on", "at", "for", "with", "is", "are", "was", "were", "be",
        "been", "it", "its", "it's", "this", "that", "these", "those", "i", "you", "he", "she", "we", "they", "me", "him", "her",
        "us", "them", "my", "your", "our", "their", "do", "does", "did", "have", "has", "had", "not", "no", "yes", "just", "like",
        "um", "uh", "er", "ah", "oh", "okay", "ok", "yeah", "well", "really", "very", "can", "could", "would", "will", "there",
        "here", "what", "which", "who", "if", "as", "then", "than", "about", "from", "up", "out", "get", "got", "know", "think",
        "i'm", "don't", "that's", "we're", "you're", "gonna", "kind", "sort", "mean", "thing", "things",
        // Thai particles and function words
        "ครับ", "ค่ะ", "คะ", "นะ", "ก็", "ที่", "และ", "ว่า", "เป็น", "จะ", "ได้", "มี", "ไม่", "ให้", "ไป", "มา", "แล้ว", "กับ",
        "ของ", "ใน", "นี้", "นั้น", "คือ", "อยู่", "เรา", "ผม", "ฉัน", "เขา", "มัน", "อะ", "เอ่อ", "อืม", "แบบ", "ก็คือ"};
    return words;
}

const std::vector<std::string>& hookWords() {
    static const std::vector<std::string> words{
        "why", "how", "what", "imagine", "secret", "mistake", "never", "nobody", "everyone", "truth", "actually", "best", "worst",
        "biggest", "surprising", "crazy", "insane", "wrong", "here's", "listen", "stop", "money", "free", "first", "last",
        "ทำไม", "อย่างไร", "ยังไง", "อะไร", "ความลับ", "ไม่เคย", "ที่สุด", "จริงๆ", "เคล็ดลับ", "ผิด", "เงิน"};
    return words;
}

bool endsSentence(std::string_view word) {
    if (word.empty()) {
        return false;
    }
    const char last = word.back();
    if (last == '.' || last == '?' || last == '!') {
        return true;
    }
    // "。" "？" "！" (CJK) end in these UTF-8 bytes.
    return word.ends_with("\xE3\x80\x82") || word.ends_with("\xEF\xBC\x9F") || word.ends_with("\xEF\xBC\x81");
}

bool isQuestion(std::string_view text) { return text.find('?') != std::string_view::npos || text.find("\xEF\xBC\x9F") != std::string_view::npos; }

/// The tokens of one transcript word (lowercased, without punctuation; stop words dropped).
std::vector<std::string> wordTokens(std::string_view word) {
    std::vector<std::string> out;
    std::string current;
    for (std::size_t i = 0; i < word.size();) {
        const std::size_t at = i;
        const char32_t cp = nextCodePoint(word, i);
        if (isWordChar(cp) && !(cp == '\'' && current.empty())) {
            current.append(word.substr(at, i - at));
        } else if (!current.empty()) {
            out.push_back(lowerAscii(std::move(current)));
            current.clear();
        }
    }
    if (!current.empty()) {
        out.push_back(lowerAscii(std::move(current)));
    }
    std::erase_if(out, [](const std::string& t) { return stopWords().contains(t) || t == "'"; });
    return out;
}

Sparse addSparse(const Sparse& a, const Sparse& b) {
    Sparse out;
    out.reserve(a.size() + b.size());
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() || j < b.size()) {
        if (j == b.size() || (i < a.size() && a[i].first < b[j].first)) {
            out.push_back(a[i++]);
        } else if (i == a.size() || b[j].first < a[i].first) {
            out.push_back(b[j++]);
        } else {
            out.emplace_back(a[i].first, a[i].second + b[j].second);
            ++i;
            ++j;
        }
    }
    return out;
}

/// Smoothly 1 at the target, ~0.5 at the limits.
double lengthScore(double d, const ShortsOptions& o) {
    const double spread = d < o.targetSeconds ? std::max(o.targetSeconds - o.minSeconds, 1.0) : std::max(o.maxSeconds - o.targetSeconds, 1.0);
    const double x = (d - o.targetSeconds) / spread;
    return std::exp(-0.7 * x * x);
}

/// The timeline segments of the clips on video tracks that show [range): (clip, the part of the range it covers).
std::vector<std::pair<const Clip*, TimeRange>> videoUnder(const Sequence& seq, TimeRange range) {
    std::vector<std::pair<const Clip*, TimeRange>> out;
    for (auto t = seq.videoTracks.rbegin(); t != seq.videoTracks.rend(); ++t) { // top first
        if (!(*t)->enabled) {
            continue;
        }
        for (const Clip& clip : (*t)->clips) {
            if (!clip.enabled || clip.end() <= range.start || clip.start() >= range.end()) {
                continue;
            }
            const Ticks a = std::max(clip.start(), range.start);
            const Ticks b = std::min(clip.end(), range.end());
            out.emplace_back(&clip, TimeRange::fromStartEnd(a, b));
        }
        if (!out.empty()) {
            return out; // the top track with pictures there
        }
    }
    return out;
}

} // namespace

// --- Sentences -----------------------------------------------------------------------------------------------------

std::vector<Sentence> splitSentences(const edit::SequenceTranscript& transcript, Ticks pause, Ticks maxLength) {
    std::vector<Sentence> out;
    const std::size_t n = transcript.size();
    std::size_t first = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const edit::TimelineWord& w = transcript[i];
        const bool lastWord = i + 1 == n;
        bool end = lastWord || endsSentence(transcript.text(i));
        if (!end) {
            const edit::TimelineWord& next = transcript[i + 1];
            end = next.timeline.start - w.timeline.end() >= pause || next.speaker != w.speaker ||
                  next.timeline.start < w.timeline.start || // another clip's words: a new run
                  next.timeline.end() - transcript[first].timeline.start > maxLength;
        }
        if (end) {
            Sentence s;
            s.first = first;
            s.last = i;
            s.range = TimeRange::fromStartEnd(transcript[first].timeline.start, std::max(w.timeline.end(), transcript[first].timeline.start));
            s.text = transcript.textOf(first, i);
            s.speaker = transcript[first].speaker;
            out.push_back(std::move(s));
            first = i + 1;
        }
    }
    return out;
}

// --- Text vectors and topics ---------------------------------------------------------------------------------------

TextVectors::TextVectors(std::span<const std::string> documents) {
    std::vector<std::vector<std::string>> tokenized;
    for (const std::string& d : documents) {
        tokenized.push_back(tokens(d));
    }
    std::vector<std::uint32_t> df;
    for (const auto& doc : tokenized) {
        std::set<std::uint32_t> seen;
        for (const std::string& t : doc) {
            auto [it, inserted] = m_ids.try_emplace(t, static_cast<std::uint32_t>(m_ids.size()));
            if (inserted) {
                df.push_back(0);
            }
            if (seen.insert(it->second).second) {
                ++df[it->second];
            }
        }
    }
    const double count = static_cast<double>(std::max<std::size_t>(documents.size(), 1));
    m_idf.resize(df.size());
    for (std::size_t i = 0; i < df.size(); ++i) {
        m_idf[i] = std::log((1.0 + count) / (1.0 + df[i])) + 1.0;
    }
    for (const auto& doc : tokenized) {
        std::map<std::uint32_t, float> tf;
        for (const std::string& t : doc) {
            tf[m_ids.find(t)->second] += 1.0f;
        }
        Sparse v;
        double norm = 0.0;
        for (const auto& [id, f] : tf) {
            const double w = (1.0 + std::log(f)) * m_idf[id];
            v.emplace_back(id, static_cast<float>(w));
            norm += w * w;
        }
        norm = std::sqrt(norm);
        for (auto& [id, w] : v) {
            w = norm > 0.0 ? static_cast<float>(w / norm) : 0.0f;
        }
        m_documents.push_back(std::move(v));
    }
}

std::vector<std::string> TextVectors::tokens(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ' ' || text[i] == '\n' || text[i] == '\t') {
            if (i > start) {
                for (std::string& t : wordTokens(text.substr(start, i - start))) {
                    out.push_back(std::move(t));
                }
            }
            start = i + 1;
        }
    }
    return out;
}

Sparse TextVectors::vectorOf(std::string_view text) const {
    std::map<std::uint32_t, float> tf;
    for (const std::string& t : tokens(text)) {
        if (const auto it = m_ids.find(t); it != m_ids.end()) {
            tf[it->second] += 1.0f;
        }
    }
    Sparse v;
    double norm = 0.0;
    for (const auto& [id, f] : tf) {
        const double w = (1.0 + std::log(f)) * m_idf[id];
        v.emplace_back(id, static_cast<float>(w));
        norm += w * w;
    }
    norm = std::sqrt(norm);
    for (auto& [id, w] : v) {
        w = norm > 0.0 ? static_cast<float>(w / norm) : 0.0f;
    }
    return v;
}

double TextVectors::informativeness(std::string_view text) const {
    const auto t = tokens(text);
    double sum = 0.0;
    for (const std::string& token : t) {
        if (const auto it = m_ids.find(token); it != m_ids.end()) {
            sum += m_idf[it->second];
        }
    }
    return sum / std::sqrt(static_cast<double>(std::max<std::size_t>(t.size(), 1)) + 2.0);
}

float TextVectors::cosine(const Sparse& a, const Sparse& b) {
    double dot = 0.0;
    double na = 0.0;
    double nb = 0.0;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].first < b[j].first) {
            ++i;
        } else if (b[j].first < a[i].first) {
            ++j;
        } else {
            dot += static_cast<double>(a[i].second) * b[j].second;
            ++i;
            ++j;
        }
    }
    for (const auto& [id, w] : a) {
        na += static_cast<double>(w) * w;
    }
    for (const auto& [id, w] : b) {
        nb += static_cast<double>(w) * w;
    }
    return na > 0.0 && nb > 0.0 ? static_cast<float>(dot / std::sqrt(na * nb)) : 0.0f;
}

Topics segmentTopics(std::span<const Sparse> vectors, std::size_t window) {
    Topics t;
    const std::size_t n = vectors.size();
    t.cohesion.assign(n, 1.0);
    t.starts.assign(n, false);
    if (n == 0) {
        return t;
    }
    t.starts[0] = true;
    window = std::max<std::size_t>(window, 1);
    for (std::size_t gap = 1; gap < n; ++gap) {
        Sparse left;
        Sparse right;
        for (std::size_t k = gap >= window ? gap - window : 0; k < gap; ++k) {
            left = addSparse(left, vectors[k]);
        }
        for (std::size_t k = gap; k < std::min(n, gap + window); ++k) {
            right = addSparse(right, vectors[k]);
        }
        t.cohesion[gap] = TextVectors::cosine(left, right);
    }
    // Depth of each valley: how far cohesion climbs on both sides before it falls again.
    std::vector<double> depth(n, 0.0);
    for (std::size_t gap = 1; gap < n; ++gap) {
        double leftPeak = t.cohesion[gap];
        for (std::size_t k = gap; k-- > 1 && t.cohesion[k] >= leftPeak;) {
            leftPeak = t.cohesion[k];
        }
        double rightPeak = t.cohesion[gap];
        for (std::size_t k = gap + 1; k < n && t.cohesion[k] >= rightPeak; ++k) {
            rightPeak = t.cohesion[k];
        }
        depth[gap] = (leftPeak - t.cohesion[gap]) + (rightPeak - t.cohesion[gap]);
    }
    double mean = 0.0;
    double sq = 0.0;
    std::size_t count = 0;
    for (std::size_t gap = 1; gap < n; ++gap) {
        if (depth[gap] > 0.0) {
            mean += depth[gap];
            sq += depth[gap] * depth[gap];
            ++count;
        }
    }
    if (count == 0) {
        return t;
    }
    mean /= static_cast<double>(count);
    const double sigma = std::sqrt(std::max(sq / static_cast<double>(count) - mean * mean, 0.0));
    const double cutoff = std::max(mean - sigma / 2.0, 0.05);
    // The deepest valleys first, at least `window` sentences apart.
    std::vector<std::size_t> gaps;
    for (std::size_t gap = 1; gap < n; ++gap) {
        if (depth[gap] >= cutoff) {
            gaps.push_back(gap);
        }
    }
    std::ranges::stable_sort(gaps, [&](std::size_t a, std::size_t b) { return depth[a] > depth[b]; });
    std::vector<std::size_t> chosen;
    for (const std::size_t gap : gaps) {
        if (std::ranges::all_of(chosen, [&](std::size_t c) { return (gap > c ? gap - c : c - gap) >= window; })) {
            chosen.push_back(gap);
            t.starts[gap] = true;
        }
    }
    return t;
}

// --- Titles and cuts -------------------------------------------------------------------------------------------------

std::string titleFrom(std::string_view sentence, std::size_t maxChars) {
    std::vector<std::string> words;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= sentence.size(); ++i) {
        if (i == sentence.size() || sentence[i] == ' ') {
            if (i > start) {
                words.emplace_back(sentence.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    static const std::set<std::string> leading{"so", "and", "but", "um", "uh", "like", "well", "okay", "ok", "yeah", "anyway",
                                                "so,", "and,", "well,", "okay,", "yeah,", "um,", "uh,", "เอ่อ", "อืม", "แบบ", "ก็"};
    while (!words.empty() && leading.contains(lowerAscii(words.front()))) {
        words.erase(words.begin());
    }
    std::string out;
    for (const std::string& w : words) {
        if (out.size() + w.size() + 1 > maxChars && !out.empty()) {
            break;
        }
        out += (out.empty() ? "" : " ") + w;
    }
    if (out.size() > maxChars) { // one long unspaced run (Thai): cut at a code point
        std::size_t cut = 0;
        for (std::size_t i = 0; i < out.size() && i <= maxChars;) {
            cut = i;
            nextCodePoint(out, i);
        }
        out.resize(cut);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ',' || out.back() == ';' || out.back() == ':' || out.back() == ' ')) {
        out.pop_back();
    }
    if (!out.empty() && out.front() >= 'a' && out.front() <= 'z') {
        out.front() = static_cast<char>(out.front() - 'a' + 'A');
    }
    return out;
}

std::vector<Ticks> cutPoints(const ProjectState& state, SequenceId sequence, const ShortsSignals& inputs) {
    std::vector<Ticks> cuts;
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        return cuts;
    }
    for (const TrackPtr& track : seq->videoTracks) {
        for (const Clip& clip : track->clips) {
            cuts.push_back(clip.start());
            cuts.push_back(clip.end());
            if (!inputs.subjects || clip.isFreeze()) {
                continue;
            }
            const MediaItem* media = state.findMedia(clip.media);
            if (!media || media->info.video.empty() || clip.stream.kind != StreamKind::Video) {
                continue;
            }
            if (const vision::SubjectAnalysis* a = inputs.subjects(*media, clip.stream.index)) {
                for (const Ticks cut : a->cuts) {
                    if (cut > clip.sourceIn && cut < clip.sourceOut()) {
                        if (const auto t = clip.timelineTimeOfCurveTime(cut)) {
                            cuts.push_back(*t);
                        }
                    }
                }
            }
        }
    }
    std::ranges::sort(cuts);
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    return cuts;
}

// --- The finder ------------------------------------------------------------------------------------------------------

ShortsReport findShorts(const ProjectState& state, SequenceId sequence, const edit::SequenceTranscript& transcript,
                        std::span<const edit::ReviewItem> review, const ShortsSignals& inputs, const ShortsOptions& options) {
    const auto started = std::chrono::steady_clock::now();
    ShortsReport report;
    const Sequence* seq = state.findSequence(sequence);
    if (!seq || transcript.empty()) {
        report.warnings.push_back(seq ? "the sequence has no transcript: transcribe its media first" : "no such sequence");
        return report;
    }
    const std::vector<Sentence> sentences = splitSentences(transcript);
    const std::size_t s = sentences.size();
    report.sentences = s;

    // Sentence vectors: TF-IDF, or the host's text embedder.
    std::vector<std::string> texts;
    for (const Sentence& sentence : sentences) {
        texts.push_back(sentence.text);
    }
    const TextVectors tfidf(texts);
    std::vector<Sparse> vectors;
    for (std::size_t i = 0; i < s; ++i) {
        std::optional<std::vector<float>> dense = inputs.embed ? inputs.embed(texts[i]) : std::nullopt;
        if (dense) {
            Sparse v;
            for (std::size_t k = 0; k < dense->size(); ++k) {
                v.emplace_back(static_cast<std::uint32_t>(k), (*dense)[k]);
            }
            vectors.push_back(std::move(v));
        } else {
            vectors.push_back(tfidf.document(i));
        }
    }
    const Topics topics = segmentTopics(vectors);
    report.topics = static_cast<std::size_t>(std::ranges::count(topics.starts, true));

    // Per-sentence features, summed by prefix so a candidate costs O(1) (O(length) for the query's best match).
    std::vector<double> fillerSeconds(s, 0.0);
    {
        std::size_t at = 0;
        for (const edit::ReviewItem& item : review) {
            while (at < s && sentences[at].last < item.firstWord) {
                ++at;
            }
            if (at < s) {
                fillerSeconds[at] += item.kind == edit::ReviewKind::Filler ? 0.6 : seconds(item.range.duration);
            }
        }
    }
    std::vector<double> prefixFiller(s + 1, 0.0);
    std::vector<double> prefixCohesion(s + 1, 0.0);
    std::vector<int> prefixStarts(s + 1, 0);
    for (std::size_t i = 0; i < s; ++i) {
        prefixFiller[i + 1] = prefixFiller[i] + fillerSeconds[i];
        prefixCohesion[i + 1] = prefixCohesion[i] + topics.cohesion[i];
        prefixStarts[i + 1] = prefixStarts[i] + (topics.starts[i] ? 1 : 0);
    }
    std::vector<double> querySim(s, 0.0);
    const std::string query = lowerAscii(options.query);
    if (!query.empty()) {
        const Sparse q = tfidf.vectorOf(query);
        for (std::size_t i = 0; i < s; ++i) {
            querySim[i] = TextVectors::cosine(q, tfidf.document(i));
            if (lowerAscii(texts[i]).find(query) != std::string::npos) {
                querySim[i] = std::max(querySim[i], 0.9); // the phrase itself (also Thai, unsegmented)
            }
        }
    }
    // The hook: the first 3 seconds from each sentence's start.
    std::vector<double> hook(s, 0.0);
    std::vector<std::string> hookPhrase(s);
    for (std::size_t i = 0; i < s; ++i) {
        const Ticks until = sentences[i].range.start + Ticks{kTicksPerSecond * 3};
        std::size_t words = 0;
        int hooks = 0;
        Ticks lastEnd = sentences[i].range.start;
        for (std::size_t w = sentences[i].first; w < transcript.size() && transcript[w].timeline.start < until; ++w) {
            ++words;
            lastEnd = transcript[w].timeline.end();
            const std::string word = lowerAscii(std::string(transcript.text(w)));
            for (const std::string& h : hookWords()) {
                if (word.starts_with(h)) {
                    ++hooks;
                    break;
                }
            }
            if (std::ranges::any_of(word, [](char ch) { return ch >= '0' && ch <= '9'; })) {
                ++hooks; // numbers make a hook ("3 mistakes")
            }
        }
        const double span = std::max(seconds(lastEnd - sentences[i].range.start), 1.0);
        const double rate = static_cast<double>(words) / span; // words per second
        double h = 0.0;
        if (isQuestion(sentences[i].text)) {
            h += 0.45;
            hookPhrase[i] = "opens with a question";
        }
        h += std::min(0.3, 0.15 * hooks);
        if (hooks > 0 && hookPhrase[i].empty()) {
            hookPhrase[i] = "a hook in the first 3 s";
        }
        h += std::clamp((rate - 1.5) / 2.0, 0.0, 1.0) * 0.25;
        hook[i] = std::min(h, 1.0);
    }

    // Candidates: whole sentences, min..max seconds.
    struct Scored {
        std::size_t a = 0;
        std::size_t b = 0;
        double text = 0.0;
        ScoreParts parts;
    };
    std::vector<Scored> scored;
    const bool hasQuery = !query.empty();
    for (std::size_t a = 0; a < s; ++a) {
        for (std::size_t b = a; b < s; ++b) {
            const double d = seconds(sentences[b].range.end() - sentences[a].range.start);
            if (d > options.maxSeconds) {
                break;
            }
            if (d < options.minSeconds || sentences[b].range.end() <= sentences[a].range.start) {
                continue;
            }
            ScoreParts p;
            const bool startsTopic = topics.starts[a];
            const bool endsTopic = b + 1 == s || topics.starts[b + 1];
            p.structure = 0.4 + (startsTopic ? 0.3 : 0.0) + (endsTopic ? 0.3 : 0.0);
            p.hook = hook[a];
            const int inside = prefixStarts[b + 1] - prefixStarts[a + 1]; // topic starts after the first sentence
            const double cohesion = b > a ? (prefixCohesion[b + 1] - prefixCohesion[a + 1]) / static_cast<double>(b - a) : 1.0;
            p.topic = std::clamp(0.35 + 0.65 * cohesion * 2.0, 0.0, 1.0) * (inside == 0 ? 1.0 : 1.0 / (1.0 + inside));
            p.clean = std::clamp(1.0 - 3.0 * (prefixFiller[b + 1] - prefixFiller[a]) / d, 0.0, 1.0);
            p.length = lengthScore(d, options);
            if (hasQuery) {
                double best = 0.0;
                double sum = 0.0;
                for (std::size_t k = a; k <= b; ++k) {
                    best = std::max(best, querySim[k]);
                    sum += querySim[k];
                }
                p.query = std::clamp(0.7 * best + 0.3 * sum / static_cast<double>(b - a + 1), 0.0, 1.0);
            }
            Scored c{a, b, 0.0, p};
            c.text = 0.18 * p.structure + 0.24 * p.hook + 0.18 * p.topic + 0.12 * p.clean + 0.16 * p.length;
            if (hasQuery) {
                c.text = 0.35 * c.text + 0.65 * p.query;
            }
            scored.push_back(c);
        }
    }
    report.candidates = scored.size();
    if (scored.empty()) {
        report.warnings.push_back("no run of whole sentences lasts between " + std::to_string(static_cast<int>(options.minSeconds)) + " and " +
                                  std::to_string(static_cast<int>(options.maxSeconds)) + " seconds");
        return report;
    }
    std::ranges::stable_sort(scored, [&](const Scored& x, const Scored& y) {
        return x.text != y.text ? x.text > y.text : sentences[x.a].range.start < sentences[y.a].range.start;
    });

    // Refine the best ones with the media: energy (and bursts in pauses), faces, shot rhythm, motion.
    const std::size_t refine = std::min(scored.size(), std::max(options.refine, options.count * 4));
    std::optional<double> baseline;
    const auto energyOf = [&](TimeRange range) -> std::optional<double> {
        if (!inputs.energy) {
            return std::nullopt;
        }
        double sum = 0.0;
        double weight = 0.0;
        const auto [w0, w1] = transcript.wordsIn(range);
        std::set<std::uint64_t> clips;
        for (std::size_t w = w0; w < w1; ++w) {
            clips.insert(transcript[w].clip.value);
        }
        for (const std::uint64_t id : clips) {
            const auto at = seq->findClip(ClipId{id});
            if (!at) {
                continue;
            }
            const Clip& clip = at->clip();
            const MediaItem* media = state.findMedia(clip.media);
            const Ticks a = std::max(clip.start(), range.start);
            const Ticks b = std::min(clip.end(), range.end());
            if (!media || b <= a || clip.isFreeze()) {
                continue;
            }
            const Ticks sa = clip.sourceTimeAt(a);
            const Ticks sb = clip.sourceTimeAt(b - Ticks{1});
            const TimeRange source = TimeRange::fromStartEnd(std::min(sa, sb), std::max(sa, sb) + Ticks{1});
            if (const auto db = inputs.energy->rmsDb(*media, clip.stream.index, source)) {
                const double wgt = seconds(b - a);
                sum += std::pow(10.0, *db / 10.0) * wgt; // average the power, not the decibels
                weight += wgt;
            }
        }
        if (weight <= 0.0) {
            return std::nullopt;
        }
        return 10.0 * std::log10(std::max(sum / weight, 1e-12));
    };
    if (inputs.energy) {
        // The sequence's typical level: a few windows spread over the transcript.
        const auto extent = transcript.extent();
        double sum = 0.0;
        int n = 0;
        for (int k = 0; extent && k < 16; ++k) {
            const Ticks at = extent->start + Ticks{static_cast<std::int64_t>(static_cast<double>(extent->duration.value) * (k + 0.5) / 16.0)};
            if (const auto db = energyOf(TimeRange{at, Ticks{kTicksPerSecond * 4}})) {
                sum += std::pow(10.0, *db / 10.0);
                ++n;
            }
        }
        if (n > 0) {
            baseline = 10.0 * std::log10(std::max(sum / n, 1e-12));
        }
    }
    std::vector<std::vector<std::string>> reasons(refine);
    for (std::size_t r = 0; r < refine; ++r) {
        Scored& c = scored[r];
        const TimeRange range = TimeRange::fromStartEnd(sentences[c.a].range.start, sentences[c.b].range.end());
        if (baseline) {
            if (const auto db = energyOf(range)) {
                c.parts.energy = std::clamp(0.5 + (*db - *baseline) / 12.0, 0.0, 1.0);
                if (*db - *baseline >= 2.0) {
                    reasons[r].push_back("lively delivery");
                }
            }
            // Laughter / applause: loud pauses between words.
            int bursts = 0;
            for (std::size_t w = sentences[c.a].first; w < sentences[c.b].last && bursts < 4; ++w) {
                const Ticks gapStart = transcript[w].timeline.end();
                const Ticks gapEnd = transcript[w + 1].timeline.start;
                if (gapEnd - gapStart >= Ticks{kTicksPerSecond / 2}) {
                    if (const auto db = energyOf(TimeRange::fromStartEnd(gapStart, gapEnd)); db && *db > *baseline - 3.0) {
                        ++bursts;
                    }
                }
            }
            if (bursts > 0) {
                c.parts.energy = std::min(1.0, c.parts.energy + 0.1 * bursts);
                reasons[r].push_back("laughter or applause");
            }
        }
        if (inputs.subjects) {
            std::size_t samples = 0;
            std::size_t withFaces = 0;
            double motion = 0.0;
            int cuts = 0;
            bool known = false;
            for (const auto& [clip, part] : videoUnder(*seq, range)) {
                cuts += part.start > range.start ? 1 : 0;
                const MediaItem* media = state.findMedia(clip->media);
                if (!media || media->info.video.empty() || clip->stream.kind != StreamKind::Video) {
                    continue;
                }
                const vision::SubjectAnalysis* a = inputs.subjects(*media, clip->stream.index);
                if (!a) {
                    continue;
                }
                known = true;
                const Ticks sa = clip->sourceTimeAt(part.start);
                const Ticks sb = clip->sourceTimeAt(part.end() - Ticks{1});
                const Ticks lo = std::min(sa, sb);
                const Ticks hi = std::max(sa, sb);
                for (const vision::SubjectSample& sample : a->samples) {
                    if (sample.time >= lo && sample.time <= hi) {
                        ++samples;
                        withFaces += sample.faces.empty() ? 0 : 1;
                        motion += sample.motion;
                    }
                }
                for (const Ticks cut : a->cuts) {
                    cuts += cut > lo && cut < hi ? 1 : 0;
                }
            }
            if (known && samples > 0) {
                const double faces = static_cast<double>(withFaces) / static_cast<double>(samples);
                const double perMinute = cuts / std::max(seconds(range.duration) / 60.0, 1e-3);
                c.parts.visual = std::clamp(0.5 * faces + 0.25 * std::min(motion / static_cast<double>(samples) / 0.05, 1.0) +
                                                0.25 * std::min(perMinute / 10.0, 1.0),
                                            0.0, 1.0);
                if (faces >= 0.7) {
                    reasons[r].push_back("faces on screen");
                }
                if (perMinute >= 6.0) {
                    reasons[r].push_back("lively cutting");
                }
            }
        }
    }

    // Final scores; the best non-overlapping ones.
    std::vector<std::pair<double, std::size_t>> order;
    for (std::size_t r = 0; r < scored.size(); ++r) {
        const ScoreParts& p = scored[r].parts;
        double v = 0.15 * p.structure + 0.2 * p.hook + 0.15 * p.topic + 0.15 * p.energy + 0.1 * p.visual + 0.1 * p.clean + 0.15 * p.length;
        if (hasQuery) {
            v = 0.35 * v + 0.65 * p.query;
        }
        order.emplace_back(r < refine ? v : v - 1.0, r); // unrefined ones only after every refined one
    }
    std::ranges::stable_sort(order, [&](const auto& x, const auto& y) {
        return x.first != y.first ? x.first > y.first : sentences[scored[x.second].a].range.start < sentences[scored[y.second].a].range.start;
    });
    const std::vector<Ticks> cuts = cutPoints(state, sequence, inputs);
    const Ticks frame{seq->frameRate.ticksPerUnit()};
    const Ticks sequenceEnd = std::max(seq->duration, seq->contentEnd());
    for (const auto& [value, r] : order) {
        if (report.shorts.size() >= options.count) {
            break;
        }
        const Scored& c = scored[r];
        const Ticks wordStart = sentences[c.a].range.start;
        const Ticks wordEnd = sentences[c.b].range.end();
        const bool overlaps = std::ranges::any_of(report.shorts, [&](const ShortCandidate& o) {
            const Ticks a = std::max(o.range.start, wordStart);
            const Ticks b = std::min(o.range.end(), wordEnd);
            const Ticks shorter = std::min(o.range.duration, wordEnd - wordStart);
            return b > a && seconds(b - a) > options.maxOverlap * seconds(shorter);
        });
        if (overlaps) {
            continue;
        }
        // Snap outwards: a cut within `snap` that does not take in a neighbouring word, else padding.
        const Ticks before = sentences[c.a].first > 0 ? transcript[sentences[c.a].first - 1].timeline.end() : Ticks{0};
        const Ticks after = sentences[c.b].last + 1 < transcript.size() ? transcript[sentences[c.b].last + 1].timeline.start : sequenceEnd;
        const Ticks snap = fromSeconds(options.snap);
        const Ticks pad = fromSeconds(options.padding);
        Ticks start = std::max(wordStart - pad, before + Ticks{(wordStart - before).value / 2});
        Ticks end = std::min(wordEnd + pad, wordEnd + Ticks{(after - wordEnd).value / 2});
        bool snappedStart = false;
        bool snappedEnd = false;
        for (auto it = std::ranges::upper_bound(cuts, wordStart); it != cuts.begin();) {
            --it;
            if (*it < std::max(before, wordStart - snap)) {
                break;
            }
            start = *it;
            snappedStart = true;
            break;
        }
        for (auto it = std::ranges::lower_bound(cuts, wordEnd); it != cuts.end(); ++it) {
            if (*it > std::min(after, wordEnd + snap)) {
                break;
            }
            end = *it;
            snappedEnd = true;
            break;
        }
        if (!snappedStart && frame.value > 0) {
            start = Ticks{floorDiv(start.value, frame.value) * frame.value};
        }
        if (!snappedEnd && frame.value > 0) {
            end = Ticks{(floorDiv(end.value - 1, frame.value) + 1) * frame.value};
        }
        start = std::max(start, Ticks{0});
        end = std::min(std::max(end, start + frame), std::max(sequenceEnd, wordEnd));

        ShortCandidate out;
        out.range = TimeRange::fromStartEnd(start, end);
        out.firstSentence = c.a;
        out.lastSentence = c.b;
        out.firstWord = sentences[c.a].first;
        out.lastWord = sentences[c.b].last;
        out.parts = c.parts;
        out.score = std::clamp(r < refine ? value : value + 1.0, 0.0, 1.0);
        out.hook = sentences[c.a].text;
        out.text = transcript.textOf(out.firstWord, out.lastWord);
        if (out.text.size() > 600) {
            std::size_t cut = 600;
            while (cut > 0 && (static_cast<unsigned char>(out.text[cut]) & 0xC0) == 0x80) {
                --cut;
            }
            out.text = out.text.substr(0, cut) + "…";
        }
        // The title: the sentence that matches the query, else a question or the most telling one near the start.
        std::size_t best = c.a;
        double bestValue = -1.0;
        for (std::size_t k = c.a; k <= c.b && k < c.a + 6; ++k) {
            const double v = hasQuery ? querySim[k] : tfidf.informativeness(texts[k]) * (isQuestion(texts[k]) ? 1.5 : 1.0) / (1.0 + 0.1 * static_cast<double>(k - c.a));
            if (v > bestValue) {
                bestValue = v;
                best = k;
            }
        }
        out.title = titleFrom(texts[best]);
        if (!hookPhrase[c.a].empty() && c.parts.hook >= 0.4) {
            out.reasons.push_back(hookPhrase[c.a]);
        }
        if (c.parts.structure >= 0.99) {
            out.reasons.push_back("a complete topic");
        } else if (c.parts.structure >= 0.69) {
            out.reasons.push_back(topics.starts[c.a] ? "starts a topic" : "ends a topic");
        }
        if (r < refine) {
            out.reasons.insert(out.reasons.end(), reasons[r].begin(), reasons[r].end());
        }
        if (c.parts.clean >= 0.9) {
            out.reasons.push_back("few fillers or pauses");
        }
        if (hasQuery && c.parts.query >= 0.4) {
            out.reasons.push_back("about \"" + options.query + "\"");
        }
        report.shorts.push_back(std::move(out));
    }
    report.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return report;
}

} // namespace wabcut::ai::shorts
