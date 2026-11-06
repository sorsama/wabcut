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

#include <wabcut/command/CsvText.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/GraphicOps.h>
#include <wabcut/model/Plural.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Resolve.h>
#include <wabcut/model/Rights.h>
#include <wabcut/model/Timecode.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <set>
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


/// One text cell of the report. 084: media, tape, scene / shot / take and sequence names come from files, camera
/// metadata and projects other people made, and Excel / Sheets / LibreOffice run a cell that starts with = + - @ (or a
/// tab or carriage return) as a formula (CSV injection). Such a cell gets a single quote in front, so it stays text.
std::string csvCell(const std::string& text) { return csvText(text); } // CsvText.h: shared with the marker and transcript CSVs

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

// ---- Rights and clearance (0.7, card 179) --------------------------------------------------------------------------

struct Collected {
    std::vector<Use> uses;
    bool truncated = false;
};

/// The uses of the report's walk (enabled video tracks, audible audio tracks), a linked pair of one source as one use.
Collected collectUses(ExecutionContext& c, const Sequence& seq, TimeRange range, bool flatten) {
    Walk w{c.state(), range, flatten, {}, false};
    std::vector<RecordMap> maps;
    for (const bool video : {true, false}) {
        const std::vector<TrackPtr>& all = seq.tracks(video ? TrackKind::Video : TrackKind::Audio);
        const bool anySolo = std::any_of(all.begin(), all.end(), [](const TrackPtr& t) { return t && t->solo; }); // video solo too (card 106)
        std::vector<std::size_t> tracks;
        for (std::size_t i = 0; i < all.size(); ++i) {
            const TrackPtr& t = all[i];
            if (t && t->enabled && (video || !t->muted) && (!anySolo || t->solo))
                tracks.push_back(i);
        }
        walkTracks(w, seq, video, tracks, range, maps, {}, {}, 0);
    }
    return {mergeLinked(std::move(w.uses)), w.truncated};
}

/// Today as YYYY-MM-DD (UTC): the day licences are judged on unless the request names one.
std::string todayIso() {
    const auto now = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    return isoDateFromDays(now.time_since_epoch().count());
}

const Json& asOfSchema() {
    static const Json s = schema::string("The day to judge expiry dates on, YYYY-MM-DD (usually the delivery or air date). Default today.");
    return s;
}

std::string asOfParam(ExecutionContext& c) {
    if (!c.has("asOf"))
        return todayIso();
    const std::string day = c.string("asOf");
    if (!isIsoCalendarDate(day))
        fail(ErrorCode::InvalidParams, "asOf must be a date written YYYY-MM-DD", Json{{"path", "/asOf"}});
    return day;
}

int soonDaysParam(ExecutionContext& c) { return static_cast<int>(c.integer("soonDays", 30)); }

std::string clearanceLabel(ClearanceStatus status) {
    switch (status) {
    case ClearanceStatus::Cleared:
        return "Cleared";
    case ClearanceStatus::Pending:
        return "Pending";
    case ClearanceStatus::FairUse:
        return "Fair use";
    case ClearanceStatus::NotRequired:
        return "Not required";
    case ClearanceStatus::Unset:
        break;
    }
    return {};
}

/// "1250.00 USD", "1250.00" without a currency, empty without a cost.
std::string costText(const MediaRights& r) {
    if (!r.costMinor)
        return {};
    return formatCostMinor(*r.costMinor) + (r.currency.empty() ? std::string() : " " + r.currency);
}

/// The columns Media Usage adds for a source's rights; blank without a record.
constexpr const char* kRightsColumns = "Clearance,Source,Licence,Territory,Expires,Cost,Credit Line,Rights Flags";

std::string rightsCells(const MediaRights* r, const std::string& flags) {
    if (!r)
        return ",,,,,,,";
    return csvCell(clearanceLabel(r->status)) + ',' + csvCell(r->source) + ',' + csvCell(r->licence) + ',' +
           csvCell(r->territory) + ',' + csvCell(r->expiry) + ',' + csvCell(costText(*r)) + ',' + csvCell(r->creditLine) + ',' +
           csvCell(flags);
}

