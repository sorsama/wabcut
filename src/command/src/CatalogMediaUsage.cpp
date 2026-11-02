// P1 round (#25): sequence.mediaUsage, the media usage report for archive clearances, music licensing and credits
// (Avid's source lists, Premiere's EDL reports): one row per use of a source in the sequence, with its record and source
// timecodes, and a totals block per source. Read only: the app writes the text to a file (File > Export > Media Usage
// Report…).
//
// The walk: every enabled clip on the tracks the export plays (enabled video tracks, audible audio tracks), over the
// whole sequence or its In/Out marks. With "flatten" (the default) nested sequences are reported through: their clips'
// timeline ranges are mapped into the top sequence's record time, and a multicam clip reports its active angle(s) only
// (an audition, likewise, its active take only: the other takes are not in the cut and need no clearance).
// A linked video and audio clip of one source over the same frames is one use ("V1 A1"). Generated items (titles,
// colour mattes, bars) and graphics are not sources and are left out. A time-remapped clip, and anything inside a
// nested clip that is remapped, reversed or held, is reported as a hull (its whole range) with the note "remapped".

#include "Catalog.h"

#include <wabcut/model/EditingOps.h>
#include <wabcut/model/Resolve.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace wabcut::command::detail {

void registerMediaUsageCommands(CommandRegistry& registry);

