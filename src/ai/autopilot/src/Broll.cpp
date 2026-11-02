// Creator autopilot: Auto B-roll (Autopilot.h). Lines of the transcript matched against the library by picture and by
// words, chosen within the density rules, and placed on a free video track in one edit.

#include <wabcut/ai/autopilot/Autopilot.h>

#include <wabcut/model/Auditions.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_set>

namespace wabcut::ai::autopilot {

namespace {

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

/// Whole frames of `rate`, rounded down.
Ticks floorToFrame(Ticks t, Rate rate) {
    const Ticks frame{rate.ticksPerUnit()};
    return frame.value > 0 ? Ticks{t.value / frame.value * frame.value} : t;
}

/// Where a word match was found in a library item.
struct Segment {
    std::string field;              ///< "name" | "bin" | "description" | "marker" | "transcript"
    std::string label;              ///< for the reason
    std::set<std::string> words;
    std::optional<TimeRange> range; ///< the moment (source time), when the field has one
    double weight = 1.0;
};

struct Entry {
    const MediaItem* media = nullptr;
    std::vector<Segment> segments;
};

std::set<std::string> wordSet(std::string_view text) {
    const std::vector<std::string> words = keywordTokens(text);
    return {words.begin(), words.end()};
}

/// The words of a library item: its name, bins, Description and Comment, clip markers, transcript phrases.
Entry entryOf(const ProjectState& state, const MediaItem& m, const edit::TranscriptProvider* transcripts) {
    Entry e;
    e.media = &m;
    e.segments.push_back(Segment{"name", m.name, wordSet(m.name), std::nullopt, 1.0});
    std::string bins;
    std::set<std::uint64_t> visited;
    for (const Bin* b = state.findBin(m.bin); b && visited.insert(b->id.value).second; b = state.findBin(b->parent)) {
        bins = b->name + (bins.empty() ? "" : " / " + bins);
    }
    if (!bins.empty()) {
        e.segments.push_back(Segment{"bin", bins, wordSet(bins), std::nullopt, 0.7});
    }
    if (const std::string text = m.description + " " + m.comment; text.size() > 1) {
        e.segments.push_back(Segment{"description", m.description.empty() ? m.comment : m.description, wordSet(text), std::nullopt, 0.9});
    }
    for (const Marker& marker : m.markers) {
        const std::string text = marker.name + " " + marker.comment;
        if (text.size() > 1) {
            e.segments.push_back(Segment{"marker", marker.name.empty() ? marker.comment : marker.name, wordSet(text),
                                         TimeRange{marker.start, std::max(marker.duration, seconds(1.0))}, 1.0});
        }
    }
    if (transcripts && !m.info.audio.empty()) {
        if (const std::shared_ptr<const Transcript> t = transcripts->transcript(m, 0); t && !t->words.empty()) {
            // Phrases: words up to a sentence end or a pause of 0.7 s.
            std::string text;
            Ticks from = t->words.front().t0;
            Ticks to = from;
            const auto flush = [&] {
                if (!text.empty()) {
                    e.segments.push_back(Segment{"transcript", text, wordSet(text), TimeRange::fromStartEnd(from, std::max(to, from)), 0.6});
                }
                text.clear();
            };
            for (const TranscriptWord& w : t->words) {
                if (!text.empty() && w.t0 - to > seconds(0.7)) {
                    flush();
                }
                if (text.empty()) {
                    from = w.t0;
                }
                text += (text.empty() ? "" : " ") + w.text;
                to = w.t1;
                if (!w.text.empty() && (w.text.back() == '.' || w.text.back() == '?' || w.text.back() == '!')) {
                    flush();
                }
            }
            flush();
        }
    }
    return e;
}

/// A candidate for a line.
struct Option {
    const MediaItem* media = nullptr;
    Ticks centre;        ///< source time to show around
    bool centred = false;
    std::optional<TimeRange> hint; ///< the source range that matched (a picture hit's shot)
    double score = 0.0;
    std::string how;
    std::string evidence;
    std::string reason;
};

double pictureScore(float similarity) { return std::clamp((static_cast<double>(similarity) - 0.06) / 0.14, 0.0, 1.0); }

/// The source range of `length` to show for an option, inside the media; nullopt when the media is too short.
std::optional<TimeRange> sourceFor(const Option& o, Ticks length, Rate rate) {
    const MediaItem& m = *o.media;
    const std::optional<Ticks> extent = m.info.isStillImage ? std::nullopt : m.sourceExtent(StreamRef{StreamKind::Video, 0});
    if (!extent) {
        return TimeRange{Ticks{}, length}; // a still: any length
    }
    if (*extent < length) {
        length = floorToFrame(*extent, rate);
        if (length < seconds(1.0)) {
            return std::nullopt;
        }
    }
    Ticks centre{extent->value / 2};
    if (o.centred) {
        centre = o.centre;
    } else if (m.markIn || m.markOut) {
        const Ticks in = m.markIn.value_or(Ticks{});
        const Ticks out = std::min(m.markOut.value_or(*extent), *extent);
        centre = in + Ticks{(std::max(out, in) - in).value / 2};
    } else if (m.posterFrame) {
        centre = *m.posterFrame;
    }
    Ticks start = centre - Ticks{length.value / 2};
    if (o.hint && o.hint->duration >= length) {
        // A picture hit: stay inside the shot that matched.
        start = std::clamp(start, o.hint->start, o.hint->end() - length);
    }
    start = std::clamp(start, Ticks{}, *extent - length);
    return TimeRange{floorToFrame(start, rate), length};
}

} // namespace

std::string generationPrompt(const Line& line) {
    std::string words;
    for (const std::string& k : line.keywords) {
        words += (words.empty() ? "" : ", ") + k;
    }
    std::string text = line.text.size() > 160 ? line.text.substr(0, 160) : line.text;
    return "B-roll for a video: " + text + (words.empty() ? "" : " (" + words + ")") + ". Cinematic, no text, no people talking to camera.";
}

BrollPlan planBroll(const ProjectState& state, SequenceId sequence, const edit::SequenceTranscript& transcript,
                    const edit::TranscriptProvider* mediaTranscripts, const PictureSearch& pictures, const BrollOptions& options,
                    const std::function<void(std::size_t, std::size_t)>& progress) {
    BrollPlan plan;
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        plan.warnings.emplace_back("no such sequence");
        return plan;
    }
    plan.lines = transcriptLines(transcript);
    if (plan.lines.empty()) {
        plan.warnings.emplace_back("the sequence has no transcribed words: transcribe it first");
        return plan;
    }
    // The library: video items the sequence does not use (its A-roll and what is already placed; 0.6: an audition's
    // other takes are placed too).
    std::unordered_set<std::uint64_t> used;
    seq->forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            forEachTakeSource(c, [&](MediaId m, SequenceId) { used.insert(m.value); });
        }
    });
    std::vector<Entry> library;
    std::vector<MediaId> ids;
    for (const MediaPtr& m : state.media) {
        if (!m || !m->info.hasVideo() || m->isGenerator() || m->locator.offline || used.contains(m->id.value) ||
            m->subclip.has_value()) {
            continue;
        }
        library.push_back(entryOf(state, *m, mediaTranscripts));
        ids.push_back(m->id);
    }
    plan.candidates = library.size();
    if (library.empty()) {
        plan.warnings.emplace_back("the library has no other video to use as B-roll: import some first");
    }
    std::map<std::uint64_t, const MediaItem*> byId;
    for (const Entry& e : library) {
        byId[e.media->id.value] = e.media;
    }

    // Line keyword weights (the same specificity transcriptLines ranks by).
    std::map<std::string, std::size_t, std::less<>> documents;
    for (const Line& line : plan.lines) {
        for (const std::string& k : std::set<std::string>(line.keywords.begin(), line.keywords.end())) {
            ++documents[k];
        }
    }
    const double lineCount = static_cast<double>(plan.lines.size());
    const auto idf = [&](const std::string& w) {
        const auto it = documents.find(w);
        return std::log(1.0 + lineCount / static_cast<double>(it == documents.end() ? 1 : it->second)) * (w.size() >= 4 ? 1.0 : 0.7);
    };

    const Ticks end = seq->contentEnd();
    const Ticks intro = seconds(options.introSeconds);
    std::vector<std::vector<Option>> ranked(plan.lines.size());
    std::vector<bool> eligible(plan.lines.size(), false);
    for (std::size_t i = 0; i < plan.lines.size(); ++i) {
        if (progress) {
            progress(i, plan.lines.size());
        }
        const Line& line = plan.lines[i];
        if (line.range.start < intro || line.range.duration < seconds(0.8) || line.range.start >= end || library.empty()) {
            continue;
        }
        eligible[i] = true;
        std::vector<Option> options_;
        // By words.
        double denominator = 0.0;
        for (std::size_t k = 0; k < line.keywords.size() && k < 2; ++k) {
            denominator += idf(line.keywords[k]);
        }
        if (denominator > 0.0) {
            for (const Entry& e : library) {
                Option best;
                for (const Segment& s : e.segments) {
                    double matched = 0.0;
                    std::string words;
                    for (const std::string& k : line.keywords) {
                        if (k.size() >= 3 && s.words.contains(k)) {
                            matched += idf(k);
                            words += (words.empty() ? "" : ", ") + k;
                        }
                    }
                    const double score = std::min(1.0, matched / denominator) * s.weight;
                    if (score > best.score) {
                        best.media = e.media;
                        best.score = score;
                        best.how = "words";
                        best.evidence = s.field;
                        best.centred = s.range.has_value();
                        best.centre = s.range ? s.range->start + Ticks{s.range->duration.value / 2} : Ticks{};
                        best.reason = s.field + ": " + (s.label.size() > 80 ? s.label.substr(0, 80) + "..." : s.label) + " (" + words + ")";
                    }
                }
                if (best.media && best.score > 0.0) {
                    options_.push_back(std::move(best));
                }
            }
        }
        // By picture.
        if (pictures) {
            const std::string query = line.text.size() > 300 ? line.text.substr(0, 300) : line.text;
            const std::vector<PictureHit> hits = pictures(query, ids);
            plan.visual = plan.visual || !hits.empty();
            std::set<std::uint64_t> seen;
            for (const PictureHit& hit : hits) {
                const auto it = byId.find(hit.media.value);
                if (it == byId.end() || hit.similarity < 0.12f || !seen.insert(hit.media.value).second) {
                    continue;
                }
                Option o;
                o.media = it->second;
                o.score = pictureScore(hit.similarity);
                o.how = "picture";
                o.evidence = "picture";
                o.centred = true;
                o.centre = hit.thumbnail;
                o.hint = hit.range;
                o.reason = "shows: " + (query.size() > 80 ? query.substr(0, 80) + "..." : query);
                // Seen by picture and named for it too: surer.
                if (const auto words = std::ranges::find_if(options_, [&](const Option& w) { return w.media == o.media; });
                    words != options_.end()) {
                    o.score = std::min(1.0, o.score + 0.15);
                    o.reason += "; " + words->reason;
                    options_.erase(words);
                }
                options_.push_back(std::move(o));
            }
        }
        std::erase_if(options_, [&](const Option& o) { return o.score < options.minScore; });
        std::ranges::stable_sort(options_, [](const Option& a, const Option& b) { return a.score > b.score; });
        if (options_.size() > 4) {
            options_.resize(4);
        }
        ranked[i] = std::move(options_);
    }

    // Choose: best lines first, within the density rules; then in time order.
    const double minutes = end.toSeconds() / 60.0;
    const auto cap = static_cast<std::size_t>(std::max(1.0, std::floor(options.perMinute * minutes + 0.5)));
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < ranked.size(); ++i) {
        if (!ranked[i].empty()) {
            order.push_back(i);
        }
    }
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) { return ranked[a].front().score > ranked[b].front().score; });
    std::map<std::uint64_t, std::size_t> uses;
    std::set<std::pair<std::uint64_t, std::int64_t>> shots;
    const Ticks gap = seconds(options.gapSeconds);
    for (const std::size_t i : order) {
        if (plan.matches.size() >= cap) {
            break;
        }
        const Line& line = plan.lines[i];
        const Ticks at = std::min(snapToFrame(line.range.start, seq->frameRate), end);
        Ticks length = std::clamp(line.range.duration, seconds(options.minSeconds), seconds(options.maxSeconds));
        length = floorToFrame(std::min(length, end - at), seq->frameRate);
        if (length < seconds(1.0)) {
            continue;
        }
        const bool crowded = std::ranges::any_of(plan.matches, [&](const BrollMatch& m) {
            const Ticks mEnd = m.at + m.source.duration;
            return at < mEnd + gap && m.at < at + length + gap;
        });
        if (crowded) {
            continue;
        }
        for (const Option& o : ranked[i]) {
            if (uses[o.media->id.value] >= options.usesPerMedia) {
                continue;
            }
            const std::optional<TimeRange> source = sourceFor(o, length, seq->frameRate);
            if (!source) {
                continue;
            }
            // The same shot (to the second) is never shown twice.
            const auto shot = std::pair{o.media->id.value, source->start.value / kTicksPerSecond};
            if (shots.contains(shot)) {
                continue;
            }
            shots.insert(shot);
            ++uses[o.media->id.value];
            plan.matches.push_back(BrollMatch{i, o.media->id, o.media->name, *source, at, o.score, o.how, o.evidence, o.reason});
            break;
        }
    }
    std::ranges::sort(plan.matches, [](const BrollMatch& a, const BrollMatch& b) { return a.at < b.at; });
    for (std::size_t i = 0; i < plan.lines.size(); ++i) {
        if (eligible[i] && ranked[i].empty()) {
            plan.unmatched.push_back(i);
        }
    }
    if (progress) {
        progress(plan.lines.size(), plan.lines.size());
    }
    return plan;
}