Json rightsFields(const MediaRights* r, const RightsAssessment& a, const std::string& flags) {
    Json out{{"hasRecord", r != nullptr}, {"flags", flags}, {"uncleared", a.uncleared()}, {"expired", a.expired},
             {"expiresSoon", a.expiresSoon}};
    if (!r)
        return out;
    out["status"] = std::string(toString(r->status));
    out["statusLabel"] = clearanceLabel(r->status);
    out["source"] = r->source;
    out["licence"] = r->licence;
    out["reference"] = r->reference;
    out["territory"] = r->territory;
    out["expiry"] = r->expiry;
    out["cost"] = r->costMinor ? Json(formatCostMinor(*r->costMinor)) : Json(nullptr);
    out["currency"] = r->currency;
    out["creditLine"] = r->creditLine;
    out["notes"] = r->notes;
    if (isIsoCalendarDate(r->expiry))
        out["daysLeft"] = a.daysLeft;
    return out;
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
    Collected collected = collectUses(c, seq, range, flatten);
    const std::vector<Use>& uses = collected.uses;
    const std::string asOf = asOfParam(c);
    const int soonDays = soonDaysParam(c);

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
    int flaggedSources = 0; // sources whose rights are uncleared, expired or about to expire on asOf
    Json rows = Json::array();
    std::string csv = "Media Name,File Path,Tape Name,Scene,Shot,Take,AI Generated,Track,Nested In,Record In,Record Out,"
                      "Source In,Source Out,Duration,Seconds,Speed,Note," + std::string(kRightsColumns) + "\n";
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
        // 0.7 (card 179): what the source's rights record says, and whether it needs attention on `asOf`.
        const MediaRights* rights = item ? effectiveRights(c.state(), *item) : nullptr;
        const RightsAssessment assessment = rights ? assessRights(*rights, asOf, soonDays) : RightsAssessment{};
        const std::string rightsFlags = rights ? rightsFlagText(*rights, assessment) : std::string();
        row["rights"] = rightsFields(rights, assessment, rightsFlags);
        csv += csvCell(name) + ',' + csvCell(path) + ',' + csvCell(tape) + ',' + csvCell(row["scene"].get<std::string>()) +
               ',' + csvCell(row["shot"].get<std::string>()) + ',' + csvCell(row["take"].get<std::string>()) + ',' +
               (generated ? "yes" : "") + ',' + csvCell(tracks) + ',' + csvCell(u.via) + ',' + recordTc(u.record.start) +
               ',' + recordTc(u.record.end()) + ',' + srcIn + ',' + srcOut + ',' +
               row["duration"].get<std::string>() + ',' + secondsText(u.record.duration.toSeconds()) + ',' +
               csvCell(u.speed) + ',' + csvCell(note) + ',' + rightsCells(rights, rightsFlags) + '\n';
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
    csv += "\nMedia Name,File Path,Tape Name,AI Generated,Uses,Used Seconds,Source Seconds," + std::string(kRightsColumns) + "\n";
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
        const MediaRights* rights = effectiveRights(c.state(), *item);
        const RightsAssessment assessment = rights ? assessRights(*rights, asOf, soonDays) : RightsAssessment{};
        const std::string rightsFlags = rights ? rightsFlagText(*rights, assessment) : std::string();
        media.push_back(Json{{"mediaId", t.media.value},
                             {"media", item->name},
                             {"path", path},
                             {"tapeName", tape},
                             {"aiGenerated", item->generated.has_value()},
                             {"uses", t.uses},
                             {"usedSeconds", t.used.toSeconds()},
                             {"sourceSeconds", span.toSeconds()},
                             {"rights", rightsFields(rights, assessment, rightsFlags)}});
        csv += csvCell(item->name) + ',' + csvCell(path) + ',' + csvCell(tape) + ',' +
               (item->generated ? "yes" : "") + ',' + std::to_string(t.uses) + ',' + secondsText(t.used.toSeconds()) +
               ',' + secondsText(span.toSeconds()) + ',' + rightsCells(rights, rightsFlags) + '\n';
        if (assessment.needsAttention())
            ++flaggedSources;
    }
    Json report{{"sequence", seq.name},
                {"range", Json{{"in", recordTc(range.start)}, {"out", recordTc(range.end())}, {"name", rangeName}}},
                {"flatten", flatten},
                {"asOf", asOf},
                {"flaggedSources", flaggedSources},
                {"uses", rows},
                {"media", media}};
    if (collected.truncated)
        report["truncated"] = true;
    CommandOutput out;
    out.state = c.state();
    out.data = Json{{"format", format},
                    {"uses", rows.size()},
                    {"media", media.size()},
                    {"flaggedSources", flaggedSources},
                    {"text", format == "json" ? report.dump(2) + "\n" : csv},
                    {"report", report}};
    if (collected.truncated)
        out.data["warnings"] = Json::array({"the report stops at 100,000 uses"});
    if (flaggedSources > 0)
        out.warnings.push_back(counted(flaggedSources, "source") + " in this sequence " + (flaggedSources == 1 ? "needs" : "need") +
                               " rights attention on " + asOf + ": uncleared, expired or about to expire (see the Rights Flags column).");
    return out;
}