namespace {

constexpr std::size_t kMaxDepth = 64; // as Resolve.h's nesting bound
constexpr std::size_t kMaxUses = 100'000;

/// Record time of a nested time: linear (parentStart + (t - nestedStart) * den / num), or not exact (the whole parent
/// range stands for any nested time: a remapped, reversed or held nest).
struct RecordMap {
    Ticks parentStart;
    Ticks nestedStart;
    std::int64_t num = 1;
    std::int64_t den = 1;
    bool exact = true;
    TimeRange hull; ///< the parent's record range of the whole nest (used when !exact)
};

struct Use {
    MediaId media;
    std::vector<std::string> tracks; ///< "V1", "A1", or "V1 > V2" through a nest
    std::string via;                 ///< the nested sequences it is seen through, " > "-joined
    TimeRange record;                ///< top sequence time
    TimeRange source;                ///< media time
    std::string speed;
    std::vector<std::string> notes;
};

struct Walk {
    const ProjectState& state;
    TimeRange range; ///< the report's range, top sequence time
    bool flatten = true;
    std::vector<Use> uses;
    bool truncated = false;
};

std::string percentText(double p) {
    const double rounded = std::round(p * 10.0) / 10.0;
    std::string s = std::to_string(rounded);
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    return s + "%";
}

std::string speedText(const Clip& clip) {
    if (clip.isRemapped())
        return "remapped";
    if (clip.isFreeze())
        return "freeze frame";
    const std::string p = percentText(100.0 * clip.speed.num / clip.speed.den);
    return clip.reversed ? "reverse " + p : p;
}

/// Maps a time of the sequence being walked to the top sequence through `maps` (outermost first).
Ticks toRecord(const std::vector<RecordMap>& maps, Ticks t, bool& exact) {
    for (auto it = maps.rbegin(); it != maps.rend(); ++it) {
        if (!it->exact) {
            exact = false;
            return t; // the caller uses the hull
        }
        const auto offset = mulDiv((t - it->nestedStart).value, it->den, it->num, Rounding::Floor);
        t = it->parentStart + Ticks{offset.value_or(0)};
    }
    return t;
}

std::string trackLabelOf(bool video, std::size_t index) {
    return (video ? "V" : "A") + std::to_string(index + 1);
}

void walkTracks(Walk& w, const Sequence& seq, bool video, const std::vector<std::size_t>& tracks, TimeRange inside,
                std::vector<RecordMap>& maps, const std::string& trackPath, const std::string& via, std::size_t depth) {
    if (depth >= kMaxDepth)
        return;
    const std::vector<TrackPtr>& all = seq.tracks(video ? TrackKind::Video : TrackKind::Audio);
    for (const std::size_t index : tracks) {
        if (index >= all.size() || !all[index])
            continue;
        const std::string label = trackPath.empty() ? trackLabelOf(video, index)
                                                    : trackPath + " > " + trackLabelOf(video, index);
        for (const Clip& clip : all[index]->clips) {
            if (!clip.enabled || !clip.timelineRange.overlaps(inside))
                continue;
            if (w.uses.size() >= kMaxUses) {
                w.truncated = true;
                return;
            }
            const Ticks from = std::max(clip.start(), inside.start);
            const Ticks to = std::min(clip.end(), inside.end());
            if (!(from < to))
                continue;
            // Where it is in the top sequence.
            bool exact = true;
            TimeRange record = TimeRange::fromStartEnd(toRecord(maps, from, exact), toRecord(maps, to, exact));
            if (!exact) {
                for (const RecordMap& m : maps) {
                    if (!m.exact) {
                        record = m.hull;
                        break;
                    }
                }
            }
            if (clip.isNested()) {
                const Sequence* nested = w.state.findSequence(clip.sequence);
                if (!nested)
                    continue;
                const std::string name = nested->name.empty() ? "Sequence" : nested->name;
                if (!w.flatten) {
                    Use u;
                    u.tracks = {label};
                    u.via = via;
                    u.record = record;
                    u.source = clip.isRemapped() ? TimeRange{} : TimeRange::fromStartEnd(clip.sourceTimeAt(from), clip.sourceTimeAt(to - Ticks{1}) + Ticks{1});
                    u.speed = speedText(clip);
                    u.notes = {nested->multicam ? "multicam: " + name : "nested sequence: " + name};
                    w.uses.push_back(std::move(u));
                    continue;
                }
                // The nested time this part of the clip shows, and how it maps back.
                const TimeRange shown = clip.shownSourceRange(from, to);
                RecordMap m;
                m.exact = !clip.isRemapped() && !clip.reversed && !clip.isFreeze() && exact;
                m.parentStart = from;
                m.nestedStart = clip.sourceTimeAt(from);
                m.num = clip.speed.num;
                m.den = clip.speed.den;
                m.hull = record;
                maps.push_back(m);
                walkTracks(w, *nested, video, nestedTracksRead(*nested, clip), shown, maps, label,
                           via.empty() ? name : via + " > " + name, depth + 1);
                maps.pop_back();
                if (w.truncated)
                    return;
                continue;
            }
            if (!clip.media.isValid())
                continue; // a graphic or an unknown source
            const MediaItem* item = w.state.findMedia(clip.media);
            if (!item || item->generator)
                continue; // titles, mattes, bars: not footage
            Use u;
            u.media = clip.media;
            u.tracks = {label};
            u.via = via;
            u.record = record;
            u.source = clip.shownSourceRange(from, to);
            u.speed = speedText(clip);
            if (clip.isRemapped() || !exact)
                u.notes.push_back("remapped");
            w.uses.push_back(std::move(u));
        }
    }
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string s;
    for (const std::string& p : parts) {
        if (!s.empty())
            s += sep;
        s += p;
    }
    return s;
}

/// Merges a linked video and audio use of one source over the same frames into one row.
std::vector<Use> mergeLinked(std::vector<Use> uses) {
    std::vector<Use> out;
    // (A nested clip listed as itself, flatten off, merges with its linked partner the same way: by its note.)
    std::map<std::tuple<std::uint64_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t, std::string, std::string>,
             std::size_t>
        seen;
    for (Use& u : uses) {
        const auto key = std::tuple{u.media.value, u.record.start.value, u.record.duration.value, u.source.start.value,
                                    u.source.duration.value, u.via, join(u.notes, ";")};
        if (const auto it = seen.find(key); it != seen.end()) {
            Use& into = out[it->second];
            for (const std::string& t : u.tracks) {
                if (std::find(into.tracks.begin(), into.tracks.end(), t) == into.tracks.end())
                    into.tracks.push_back(t);
            }
            continue;
        }
        seen[key] = out.size();
        out.push_back(std::move(u));
    }
    std::stable_sort(out.begin(), out.end(), [](const Use& a, const Use& b) {
        return a.record.start < b.record.start || (a.record.start == b.record.start && a.tracks < b.tracks);
    });
    return out;
}


std::string csvCell(const std::string& text) {
    if (text.find_first_of(",\"\r\n") == std::string::npos)
        return text;
    std::string out = "\"";
    for (const char ch : text) {
        out += ch;
        if (ch == '"')
            out += '"';
    }
    return out + "\"";
}

std::string secondsText(double s) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.3f", s);
    return buffer;
}

/// The rate a media item's timecode counts in: its first video stream's, else the sequence's.
Rate mediaRate(const MediaItem& item, const Sequence& seq) {
    for (const auto& v : item.info.video) {
        if (v.frameRate.num > 0 && v.frameRate.den > 0)
            return v.frameRate;
    }
    return seq.frameRate;
}

