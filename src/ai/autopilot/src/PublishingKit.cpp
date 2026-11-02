// Creator autopilot: the publishing kit (Autopilot.h) - title ideas, a description, chapters, tags and thumbnail
// candidates, made on this computer; and the optional cloud copy's request and answer.

#include <wabcut/ai/autopilot/Autopilot.h>

#include <wabcut/ai/shorts/Shorts.h>
#include <wabcut/ai/workflow/Podcast.h>
#include <wabcut/model/Resolve.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace wabcut::ai::autopilot {

namespace {

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

std::size_t wordCount(std::string_view text) {
    std::size_t n = 0;
    bool in = false;
    for (const char c : text) {
        const bool space = c == ' ' || c == '\n' || c == '\t';
        n += !space && !in ? 1 : 0;
        in = !space;
    }
    return n;
}

bool looksDefault(std::string_view name) {
    std::string lower;
    for (const char c : name) {
        lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    for (const std::string_view prefix : {"sequence", "untitled", "timeline", "rough cut", "short", "highlights"}) {
        if (lower.starts_with(prefix)) {
            return true;
        }
    }
    return lower.size() < 4;
}

std::string trimmed(std::string s, std::size_t max) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\n')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) {
        s.pop_back();
    }
    if (s.size() > max) {
        s.resize(max);
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
            s.pop_back();
        }
        if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0x80) != 0) {
            s.pop_back();
        }
    }
    return s;
}

/// The media the top video track shows at `t` and its source time.
std::optional<std::pair<const MediaItem*, Ticks>> pictureAt(const ProjectState& state, SequenceId sequence, Ticks t) {
    const std::vector<ResolvedLeaf> leaves = resolveSequenceLeaves(state, sequence, StreamKind::Video, t);
    for (auto it = leaves.rbegin(); it != leaves.rend(); ++it) {
        if (!it->clip || !it->clip->media.isValid()) {
            continue;
        }
        const MediaItem* media = state.findMedia(it->clip->media);
        if (media && media->info.hasVideo() && !media->isGenerator()) {
            return std::pair{media, it->sourceTime};
        }
    }
    return std::nullopt;
}