// ---- media.setRights -----------------------------------------------------------------------------------------------

Json boundedText(std::string description, std::size_t maxBytes) {
    Json s = schema::string(std::move(description));
    s["maxLength"] = static_cast<std::int64_t>(maxBytes);
    return s;
}

CommandOutput setRights(ExecutionContext& c) {
    edit::MediaRightsChange change;
    const auto text = [&](const char* key, std::optional<std::string>& into) {
        if (c.has(key))
            into = c.string(key);
    };
    text("source", change.source);
    text("licence", change.licence);
    text("reference", change.reference);
    text("territory", change.territory);
    text("expiry", change.expiry);
    text("currency", change.currency);
    text("creditLine", change.creditLine);
    text("notes", change.notes);
    if (c.has("status")) {
        const auto status = clearanceStatusFromName(c.string("status"));
        if (!status)
            fail(ErrorCode::InvalidParams, "status must be \"\", \"cleared\", \"pending\", \"fairUse\" or \"notRequired\"",
                 Json{{"path", "/status"}});
        change.status = *status;
    }
    if (c.params().contains("cost")) { // (null is a value here: it clears the cost)
        const Json& v = c.params()["cost"];
        if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) {
            change.costMinor.emplace(); // an empty amount: the cost is cleared
        } else {
            std::optional<std::int64_t> minor;
            if (v.is_number()) {
                const double hundredths = std::round(v.get<double>() * 100.0);
                if (std::isfinite(hundredths) && hundredths >= 0.0 && hundredths <= static_cast<double>(MediaRights::kMaxCostMinor))
                    minor = static_cast<std::int64_t>(hundredths);
            } else if (v.is_string()) {
                minor = parseCostMinor(v.get<std::string>());
            }
            if (!minor)
                fail(ErrorCode::InvalidParams, "cost must be an amount such as 1250 or \"1,250.00\"", Json{{"path", "/cost"}});
            change.costMinor = std::optional<std::int64_t>(*minor);
        }
    }
    if (change.isEmpty())
        fail(ErrorCode::InvalidParams, "name at least one rights field to set (status, source, licence, reference, territory, "
                                       "expiry, cost, currency, creditLine, notes)");
    const std::vector<MediaId> media = c.mediaList("media");
    ProjectState next = c.take(edit::setMediaRights(c.state(), media, change));
    int updated = 0;
    for (const MediaId id : media) {
        if (next.findMedia(id) != c.state().findMedia(id))
            ++updated;
    }
    CommandOutput out;
    out.state = std::move(next);
    out.data = Json{{"updated", updated}};
    return out;
}

// ---- project.rightsLog ---------------------------------------------------------------------------------------------

struct LogRow {
    const MediaItem* item = nullptr;
    const MediaRights* rights = nullptr; ///< the record that stands for the item (its own or its master's)
    RightsAssessment assessment;
    std::string flags;
    std::size_t uses = 0;
    Ticks used;
    std::optional<Ticks> firstUse; ///< sequence time
};