CommandOutput mediaUsage(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const std::string format = c.string("format", "csv");
    const bool flatten = c.boolean("flatten", true);
    TimeRange range{Ticks{}, std::max(seq.duration, seq.contentEnd())};
    const std::string rangeName = c.string("range", "sequence");
    if (rangeName == "inOut") {
        const auto marked = seq.markedRange();
        if (!marked)
            fail(ErrorCode::InvalidParams, "the sequence has no In and Out marks", Json{{"path", "/range"}});
        range = *marked;
    }
    Walk w{c.state(), range, flatten, {}, false};
    std::vector<RecordMap> maps;
    for (const bool video : {true, false}) {
        const std::vector<TrackPtr>& all = seq.tracks(video ? TrackKind::Video : TrackKind::Audio);
        const bool anySolo = !video && std::any_of(all.begin(), all.end(), [](const TrackPtr& t) { return t && t->solo; });
        std::vector<std::size_t> tracks;
        for (std::size_t i = 0; i < all.size(); ++i) {
            const TrackPtr& t = all[i];
            if (t && t->enabled && (video || (!t->muted && (!anySolo || t->solo))))
                tracks.push_back(i);
        }
        walkTracks(w, seq, video, tracks, range, maps, {}, {}, 0);
    }
    const std::vector<Use> uses = mergeLinked(std::move(w.uses));

    const auto recordTc = [&](Ticks t) { return ticksToTimecode(seq.startTimecode + t, seq.frameRate, seq.dropFrame); };
    const auto sourceTc = [&](const MediaItem& item, Ticks t) {
        const Rate rate = mediaRate(item, seq);
        const bool df = rate == seq.frameRate && seq.dropFrame;
        return ticksToTimecode(wabcut::edit::mediaStartTimecode(item).value_or(Ticks{}) + t, rate, df);
    };

    struct Total {
        MediaId media;
        std::size_t uses = 0;
        Ticks used;
        std::vector<TimeRange> source;
    };
    std::vector<Total> totals;
    Json rows = Json::array();
    std::string csv = "Media Name,File Path,Tape Name,Scene,Shot,Take,AI Generated,Track,Nested In,Record In,Record Out,"
                      "Source In,Source Out,Duration,Seconds,Speed,Note\n";
    for (const Use& u : uses) {
        const MediaItem* item = u.media.isValid() ? c.state().findMedia(u.media) : nullptr;
        const std::string name = item ? item->name : (u.notes.empty() ? std::string() : u.notes.front());
        const std::string path = item ? (item->locator.path.empty() ? item->info.path : item->locator.path) : std::string();
        const std::string tape = item && item->timecode ? item->timecode->reelName : std::string();
        const bool generated = item && item->generated.has_value();
        const std::string srcIn = item && u.source.duration.value > 0 ? sourceTc(*item, u.source.start) : std::string();
        const std::string srcOut = item && u.source.duration.value > 0 ? sourceTc(*item, u.source.end()) : std::string();
        const std::string note = join(u.notes, "; ");
        const std::string tracks = join(u.tracks, " ");
        Json row{{"media", name},
                 {"path", path},
                 {"tapeName", tape},
                 {"scene", item ? item->metadata.scene : std::string()},
                 {"shot", item ? item->metadata.shot : std::string()},
                 {"take", item ? item->metadata.take : std::string()},
                 {"aiGenerated", generated},
                 {"track", tracks},
                 {"nestedIn", u.via},
                 {"recordIn", recordTc(u.record.start)},
                 {"recordOut", recordTc(u.record.end())},
                 {"sourceIn", srcIn},
                 {"sourceOut", srcOut},
                 {"duration", ticksToTimecode(u.record.duration, seq.frameRate, seq.dropFrame)},
                 {"seconds", u.record.duration.toSeconds()},
                 {"speed", u.speed},
                 {"note", note}};
        if (u.media.isValid())
            row["mediaId"] = u.media.value;
        csv += csvCell(name) + ',' + csvCell(path) + ',' + csvCell(tape) + ',' + csvCell(row["scene"].get<std::string>()) +
               ',' + csvCell(row["shot"].get<std::string>()) + ',' + csvCell(row["take"].get<std::string>()) + ',' +
               (generated ? "yes" : "") + ',' + csvCell(tracks) + ',' + csvCell(u.via) + ',' + recordTc(u.record.start) +
               ',' + recordTc(u.record.end()) + ',' + srcIn + ',' + srcOut + ',' +
               row["duration"].get<std::string>() + ',' + secondsText(u.record.duration.toSeconds()) + ',' +
               csvCell(u.speed) + ',' + csvCell(note) + '\n';
        rows.push_back(std::move(row));
        if (!u.media.isValid())
            continue;
        auto t = std::find_if(totals.begin(), totals.end(), [&](const Total& x) { return x.media == u.media; });
        if (t == totals.end()) {
            totals.push_back({u.media, 0, Ticks{}, {}});
            t = totals.end() - 1;
        }
        ++t->uses;
        t->used = t->used + u.record.duration;
        if (u.source.duration.value > 0)
            t->source.push_back(u.source);
    }
    // Per source: uses, seconds on screen, and the seconds of the source they show (overlapping uses counted once).
    Json media = Json::array();
    csv += "\nMedia Name,File Path,Tape Name,AI Generated,Uses,Used Seconds,Source Seconds\n";
    std::stable_sort(totals.begin(), totals.end(), [&](const Total& a, const Total& b) {
        const MediaItem* x = c.state().findMedia(a.media);
        const MediaItem* y = c.state().findMedia(b.media);
        return (x ? x->name : std::string()) < (y ? y->name : std::string());
    });
    for (Total& t : totals) {
        const MediaItem* item = c.state().findMedia(t.media);
        if (!item)
            continue;
        std::sort(t.source.begin(), t.source.end(), [](const TimeRange& a, const TimeRange& b) { return a.start < b.start; });
        Ticks span{};
        Ticks reach = Ticks::min();
        for (const TimeRange& r : t.source) {
            const Ticks start = std::max(r.start, reach);
            if (start < r.end())
                span = span + (r.end() - start);
            reach = std::max(reach, r.end());
        }
        const std::string path = item->locator.path.empty() ? item->info.path : item->locator.path;
        const std::string tape = item->timecode ? item->timecode->reelName : std::string();
        media.push_back(Json{{"mediaId", t.media.value},
                             {"media", item->name},
                             {"path", path},
                             {"tapeName", tape},
                             {"aiGenerated", item->generated.has_value()},
                             {"uses", t.uses},
                             {"usedSeconds", t.used.toSeconds()},
                             {"sourceSeconds", span.toSeconds()}});
        csv += csvCell(item->name) + ',' + csvCell(path) + ',' + csvCell(tape) + ',' +
               (item->generated ? "yes" : "") + ',' + std::to_string(t.uses) + ',' + secondsText(t.used.toSeconds()) +
               ',' + secondsText(span.toSeconds()) + '\n';
    }
    Json report{{"sequence", seq.name},
                {"range", Json{{"in", recordTc(range.start)}, {"out", recordTc(range.end())}, {"name", rangeName}}},
                {"flatten", flatten},
                {"uses", rows},
                {"media", media}};
    if (w.truncated)
        report["truncated"] = true;
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"format", format},
                    {"uses", rows.size()},
                    {"media", media.size()},
                    {"text", format == "json" ? report.dump(2) + "\n" : csv},
                    {"report", report}};
    if (w.truncated)
        out.data["warnings"] = Json::array({"the report stops at 100,000 uses"});
    return out;
}

} // namespace