std::expected<PlacedBroll, Error> placeBroll(const ProjectState& state, SequenceId sequence, std::span<const BrollMatch> matches) {
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        return std::unexpected(Error(ErrorCode::NotFound, "no such sequence"));
    }
    if (matches.empty()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument, "no B-roll to place"));
    }
    for (const BrollMatch& m : matches) {
        const MediaItem* media = state.findMedia(m.media);
        if (!media) {
            return std::unexpected(Error(ErrorCode::NotFound, "no media with id " + std::to_string(m.media.value)));
        }
        if (!media->info.hasVideo()) {
            return std::unexpected(Error(ErrorCode::InvalidArgument, media->name + " has no picture"));
        }
    }
    const auto overlaps = [&](const Track& t) {
        return std::ranges::any_of(t.clips, [&](const Clip& c) {
            return std::ranges::any_of(matches, [&](const BrollMatch& m) { return c.start() < m.at + m.source.duration && m.at < c.end(); });
        });
    };
    // The A-roll: the highest video track with a linked (picture and sound) clip where the B-roll goes.
    std::size_t aroll = 0;
    for (std::size_t i = 0; i < seq->videoTracks.size(); ++i) {
        const Track& t = *seq->videoTracks[i];
        const bool linked = std::ranges::any_of(t.clips, [&](const Clip& c) {
            return c.linkGroup.isValid() && std::ranges::any_of(matches, [&](const BrollMatch& m) {
                       return c.start() < m.at + m.source.duration && m.at < c.end();
                   });
        });
        if (linked) {
            aroll = i;
        }
    }
    PlacedBroll out;
    out.state = state;
    std::optional<std::size_t> chosen;
    for (std::size_t i = aroll + 1; i < seq->videoTracks.size(); ++i) {
        const Track& t = *seq->videoTracks[i];
        if (!t.locked && !overlaps(t)) {
            chosen = i;
            break;
        }
    }
    if (chosen) {
        out.track = BrollTrack{seq->videoTracks[*chosen]->id, *chosen, seq->videoTracks[*chosen]->name, false};
    } else {
        auto created = wabcut::edit::addTrack(state, sequence, TrackKind::Video, "B-roll");
        if (!created) {
            return std::unexpected(created.error());
        }
        out.state = std::move(created->state);
        const Sequence* now = out.state.findSequence(sequence);
        const auto pos = now ? now->trackPosition(created->id) : std::nullopt;
        out.track = BrollTrack{created->id, pos ? pos->index : seq->videoTracks.size(), "B-roll", true};
    }
    std::vector<wabcut::edit::ClipPlacement> placements;
    for (const BrollMatch& m : matches) {
        placements.push_back(wabcut::edit::ClipPlacement{out.track.track, m.media, StreamRef{StreamKind::Video, 0}, m.at, m.source.duration, m.source.start});
    }
    auto placed = wabcut::edit::placeClips(out.state, sequence, placements);
    if (!placed) {
        return std::unexpected(placed.error());
    }
    out.state = std::move(placed->state);
    out.clips = std::move(placed->placed);
    return out;
}

} // namespace wabcut::ai::autopilot