CommandOutput rightsLog(ExecutionContext& c) {
    const std::string scope = c.string("scope", "sequence");
    const std::string format = c.string("format", "csv");
    const std::string asOf = asOfParam(c);
    const int soonDays = soonDaysParam(c);
    const bool flaggedOnly = c.boolean("flaggedOnly", false);
    const bool includeUnrecorded = c.boolean("includeUnrecorded", true);
    const ProjectState& state = c.state();
    const Sequence* seq = scope == "sequence" ? &c.sequence() : nullptr;

    std::vector<LogRow> rows;
    bool truncated = false;
    if (seq) {
        TimeRange range{Ticks{}, std::max(seq->duration, seq->contentEnd())};
        if (c.string("range", "sequence") == "inOut") {
            const auto marked = seq->markedRange();
            if (!marked)
                fail(ErrorCode::InvalidParams, "the sequence has no In and Out marks", Json{{"path", "/range"}});
            range = *marked;
        }
        const Collected collected = collectUses(c, *seq, range, true);
        truncated = collected.truncated;
        std::map<std::uint64_t, std::size_t> at;
        for (const Use& u : collected.uses) {
            const MediaItem* item = u.media.isValid() ? state.findMedia(u.media) : nullptr;
            if (!item)
                continue;
            const auto [it, added] = at.try_emplace(u.media.value, rows.size());
            if (added) {
                rows.push_back({});
                rows.back().item = item;
            }
            LogRow& row = rows[it->second];
            ++row.uses;
            row.used = row.used + u.record.duration;
            if (!row.firstUse || u.record.start < *row.firstUse)
                row.firstUse = u.record.start;
        }
    } else if (scope == "project") {
        for (const MediaPtr& m : state.media) {
            if (m && !m->isGenerator() && !m->isSubclip()) {
                rows.push_back({});
                rows.back().item = m.get();
            }
        }
        std::stable_sort(rows.begin(), rows.end(), [](const LogRow& a, const LogRow& b) { return a.item->name < b.item->name; });
    } else {
        fail(ErrorCode::InvalidParams, "scope must be \"sequence\" or \"project\"", Json{{"path", "/scope"}});
    }
    for (LogRow& row : rows) {
        row.rights = effectiveRights(state, *row.item);
        row.assessment = row.rights ? assessRights(*row.rights, asOf, soonDays) : RightsAssessment{};
        row.flags = row.rights ? rightsFlagText(*row.rights, row.assessment) : std::string();
    }
    std::erase_if(rows, [&](const LogRow& r) {
        if (!r.rights)
            return !includeUnrecorded || flaggedOnly;
        return flaggedOnly && !r.assessment.needsAttention();
    });

    const auto recordTc = [&](Ticks t) {
        return seq ? ticksToTimecode(seq->startTimecode + t, seq->frameRate, seq->dropFrame) : std::string();
    };
    Json json = Json::array();
    std::string csv = "Media Name,File Path,Clearance,Source,Licence,Reference,Territory,Expires,Cost,Currency,Credit Line,Notes,"
                      "Uses,First Used,Seconds Used,Flags\n";
    std::map<std::string, std::size_t> byStatus; // status label -> items
    std::size_t withRecord = 0;
    std::size_t withoutRecord = 0;
    std::size_t flagged = 0;
    std::size_t uncleared = 0;
    std::size_t expired = 0;
    std::size_t expiresSoon = 0;
    std::map<std::string, std::pair<std::int64_t, std::size_t>> costs; // currency -> (total minor, items)
    std::set<const MediaRights*> costCounted; // a master and its subclips share one record: its cost counts once
    for (const LogRow& row : rows) {
        const MediaItem& item = *row.item;
        const std::string path = item.locator.path.empty() ? item.info.path : item.locator.path;
        Json j = rightsFields(row.rights, row.assessment, row.rights ? row.flags : "No rights record");
        j["mediaId"] = item.id.value;
        j["media"] = item.name;
        j["path"] = path;
        j["inherited"] = row.rights && !(item.rights && !item.rights->isEmpty());
        j["uses"] = row.uses;
        j["usedSeconds"] = row.used.toSeconds();
        j["firstUse"] = row.firstUse ? recordTc(*row.firstUse) : std::string();
        if (row.rights) {
            ++withRecord;
            ++byStatus[row.rights->status == ClearanceStatus::Unset ? "Status not set" : clearanceLabel(row.rights->status)];
            if (row.assessment.needsAttention())
                ++flagged;
            uncleared += row.assessment.uncleared() ? 1 : 0;
            expired += row.assessment.expired ? 1 : 0;
            expiresSoon += row.assessment.expiresSoon ? 1 : 0;
            if (row.rights->costMinor && costCounted.insert(row.rights).second) {
                auto& total = costs[row.rights->currency];
                total.first += *row.rights->costMinor;
                ++total.second;
            }
        } else {
            ++withoutRecord;
        }
        const MediaRights none;
        const MediaRights& r = row.rights ? *row.rights : none;
        csv += csvCell(item.name) + ',' + csvCell(path) + ',' + csvCell(clearanceLabel(r.status)) + ',' + csvCell(r.source) + ',' +
               csvCell(r.licence) + ',' + csvCell(r.reference) + ',' + csvCell(r.territory) + ',' + csvCell(r.expiry) + ',' +
               (r.costMinor ? formatCostMinor(*r.costMinor) : std::string()) + ',' + csvCell(r.currency) + ',' +
               csvCell(r.creditLine) + ',' + csvCell(r.notes) + ',' + (seq ? std::to_string(row.uses) : std::string()) + ',' +
               (row.firstUse ? recordTc(*row.firstUse) : std::string()) + ',' +
               (seq ? secondsText(row.used.toSeconds()) : std::string()) + ',' + csvCell(j["flags"].get<std::string>()) + '\n';
        json.push_back(std::move(j));
    }
    Json statuses = Json::object();
    csv += "\nSummary,Items\n";
    for (const char* label : {"Cleared", "Pending", "Fair use", "Not required", "Status not set"}) {
        const std::size_t n = byStatus.contains(label) ? byStatus[label] : 0;
        statuses[label] = n;
        csv += std::string(label) + ',' + std::to_string(n) + '\n';
    }
    csv += "No rights record," + std::to_string(withoutRecord) + "\nNeeds attention on " + asOf + "," + std::to_string(flagged) + '\n';
    Json costsJson = Json::array();
    if (!costs.empty())
        csv += "\nCurrency,Total Cost,Items\n";
    for (const auto& [currency, total] : costs) {
        costsJson.push_back(Json{{"currency", currency}, {"total", formatCostMinor(total.first)}, {"items", total.second}});
        csv += csvCell(currency) + ',' + formatCostMinor(total.first) + ',' + std::to_string(total.second) + '\n';
    }
    Json totals{{"items", rows.size()},         {"withRecord", withRecord}, {"withoutRecord", withoutRecord},
                {"needAttention", flagged},     {"uncleared", uncleared},   {"expired", expired},
                {"expiresSoon", expiresSoon},   {"statuses", statuses},     {"costs", costsJson}};
    Json report{{"scope", scope}, {"asOf", asOf}, {"soonDays", soonDays}, {"rows", json}, {"totals", totals}};
    if (seq)
        report["sequence"] = seq->name;
    if (truncated)
        report["truncated"] = true;
    CommandOutput out;
    out.state = state;
    out.data = Json{{"format", format},
                    {"rows", rows.size()},
                    {"needAttention", flagged},
                    {"text", format == "json" ? report.dump(2) + "\n" : csv},
                    {"report", report}};
    if (flagged > 0)
        out.warnings.push_back(counted(static_cast<std::int64_t>(flagged), "source") + (flagged == 1 ? " needs" : " need") +
                               " rights attention on " + asOf + ": uncleared, expired or about to expire.");
    return out;
}