/// `telling`: the sentences that say most, best first (a short video's tags come from them when little repeats).
std::vector<std::string> tagsOf(const std::vector<shorts::Sentence>& sentences, const std::vector<std::string>& telling) {
    std::map<std::string, std::size_t, std::less<>> counts;
    std::map<std::string, std::size_t, std::less<>> documents;
    std::map<std::string, std::size_t, std::less<>> pairs;
    for (const shorts::Sentence& s : sentences) {
        const std::vector<std::string> words = shorts::TextVectors::tokens(s.text);
        std::set<std::string> unique;
        for (std::size_t i = 0; i < words.size(); ++i) {
            const std::string& w = words[i];
            if (w.size() < 3 || std::ranges::all_of(w, [](char c) { return c >= '0' && c <= '9'; })) {
                continue;
            }
            ++counts[w];
            unique.insert(w);
            if (i + 1 < words.size() && words[i + 1].size() >= 3) {
                ++pairs[w + " " + words[i + 1]];
            }
        }
        for (const std::string& w : unique) {
            ++documents[w];
        }
    }
    const double n = static_cast<double>(std::max<std::size_t>(1, sentences.size()));
    const auto idf = [&](const std::string& w) {
        const auto it = documents.find(w);
        return std::log(1.0 + n / static_cast<double>(it == documents.end() ? 1 : it->second));
    };
    std::vector<std::pair<std::string, double>> scored;
    for (const auto& [w, c] : counts) {
        if (c >= 2 || sentences.size() < 6) {
            scored.emplace_back(w, static_cast<double>(c) * idf(w) * (w.size() >= 5 ? 1.15 : 1.0));
        }
    }
    for (const auto& [p, c] : pairs) {
        if (c >= 2) {
            const std::size_t space = p.find(' ');
            scored.emplace_back(p, static_cast<double>(c) * 1.6 * (idf(p.substr(0, space)) + idf(p.substr(space + 1))) / 2.0);
        }
    }
    std::ranges::stable_sort(scored, [](const auto& a, const auto& b) { return a.second > b.second; });
    // A short video repeats little: then the words of its most telling sentences (four letters or more, not the words
    // every video says), at most 8 tags in all.
    if (scored.size() < 5) {
        static const std::set<std::string, std::less<>> generic{
            "back", "last", "next", "first", "today", "week", "time", "everyone", "thanks", "watching", "welcome", "really",
            "going", "thing", "things", "want", "know", "like", "just", "make", "made", "good", "great", "little", "much",
            "many", "very", "well", "also", "then", "there", "here", "come", "came", "went", "said", "says", "think"};
        double score = 0.5;
        for (const std::string& text : telling) {
            for (const std::string& w : shorts::TextVectors::tokens(text)) {
                if (scored.size() >= 8) {
                    break;
                }
                if (w.size() < 4 || generic.contains(w) || std::ranges::any_of(scored, [&](const auto& t) { return t.first == w; })) {
                    continue;
                }
                scored.emplace_back(w, score);
                score *= 0.99;
            }
        }
    }
    std::vector<std::string> out;
    std::size_t chars = 0;
    for (const auto& [tag, score] : scored) {
        if (out.size() == 15) {
            break;
        }
        // A word a chosen phrase already holds adds little; a phrase over two chosen words replaces nothing.
        if (std::ranges::any_of(out, [&](const std::string& t) { return t == tag || (t.find(' ') != std::string::npos &&
                                                                                    (t.starts_with(tag + " ") || t.ends_with(" " + tag))); })) {
            continue;
        }
        if (chars + tag.size() + 1 > 500) {
            break;
        }
        chars += tag.size() + 1;
        out.push_back(tag);
    }
    return out;
}

} // namespace