void registerMediaUsageCommands(CommandRegistry& registry) {
    addBuiltin(registry,
               {"sequence.mediaUsage", 1, "Media Usage Report",
                "Lists every use of a source in the sequence, for archive clearances, music licensing and credits: one row "
                "per use with the media's name, path, tape name, scene / shot / take, whether it is AI-generated, the "
                "track, the record In/Out (the sequence's timecode, from its start time), the source In/Out (the media's "
                "timecode, at the clip's speed), the duration and the speed; then per source its uses, seconds on screen "
                "and seconds of the source shown. Clips of enabled video tracks and audible audio tracks, enabled clips "
                "only; a linked video and audio clip is one use. \"flatten\" (default true) reports through nested "
                "sequences (in the top sequence's record time) and multicam clips (their active angle); false lists a "
                "nested clip as one row. Time-remapped clips are reported as their whole range with the note "
                "\"remapped\". Titles, mattes and graphics are not listed. \"text\" is the CSV (or the JSON) to write; "
                "\"report\" the same as data. Nothing is written to disk.",
                Capability::Read,
                withSequence({{"range", schema::enumeration({"sequence", "inOut"},
                                                            "The whole sequence (default) or its In/Out marks.")},
                              {"format", schema::enumeration({"csv", "json"}, "The text format. Default csv.")},
                              {"flatten", schema::boolean("Report through nested and multicam clips. Default true.")}}),
                Json{{"format", "csv"}}, {},
                [](ExecutionContext& c) { return mediaUsage(c); }});
}

} // namespace wabcut::command::detail