// ---- sequence.creditsRoll ------------------------------------------------------------------------------------------

/// ASCII lower-case, trimmed: two credit lines that differ only in case or spacing are one credit.
std::string creditKey(const std::string& line) {
    std::string key;
    bool space = false;
    for (const char ch : line) {
        if (ch == ' ' || ch == '\t') {
            space = !key.empty();
            continue;
        }
        if (space)
            key += ' ';
        space = false;
        key += (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    }
    return key;
}

CommandOutput creditsRoll(ExecutionContext& c) {
    const Sequence& seq = c.sequence();
    const ProjectState& state = c.state();
    TimeRange range{Ticks{}, std::max(seq.duration, seq.contentEnd())};
    if (c.string("range", "sequence") == "inOut") {
        const auto marked = seq.markedRange();
        if (!marked)
            fail(ErrorCode::InvalidParams, "the sequence has no In and Out marks", Json{{"path", "/range"}});
        range = *marked;
    }
    const Collected collected = collectUses(c, seq, range, true);
    const bool alphabetical = c.string("order", "appearance") == "alphabetical";
    const bool skipNotRequired = c.boolean("skipNotRequired", true);

    std::vector<std::string> lines;
    std::set<std::string> seen;
    std::set<std::uint64_t> sources;
    Json missing = Json::array(); // used sources with a rights record but no credit line
    std::set<std::uint64_t> missingSeen;
    for (const Use& u : collected.uses) {
        const MediaItem* item = u.media.isValid() ? state.findMedia(u.media) : nullptr;
        if (!item || !sources.insert(u.media.value).second)
            continue;
        const MediaRights* rights = effectiveRights(state, *item);
        if (!rights)
            continue;
        if (rights->creditLine.empty()) {
            if (rights->status != ClearanceStatus::NotRequired && missingSeen.insert(u.media.value).second)
                missing.push_back(Json{{"mediaId", u.media.value}, {"media", item->name}});
            continue;
        }
        if (skipNotRequired && rights->status == ClearanceStatus::NotRequired && rights->licence.empty() && rights->source.empty())
            continue;
        if (seen.insert(creditKey(rights->creditLine)).second)
            lines.push_back(rights->creditLine);
    }
    if (lines.empty())
        fail(ErrorCode::EditRejected, "No source used in this sequence has a credit line: add the credit lines in the Rights and "
                                      "Clearance Log first.");
    if (alphabetical)
        std::sort(lines.begin(), lines.end(), [](const std::string& a, const std::string& b) { return creditKey(a) < creditKey(b); });

    // One text layer: the heading, then a line per credit. A layer holds 16 KiB of text.
    const std::string heading = c.string("heading", "Archive and Licensed Material");
    std::string body;
    std::size_t kept = 0;
    for (const std::string& line : lines) {
        if (heading.size() + body.size() + line.size() + 8 > kMaxGraphicTextBytes)
            break;
        body += (body.empty() ? "" : "\n") + line;
        ++kept;
    }
    Graphic g;
    g.name = c.string("name", "Credits");
    g.width = seq.width;
    g.height = seq.height;
    GraphicLayer layer;
    layer.name = "Credits";
    layer.kind = LayerKind::Text;
    TextStyle headingStyle;
    headingStyle.fontSize = std::max(24.0, seq.height * 0.055);
    headingStyle.fontWeight = 700;
    TextStyle bodyStyle;
    bodyStyle.fontSize = std::max(18.0, seq.height * 0.04);
    bodyStyle.leading = bodyStyle.fontSize * 1.5;
    layer.text.align = TextAlign::Center;
    layer.text.boxWidth = seq.width * 0.7;
    if (!heading.empty())
        layer.text.runs.push_back({heading + "\n\n", headingStyle});
    layer.text.runs.push_back({body, bodyStyle});
    g.layers.push_back(std::move(layer));
    GraphicRoll roll;
    roll.kind = RollKind::Roll;
    g.roll = roll;

    const Ticks at = c.optionalTime("at").value_or(std::max(seq.duration, seq.contentEnd()));
    const Ticks duration = c.optionalTime("duration").value_or(Ticks{});
    const double seconds = std::clamp(4.0 + 1.6 * static_cast<double>(kept) + (heading.empty() ? 0.0 : 3.0), 8.0, 600.0);
    const Ticks length = duration.value > 0 ? duration : Ticks{static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)))};
    ProjectState next = state;
    const TrackId track = videoPlacementTrack(c, next, at, length);
    edit::EditResult r = c.take(edit::addGraphic(next, c.sequenceId(), track, at, length, std::move(g)));
    Json creditsJson = Json::array();
    for (std::size_t i = 0; i < kept; ++i)
        creditsJson.push_back(lines[i]);
    Json data{{"clip", r.placed.front().value}, {"credits", creditsJson}, {"missingCreditLine", missing}, {"duration", length.toSeconds()}};
    CommandOutput out = edited(std::move(r), std::move(data));
    if (kept < lines.size())
        out.warnings.push_back(std::to_string(lines.size() - kept) + " more credit lines did not fit the roll (16 KiB of text).");
    if (!missing.empty())
        out.warnings.push_back(counted(static_cast<std::int64_t>(missing.size()), "source") + (missing.size() == 1 ? " has" : " have") +
                               " a rights record but no credit line.");
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
                "\"report\" the same as data. Nothing is written to disk. (0.7) Every row and source also carries its "
                "rights and clearance record (media.setRights): clearance, source, licence, territory, expiry, cost and "
                "credit line, and \"Rights Flags\" - uncleared material, a licence that has expired or expires within "
                "\"soonDays\" of \"asOf\" - and the command warns when any source needs attention.",
                Capability::Read,
                withSequence({{"range", schema::enumeration({"sequence", "inOut"},
                                                            "The whole sequence (default) or its In/Out marks.")},
                              {"format", schema::enumeration({"csv", "json"}, "The text format. Default csv.")},
                              {"flatten", schema::boolean("Report through nested and multicam clips. Default true.")},
                              {"asOf", asOfSchema()},
                              {"soonDays", schema::integer("Flag a licence that expires within this many days of asOf (0 = never). "
                                                           "Default 30.", 0, 3650)}}),
                Json{{"format", "csv"}}, {},
                [](ExecutionContext& c) { return mediaUsage(c); }});

    // ---- Rights and clearance (0.7, card 179) ----
    addBuiltin(registry,
               {"media.setRights", 1, "Set Rights and Clearance",
                "Sets the rights and clearance record of media items (the Rights and Clearance Log): where the footage "
                "came from (\"source\"), its \"licence\" terms and \"reference\" number, the \"territory\" it may be shown "
                "in, the day it \"expiry\" (YYYY-MM-DD), what it cost (\"cost\" and a three-letter \"currency\"), the "
                "\"creditLine\" as it must appear in the credits, free \"notes\" and its \"status\": cleared, pending, "
                "fairUse or notRequired (\"\" = not said). Only the fields given change; an empty text clears one. A "
                "subclip without a record of its own uses its master's. Titles and graphics are refused. One undo step "
                "for all the items.",
                Capability::EditProject,
                withSequence({{"media", schema::array(mediaParam("A media item."), "The media items.", 1)},
                              {"status", schema::enumeration({"", "cleared", "pending", "fairUse", "notRequired"},
                                                             "Where the clearance stands.")},
                              {"source", boundedText("Who supplied or owns it.", MediaRights::kMaxFieldBytes)},
                              {"licence", boundedText("The licence terms.", MediaRights::kMaxFieldBytes)},
                              {"reference", boundedText("The licence, asset or invoice number.", MediaRights::kMaxFieldBytes)},
                              {"territory", boundedText("Where it may be shown.", MediaRights::kMaxFieldBytes)},
                              {"expiry", boundedText("The last day it may be shown, YYYY-MM-DD; empty = none.", 10)},
                              {"cost", Json{{"type", Json::array({"string", "number", "null"})},
                                            {"description", "What it cost, as an amount (1250, \"1,250.00\"); empty or null clears it."}}},
                              {"currency", boundedText("Three capital letters (USD, EUR, GBP).", 3)},
                              {"creditLine", boundedText("As it must appear in the credits.", MediaRights::kMaxFieldBytes)},
                              {"notes", boundedText("Free notes.", MediaRights::kMaxNotesBytes)}},
                             {"media"}),
                Json{{"media", Json::array({4, 5})}, {"status", "cleared"}, {"source", "Newsreel Archive"},
                     {"creditLine", "Archive footage courtesy of Newsreel Archive"}, {"cost", "250.00"}, {"currency", "GBP"},
                     {"expiry", "2028-06-30"}},
                {}, [](ExecutionContext& c) { return setRights(c); }});
    addBuiltin(registry,
               {"project.rightsLog", 1, "Rights and Clearance Log",
                "The clearance report documentary makers hand to legal: one row per source used in the sequence (\"scope\" "
                "sequence, the default, in the order each first appears) or per file in the project (\"project\"), with its "
                "clearance status, source, licence, reference, territory, expiry, cost, credit line and notes, how often and "
                "where it is used, and flags - uncleared (pending, or never said), expired on \"asOf\" or expiring within "
                "\"soonDays\" - and a summary: items per status, sources without a record, sources that need attention and "
                "the total cost per currency (a master and its subclips count once). \"flaggedOnly\" keeps the rows that "
                "need attention; \"includeUnrecorded\" (default true) keeps sources with no rights record. \"text\" is the "
                "CSV (or the JSON) to write; \"report\" the same as data. Nothing is written to disk.",
                Capability::Read,
                withSequence({{"scope", schema::enumeration({"sequence", "project"}, "Sources used in the sequence (default) or every file.")},
                              {"range", schema::enumeration({"sequence", "inOut"}, "For scope sequence: the whole sequence (default) or its In/Out marks.")},
                              {"format", schema::enumeration({"csv", "json"}, "The text format. Default csv.")},
                              {"asOf", asOfSchema()},
                              {"soonDays", schema::integer("Flag a licence that expires within this many days of asOf (0 = never). "
                                                           "Default 30.", 0, 3650)},
                              {"flaggedOnly", schema::boolean("Only the rows that need attention.")},
                              {"includeUnrecorded", schema::boolean("Keep sources with no rights record. Default true.")}}),
                Json{{"asOf", "2026-12-01"}}, {},
                [](ExecutionContext& c) { return rightsLog(c); }});
    addBuiltin(registry,
               {"sequence.creditsRoll", 1, "Credits Roll from Rights Log",
                "Makes an end-credits roll from the credit lines of the sources the sequence uses: a rolling graphic clip "
                "(Essential Graphics Roll) holding \"heading\" and one line per distinct credit, in the order the sources "
                "first appear (or \"alphabetical\"), starting at \"at\" (default the end of the sequence's content). Sources "
                "with a record but no credit line are reported under \"missingCreditLine\". The length sets the speed: "
                "default about 1.6 s a line. Edit the result like any graphic (graphic.get, graphic.update). One undo step.",
                Capability::EditSequence,
                withSequence({{"at", timeParam("Where the roll starts. Default: the end of the sequence's content.")},
                              {"duration", timeParam("How long it lasts. Default about 1.6 seconds a line, at least 8 seconds.")},
                              {"track", trackParam("A video track. Default: the first free track above the picture.")},
                              {"placement", schema::enumeration({"targeted", "freeTrackAtOrAbove", "newTrack"},
                                                                "As graphic.add. Default freeTrackAtOrAbove.")},
                              {"heading", boundedText("The roll's heading; empty for none. Default \"Archive and Licensed Material\".", 200)},
                              {"name", boundedText("The graphic's name (also the clip's). Default Credits.", 200)},
                              {"order", schema::enumeration({"appearance", "alphabetical"}, "Default appearance.")},
                              {"range", schema::enumeration({"sequence", "inOut"}, "The whole sequence (default) or its In/Out marks.")},
                              {"skipNotRequired", schema::boolean("Leave out sources marked not required that name no source or "
                                                                  "licence. Default true.")}}),
                Json{{"heading", "Archive Footage"}}, {}, [](ExecutionContext& c) { return creditsRoll(c); }});
}

} // namespace wabcut::command::detail