FrameQuality frameQuality(const search::Image& picture) {
    FrameQuality q;
    if (!picture.valid()) {
        q.usable = false;
        q.reasons.emplace_back("no picture");
        return q;
    }
    // Every few pixels: about 160 x 90 samples.
    const int step = std::max(1, picture.width / 160);
    const int w = picture.width / step;
    const int h = picture.height / step;
    if (w < 3 || h < 3) {
        q.usable = false;
        q.reasons.emplace_back("too small");
        return q;
    }
    std::vector<double> luma(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    double sum = 0.0;
    double sumSq = 0.0;
    double rgSum = 0.0, ybSum = 0.0, rgSq = 0.0, ybSq = 0.0;
    std::size_t clipped = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y * step) * static_cast<std::size_t>(picture.width) + static_cast<std::size_t>(x * step)) * 3;
            const double r = picture.rgb[i];
            const double g = picture.rgb[i + 1];
            const double b = picture.rgb[i + 2];
            const double v = (0.299 * r + 0.587 * g + 0.114 * b) / 255.0;
            luma[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = v;
            sum += v;
            sumSq += v * v;
            clipped += v < 0.02 || v > 0.98 ? 1 : 0;
            const double rg = r - g;
            const double yb = 0.5 * (r + g) - b;
            rgSum += rg;
            ybSum += yb;
            rgSq += rg * rg;
            ybSq += yb * yb;
        }
    }
    const double n = static_cast<double>(luma.size());
    const double mean = sum / n;
    const double sd = std::sqrt(std::max(0.0, sumSq / n - mean * mean));
    double lap = 0.0;
    for (int y = 1; y + 1 < h; ++y) {
        for (int x = 1; x + 1 < w; ++x) {
            const auto at = [&](int xx, int yy) { return luma[static_cast<std::size_t>(yy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(xx)]; };
            lap += std::abs(4.0 * at(x, y) - at(x - 1, y) - at(x + 1, y) - at(x, y - 1) - at(x, y + 1));
        }
    }
    lap /= static_cast<double>((w - 2) * (h - 2));
    const double rgMean = rgSum / n;
    const double ybMean = ybSum / n;
    const double rgSd = std::sqrt(std::max(0.0, rgSq / n - rgMean * rgMean));
    const double ybSd = std::sqrt(std::max(0.0, ybSq / n - ybMean * ybMean));
    const double colourfulness = std::sqrt(rgSd * rgSd + ybSd * ybSd) + 0.3 * std::sqrt(rgMean * rgMean + ybMean * ybMean);
    q.sharpness = std::clamp(lap / 0.06, 0.0, 1.0);
    q.contrast = std::clamp(sd / 0.25, 0.0, 1.0);
    q.colour = std::clamp(colourfulness / 80.0, 0.0, 1.0);
    q.exposure = std::clamp(1.0 - std::abs(mean - 0.5) * 2.0, 0.0, 1.0) * (static_cast<double>(clipped) / n > 0.4 ? 0.5 : 1.0);
    q.score = 0.35 * q.sharpness + 0.25 * q.contrast + 0.25 * q.colour + 0.15 * q.exposure;
    if (mean < 0.06) {
        q.usable = false;
        q.reasons.emplace_back("too dark");
    } else if (mean > 0.96) {
        q.usable = false;
        q.reasons.emplace_back("blown out");
    } else if (sd < 0.02) {
        q.usable = false;
        q.reasons.emplace_back("flat");
    }
    if (q.usable) {
        if (q.sharpness >= 0.6) {
            q.reasons.emplace_back("sharp");
        }
        if (q.contrast >= 0.6) {
            q.reasons.emplace_back("strong contrast");
        }
        if (q.colour >= 0.5) {
            q.reasons.emplace_back("colourful");
        }
        if (q.exposure >= 0.7) {
            q.reasons.emplace_back("well exposed");
        }
    }
    return q;
}

std::string PublishingKit::description() const {
    std::string out = lead;
    if (chapters.size() >= 2) {
        out += (out.empty() ? "" : "\n\n") + std::string("Chapters\n") + chaptersText(chapters);
        while (!out.empty() && out.back() == '\n') {
            out.pop_back();
        }
    }
    if (const std::string tags_ = hashtags(tags); !tags_.empty()) {
        out += (out.empty() ? "" : "\n\n") + tags_;
    }
    return out;
}

PublishingKit makeKit(const ProjectState& state, SequenceId sequence, const edit::SequenceTranscript& transcript,
                      const FramePicture& frames, const KitOptions& options,
                      const std::function<void(std::size_t, std::size_t)>& progress) {
    PublishingKit kit;
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        kit.warnings.emplace_back("no such sequence");
        return kit;
    }
    const Ticks end = seq->contentEnd();
    const double length = end.toSeconds();
    const std::vector<shorts::Sentence> sentences = shorts::splitSentences(transcript);
    const std::vector<Line> lines = transcriptLines(transcript, 6);
    if (lines.empty()) {
        kit.warnings.emplace_back("the sequence has no transcribed words: titles, description and tags need a transcript");
    }

    // ---- Title ideas: the most telling lines, the hook counting extra.
    std::vector<std::pair<std::size_t, double>> telling;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Line& l = lines[i];
        const std::size_t words = wordCount(l.text);
        double score = l.weight;
        if (l.range.start.toSeconds() < std::max(30.0, 0.15 * length)) {
            score *= 1.3;
        }
        if (!l.text.empty() && l.text.back() == '?') {
            score *= 1.1;
        }
        if (words < 4) {
            score *= 0.5;
        } else if (words > 25) {
            score *= 0.8;
        }
        telling.emplace_back(i, score);
    }
    std::ranges::stable_sort(telling, [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [i, score] : telling) {
        if (kit.titles.size() >= options.titleIdeas) {
            break;
        }
        std::string title = shorts::titleFrom(lines[i].text, 70);
        if (!title.empty() && wordCount(title) >= 3 && std::ranges::find(kit.titles, title) == kit.titles.end()) {
            kit.titles.push_back(std::move(title));
        }
    }
    if (!looksDefault(seq->name) && std::ranges::find(kit.titles, seq->name) == kit.titles.end()) {
        kit.titles.push_back(trimmed(seq->name, 100));
    }
    if (kit.titles.empty()) {
        kit.titles.push_back(trimmed(seq->name.empty() ? std::string("Untitled") : seq->name, 100));
    }

    // ---- The lead: the three most telling lines of a fair length, in order.
    std::vector<std::size_t> leadLines;
    for (const auto& [i, score] : telling) {
        const std::size_t words = wordCount(lines[i].text);
        if (words >= 6 && words <= 40) {
            leadLines.push_back(i);
        }
        if (leadLines.size() == 3) {
            break;
        }
    }
    std::ranges::sort(leadLines);
    for (const std::size_t i : leadLines) {
        std::string text = lines[i].text;
        if (!text.empty() && text.back() != '.' && text.back() != '?' && text.back() != '!') {
            text += ".";
        }
        if (kit.lead.size() + text.size() > 450) {
            break;
        }
        kit.lead += (kit.lead.empty() ? "" : " ") + text;
    }

    // ---- Chapters.
    std::vector<KitChapter> markerChapters;
    for (const Marker& m : seq->markers) {
        if (m.type == MarkerType::Chapter && m.start < end) {
            markerChapters.push_back(KitChapter{m.start, m.name.empty() ? "Chapter " + std::to_string(markerChapters.size() + 1) : trimmed(m.name, 100)});
        }
    }
    std::ranges::stable_sort(markerChapters, [](const KitChapter& a, const KitChapter& b) { return a.start < b.start; });
    const bool useMarkers = options.chapters == "markers" || (options.chapters == "auto" && !markerChapters.empty());
    if (options.chapters == "none") {
        kit.chaptersFrom = "none";
    } else if (useMarkers) {
        kit.chapters = markerChapters;
        kit.chaptersFrom = "markers";
    } else {
        const double minSeconds = std::clamp(length / 8.0, 10.0, 60.0);
        for (const workflow::Chapter& c : workflow::chaptersFromTopics(transcript, minSeconds, 12)) {
            kit.chapters.push_back(KitChapter{c.start, c.title});
        }
        kit.chaptersFrom = kit.chapters.size() >= 2 ? "topics" : "none";
        if (kit.chapters.size() < 2) {
            kit.chapters.clear();
        }
    }
    if (!kit.chapters.empty()) {
        if (kit.chapters.front().start >= seconds(1.0)) {
            kit.chapters.insert(kit.chapters.begin(), KitChapter{Ticks{}, "Intro"});
        }
        kit.chapters.front().start = Ticks{};
        // One chapter per second at most (YouTube's times are whole seconds).
        std::vector<KitChapter> unique;
        for (const KitChapter& c : kit.chapters) {
            if (unique.empty() || static_cast<std::int64_t>(c.start.toSeconds()) > static_cast<std::int64_t>(unique.back().start.toSeconds())) {
                unique.push_back(c);
            }
        }
        kit.chapters = std::move(unique);
        if (kit.chapters.size() < 3) {
            kit.warnings.push_back("YouTube shows chapters only with at least 3 of them (this kit has " + std::to_string(kit.chapters.size()) + ")");
        }
        for (std::size_t i = 0; i < kit.chapters.size(); ++i) {
            const Ticks to = i + 1 < kit.chapters.size() ? kit.chapters[i + 1].start : end;
            if ((to - kit.chapters[i].start).toSeconds() < 10.0) {
                kit.warnings.push_back("YouTube needs every chapter to be 10 seconds or longer: \"" + kit.chapters[i].title + "\" is shorter");
                break;
            }
        }
    }

    // ---- Tags.
    std::vector<std::string> tellingTexts;
    for (std::size_t k = 0; k < telling.size() && k < 4; ++k) {
        tellingTexts.push_back(lines[telling[k].first].text);
    }
    kit.tags = tagsOf(sentences, tellingTexts);

    // ---- Thumbnail candidates.
    struct Candidate {
        Ticks time;
        double bonus = 0.0;
        std::string reason;
    };
    std::vector<Candidate> candidates;
    const Ticks last = end - seconds(0.5);
    const auto add = [&](Ticks t, double bonus, std::string reason) {
        t = snapToFrame(std::clamp(t, Ticks{}, std::max(Ticks{}, last)), seq->frameRate);
        for (Candidate& c : candidates) {
            if (std::abs((c.time - t).toSeconds()) < 1.0) {
                if (bonus > c.bonus) {
                    c = Candidate{t, bonus, std::move(reason)};
                }
                return;
            }
        }
        candidates.push_back(Candidate{t, bonus, std::move(reason)});
    };
    for (const Marker& m : seq->markers) {
        if (m.highlight) {
            add(m.highlight->peak, 0.15 + 0.1 * m.highlight->score, "highlight" + (m.name.empty() ? std::string() : ": " + m.name));
        }
    }
    for (std::size_t i = 0; i < kit.chapters.size(); ++i) {
        add(kit.chapters[i].start + seconds(1.5), 0.05, "chapter start: " + kit.chapters[i].title);
    }
    for (std::size_t k = 0; k < telling.size() && k < 6; ++k) {
        const Line& l = lines[telling[k].first];
        add(l.range.start + Ticks{l.range.duration.value / 2}, 0.05, "says: " + trimmed(l.text, 60));
    }
    const Rhythm rhythm = editRhythm(*seq);
    {
        Ticks from = rhythm.start;
        std::vector<Ticks> middles;
        for (std::size_t i = 0; i <= rhythm.cuts.size(); ++i) {
            const Ticks to = i < rhythm.cuts.size() ? rhythm.cuts[i] : rhythm.end;
            if (to - from >= seconds(1.0)) {
                middles.push_back(from + Ticks{(to - from).value / 2});
            }
            from = to;
        }
        // At most 24 shot middles, spread over the whole edit.
        const std::size_t take = std::min<std::size_t>(24, middles.size());
        for (std::size_t i = 0; i < take; ++i) {
            add(middles[i * middles.size() / take], 0.0, "a shot");
        }
    }
    std::vector<ThumbnailPick> scored;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (progress) {
            progress(i, candidates.size());
        }
        const Candidate& c = candidates[i];
        ThumbnailPick pick;
        pick.time = c.time;
        pick.reasons.push_back(c.reason);
        const auto shown = pictureAt(state, sequence, c.time);
        if (shown) {
            pick.media = shown->first->id;
            pick.sourceTime = shown->second;
        }
        std::optional<search::Image> picture = shown && frames ? frames(*shown->first, shown->second) : std::nullopt;
        if (picture && picture->valid()) {
            const FrameQuality q = frameQuality(*picture);
            if (!q.usable) {
                continue;
            }
            pick.score = std::min(1.0, 0.75 * q.score + c.bonus);
            pick.reasons.insert(pick.reasons.end(), q.reasons.begin(), q.reasons.end());
        } else if (!shown) {
            continue; // nothing on screen there
        } else {
            pick.score = std::min(1.0, 0.4 + c.bonus);
        }
        scored.push_back(std::move(pick));
    }
    std::ranges::stable_sort(scored, [](const ThumbnailPick& a, const ThumbnailPick& b) { return a.score > b.score; });
    const Ticks apart = seconds(options.thumbnailGapSeconds);
    for (ThumbnailPick& p : scored) {
        if (kit.thumbnails.size() >= options.thumbnails) {
            break;
        }
        if (std::ranges::any_of(kit.thumbnails, [&](const ThumbnailPick& t) { return std::abs((t.time - p.time).value) < apart.value; })) {
            continue;
        }
        kit.thumbnails.push_back(std::move(p));
    }
    if (options.thumbnails > 0 && kit.thumbnails.empty()) {
        kit.warnings.emplace_back("no frame of the sequence makes a usable thumbnail");
    }
    if (progress) {
        progress(candidates.size(), candidates.size());
    }
    return kit;
}

// ---- The cloud copy ------------------------------------------------------------------------------------------------

std::string kitTranscriptText(const edit::SequenceTranscript& transcript, std::size_t maxChars) {
    std::string out;
    for (const shorts::Sentence& s : shorts::splitSentences(transcript)) {
        if (out.size() + s.text.size() + 1 > maxChars) {
            break;
        }
        out += (out.empty() ? "" : " ") + s.text;
    }
    return out;
}

ChatRequest kitChatRequest(std::string_view transcriptText, std::span<const KitChapter> chapters, std::string_view sequenceName) {
    ChatRequest r;
    r.system =
        "You write the upload copy for a creator's video from its transcript. Answer with JSON only, of this shape: "
        "{\"titles\": [three different titles, at most 70 characters each], \"description\": \"two or three short "
        "paragraphs for the video description, without a chapter list and without hashtags\", \"tags\": [up to 15 short "
        "search tags]}. Write in the language of the transcript. Use only what the transcript says: no invented facts, "
        "no links, no calls to subscribe.";
    std::string user = "Working title: " + std::string(sequenceName) + "\n";
    if (!chapters.empty()) {
        user += "Chapters:\n" + chaptersText(chapters);
    }
    user += "\nTranscript:\n" + std::string(transcriptText);
    r.messages.push_back(ChatMessage::user(std::move(user)));
    Json strings{{"type", "array"}, {"items", Json{{"type", "string"}}}};
    r.responseFormat = ResponseFormat{"publishing_kit",
                                      Json{{"type", "object"},
                                           {"properties", Json{{"titles", strings}, {"description", Json{{"type", "string"}}}, {"tags", strings}}},
                                           {"required", Json::array({"titles", "description", "tags"})},
                                           {"additionalProperties", false}},
                                      true};
    r.maxOutputTokens = 1200;
    return r;
}

std::optional<CloudCopy> parseKitReply(std::string_view reply) {
    const std::size_t open = reply.find('{');
    const std::size_t close = reply.rfind('}');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
        return std::nullopt;
    }
    const Json j = Json::parse(reply.substr(open, close - open + 1), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return std::nullopt;
    }
    CloudCopy copy;
    if (const auto it = j.find("titles"); it != j.end() && it->is_array()) {
        for (const Json& t : *it) {
            if (t.is_string() && !t.get<std::string>().empty() && copy.titles.size() < 5) {
                copy.titles.push_back(trimmed(t.get<std::string>(), 100));
            }
        }
    }
    if (const auto it = j.find("description"); it != j.end() && it->is_string()) {
        copy.description = trimmed(it->get<std::string>(), 4500);
    }
    if (const auto it = j.find("tags"); it != j.end() && it->is_array()) {
        std::size_t chars = 0;
        for (const Json& t : *it) {
            if (!t.is_string()) {
                continue;
            }
            std::string tag = trimmed(t.get<std::string>(), 100);
            std::erase(tag, '#');
            std::erase(tag, ',');
            if (tag.empty() || copy.tags.size() == 15 || chars + tag.size() + 1 > 500) {
                continue;
            }
            chars += tag.size() + 1;
            copy.tags.push_back(std::move(tag));
        }
    }
    if (copy.titles.empty() && copy.description.empty()) {
        return std::nullopt;
    }
    return copy;
}

} // namespace wabcut::ai::autopilot
