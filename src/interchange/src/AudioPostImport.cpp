// AAF / OMF import (AudioPostImport.h): the mob graph -> IrDocument engine both readers share.

#include "AudioPostImport.h"

#include "AudioPost.h"
#include "ImporterCommon.h"
#include "TextUtil.h"
#include "TimeMath.h"

#include <wabcut/interchange/FileUrl.h>
#include <wabcut/model/AudioChannels.h>
#include <wabcut/model/Parameters.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <tuple>
#include <unordered_set>

namespace wabcut::interchange::detail::graph {

namespace {

namespace fs = std::filesystem;

constexpr int kMaxDepth = 32;
constexpr std::size_t kMaxPieces = 500'000;          ///< clips in a document
constexpr std::size_t kMaxVisits = 5'000'000;        ///< components walked (flattened compositions repeat)
constexpr std::size_t kMaxAutomationPoints = 10'000; ///< keyframes of one parameter
constexpr std::size_t kMaxSequences = 64;
/// Positions beyond a week are nonsense (or hostile): what lies there is reported and left out.
constexpr std::int64_t kMaxTimelineTicks = 7LL * 24 * 3600 * kTicksPerSecond;

std::string lowerAscii(std::string_view s) { return toLowerAscii(s); }

double clampDouble(double v, double lo, double hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : lo; }

/// dB of an amplitude multiplier, in the model's volume range.
double decibels(double amplitude) {
    const ParameterInfo& info = parameterInfo(ParamId::VolumeDb);
    if (!std::isfinite(amplitude) || amplitude <= 0.0) {
        return info.minValue;
    }
    return clampDouble(20.0 * std::log10(amplitude), info.minValue, info.maxValue);
}

/// Values over timeline time: a constant, or points (linear between, held beyond the ends).
struct Automation {
    double constant = 1.0;
    std::vector<std::pair<Ticks, double>> points;

    bool varying() const { return !points.empty(); }
    double at(Ticks t) const {
        if (points.empty()) {
            return constant;
        }
        if (t <= points.front().first) {
            return points.front().second;
        }
        if (t >= points.back().first) {
            return points.back().second;
        }
        const auto next = std::upper_bound(points.begin(), points.end(), t,
                                           [](Ticks x, const std::pair<Ticks, double>& p) { return x < p.first; });
        const auto prev = next - 1;
        const double span = static_cast<double>((next->first - prev->first).value);
        const double f = span > 0 ? static_cast<double>((t - prev->first).value) / span : 0.0;
        return prev->second + (next->second - prev->second) * f;
    }
};

/// A Control of a component that starts at `pos` and lasts `length`, in timeline time.
Automation automationOf(const Control& c, Ticks pos, Ticks length) {
    Automation a;
    a.constant = c.constant;
    for (const auto& [fraction, value] : c.points) {
        if (a.points.size() >= kMaxAutomationPoints || !std::isfinite(fraction) || !std::isfinite(value)) {
            continue;
        }
        const double f = std::clamp(fraction, 0.0, 1.0);
        const Ticks t = pos + Ticks{static_cast<std::int64_t>(std::llround(static_cast<double>(length.value) * f))};
        if (!a.points.empty() && t <= a.points.back().first) {
            if (t == a.points.back().first) {
                a.points.back().second = value; // the later point at the same time wins
            }
            continue;
        }
        a.points.emplace_back(t, value);
    }
    return a;
}

struct Source {
    std::size_t media = kNoMedia;
    Ticks sourceIn;
    int channel = -1;   ///< the file channel played (channel combiners); -1: the file as it is
    std::string master; ///< the master mob it came through (links)
    std::string name;
    int speaker = -1;   ///< from the master / file name
    std::int32_t fileChannels = 0;
};

struct Piece {
    Ticks start;
    Ticks duration;
    std::vector<Source> sources; ///< several: a channel combiner
    std::vector<Automation> gains; ///< amplitude multipliers, multiplied
    std::optional<Automation> pan; ///< 0 = left .. 1 = right
    std::string name;
    std::string where;
    bool enabled = true;
    Ticks end() const { return start + duration; }
};

struct TrackOut {
    std::vector<Piece> pieces;
    std::vector<IrTransition> transitions;
};

/// What the walk carries down: the clip gains and pan of enclosing operation groups.
struct Context {
    std::vector<Automation> gains;
    std::optional<Automation> pan;
    bool combiner = false; ///< inside a channel combiner: a multichannel file's slot names the channel played
};

/// "-6.0", "+3.5"
std::string dbLabel(double db) {
    char text[32];
    std::snprintf(text, sizeof text, "%+.1f", db);
    return text;
}

std::string suffixOfSpeaker(int speaker) {
    static constexpr std::string_view kNames[6] = {"L", "R", "C", "LFE", "Ls", "Rs"};
    return speaker >= 0 && speaker < 6 ? std::string(kNames[speaker]) : std::string();
}

class Engine {
public:
    Engine(Graph& graph, const ParseContext& context, const BuildOptions& options)
        : m_graph(graph), m_ctx(context), m_opt(options), m_media(m_doc) {}

    std::expected<IrDocument, Error> run();

private:
    SourceLocation loc(const std::string& where) const { return {m_ctx.fileLabel, where, 0}; }
    void report(ReportOutcome outcome, ReportCategory category, std::string reason, const std::string& where) {
        m_doc.report.add(outcome, category, std::move(reason), loc(where), m_seqName);
    }
    std::string tc(Ticks t) const { return timecodeLabel(t + m_startTc, m_rate, m_dropFrame); }

    std::optional<Ticks> units(std::int64_t n, Rate rate) const {
        const auto t = framesToTicksChecked(n, rate);
        if (!t || std::abs(t->value) > kMaxTimelineTicks) {
            return std::nullopt;
        }
        return t;
    }

    std::vector<const Mob*> chooseCompositions() const;
    std::expected<void, Error> buildSequence(const Mob& comp);
    void walk(const Component& c, Ticks pos, Rate rate, TrackOut& out, const Context& cx, int depth, TrackKind kind);
    void walkSequence(const Component& c, Ticks pos, Rate rate, TrackOut& out, const Context& cx, int depth, TrackKind kind);
    void walkSourceClip(const Component& c, Ticks pos, Ticks length, Rate rate, TrackOut& out, const Context& cx,
                        int depth, TrackKind kind);
    std::optional<Source> resolve(const std::string& key, std::uint32_t slotId, Ticks position, int depth, bool combiner,
                                  std::string& why);
    std::size_t mediaFor(const Mob& file, const Mob* master);
    std::optional<fs::path> extract(const Mob& file, const std::string& name, std::string& problem);
    std::vector<IrTrack> makeTracks(const Slot& slot, TrackOut& out, TrackKind kind, std::vector<Automation> trackGains,
                                    std::optional<Automation> trackPan, IrSequence& seq);
    IrClip clipOf(const Piece& p, const Source& s, TrackKind kind);
    std::optional<ClipProperties> propertiesOf(const Piece& p, const Source& s, const std::string& clipName);
    void routeSurroundGroups(IrSequence& seq);
    void linkByMaster(IrSequence& seq);
    void addMarkers(const Slot& slot, IrSequence& seq);

    Graph& m_graph;
    const ParseContext& m_ctx;
    BuildOptions m_opt;
    IrDocument m_doc;
    MediaTable m_media;
    std::int64_t m_nextLink = 0;
    std::size_t m_visits = 0;
    std::size_t m_pieces = 0;
    bool m_overBudget = false;
    std::unordered_map<std::string, std::size_t> m_mediaOfFile; ///< file mob key -> media index
    std::unordered_map<std::string, fs::path> m_extracted;      ///< file mob key -> extracted file
    std::set<std::string> m_usedNames;                          ///< extracted file names this import
    // Fix round 1 (ARGON-08): progress over the embedded media written, and Cancel (ImportOptions::progress).
    std::uint64_t m_essenceTotal = 0;
    std::uint64_t m_essenceDone = 0;
    std::uint64_t m_essenceReported = 0;
    bool m_cancelled = false;
    std::vector<fs::path> m_createdFiles; ///< written by this import (removed again on Cancel)
    bool m_createdFolder = false;
    bool reportProgress(bool force);
    void undoExtraction();
    // Per sequence.
    std::string m_seqName;
    Rate m_rate = kRate30;
    bool m_dropFrame = false;
    Ticks m_startTc;
};

std::vector<const Mob*> Engine::chooseCompositions() const {
    std::vector<const Mob*> top;
    std::vector<const Mob*> all;
    for (const Mob& m : m_graph.mobs) {
        if (m.kind != Mob::Kind::Composition) {
            continue;
        }
        all.push_back(&m);
        if (m.topLevel) {
            top.push_back(&m);
        }
    }
    if (!top.empty()) {
        return top;
    }
    // No usage codes: the compositions no other composition plays (and that do not call themselves subclips).
    std::unordered_set<std::string> referenced;
    std::vector<const Component*> todo;
    for (const Mob* m : all) {
        for (const Slot& s : m->slots) {
            todo.push_back(&s.segment);
        }
    }
    std::size_t guard = 0;
    while (!todo.empty() && ++guard < kMaxVisits) {
        const Component* c = todo.back();
        todo.pop_back();
        if (c->kind == Component::Kind::SourceClip && !c->sourceId.empty()) {
            referenced.insert(c->sourceId);
        }
        for (const Component& child : c->children) {
            todo.push_back(&child);
        }
    }
    for (const Mob* m : all) {
        if (!referenced.contains(m->key) && !m->lowerLevel) {
            top.push_back(m);
        }
    }
    if (top.empty()) {
        for (const Mob* m : all) {
            if (!referenced.contains(m->key)) {
                top.push_back(m);
            }
        }
    }
    return top;
}

bool Engine::reportProgress(bool force) {
    const auto* options = m_ctx.options;
    if (m_cancelled || !options || !options->progress) {
        return !m_cancelled;
    }
    // Every 4 MiB written (and at the end of each file): cheap enough, and Cancel answers within a moment.
    constexpr std::uint64_t kStep = 4ull << 20;
    if (!force && m_essenceDone - m_essenceReported < kStep) {
        return true;
    }
    m_essenceReported = m_essenceDone;
    const double share = m_essenceTotal == 0 ? 1.0
                                             : std::min(1.0, static_cast<double>(m_essenceDone) /
                                                                 static_cast<double>(m_essenceTotal));
    if (!options->progress(share)) {
        m_cancelled = true;
    }
    return !m_cancelled;
}

void Engine::undoExtraction() {
    std::error_code ec;
    for (const fs::path& f : m_createdFiles) {
        fs::remove(f, ec);
    }
    m_createdFiles.clear();
    if (m_createdFolder) {
        fs::remove(m_ctx.mediaFolder, ec); // only when empty: never anything that was there before
    }
}

std::expected<IrDocument, Error> Engine::run() {
    if (!m_ctx.mediaFolder.empty()) {
        for (const Mob& m : m_graph.mobs) {
            if (m.kind == Mob::Kind::File && m.essence) {
                m_essenceTotal += m.essence->header.size() + m.essence->size;
            }
        }
    }
    if (!reportProgress(true)) {
        return std::unexpected(Error{ErrorCode::Cancelled, "Import cancelled"});
    }
    std::vector<const Mob*> compositions = chooseCompositions();
    if (compositions.empty()) {
        return std::unexpected(Error{ErrorCode::ParseError,
                                     std::string(displayName(m_opt.format)) +
                                         ": the file holds no composition (sequence) to import, only media"});
    }
    if (compositions.size() > kMaxSequences) {
        m_doc.report.add(ReportOutcome::Dropped, ReportCategory::SequenceSetting,
                         std::to_string(compositions.size() - kMaxSequences) + " compositions past the first " +
                             std::to_string(kMaxSequences) + " were not imported",
                         loc({}));
        compositions.resize(kMaxSequences);
    }
    for (const Mob* comp : compositions) {
        if (auto built = buildSequence(*comp); !built) {
            return std::unexpected(built.error());
        }
        if (m_cancelled) {
            break;
        }
    }
    if (m_cancelled) {
        undoExtraction();
        return std::unexpected(Error{ErrorCode::Cancelled, "Import cancelled"});
    }
    if (m_overBudget) {
        m_doc.report.add(ReportOutcome::Dropped, ReportCategory::Clip,
                         "the file is too large or nests too deeply: clips past the import's limits were left out", loc({}));
    }
    ImportReport combined = std::move(m_graph.report);
    combined.append(m_doc.report);
    m_doc.report = std::move(combined);
    return std::move(m_doc);
}

// --- Walking a track ---------------------------------------------------------------------------------------------

void Engine::walk(const Component& c, Ticks pos, Rate rate, TrackOut& out, const Context& cx, int depth, TrackKind kind) {
    if (depth > kMaxDepth || ++m_visits > kMaxVisits || m_pieces > kMaxPieces) {
        m_overBudget = true;
        return;
    }
    const auto length = units(c.length, rate);
    if (!length || length->value < 0) {
        report(ReportOutcome::Dropped, ReportCategory::Clip, "a component with a length out of range was left out", c.where);
        return;
    }
    using K = Component::Kind;
    switch (c.kind) {
    case K::Filler:
    case K::Timecode:
    case K::Marker:
        return;
    case K::Sequence:
        walkSequence(c, pos, rate, out, cx, depth, kind);
        return;
    case K::SourceClip:
        walkSourceClip(c, pos, *length, rate, out, cx, depth, kind);
        return;
    case K::Gain:
    case K::Pan: {
        if (c.children.empty()) {
            return;
        }
        Context inner = cx;
        if (c.kind == K::Gain) {
            for (const Control& control : c.controls) {
                inner.gains.push_back(automationOf(control, pos, *length));
            }
        } else if (!c.controls.empty()) {
            inner.pan = automationOf(c.controls.front(), pos, *length);
        }
        walk(c.children.front(), pos, rate, out, inner, depth + 1, kind);
        return;
    }
    case K::Combiner: {
        // One clip of every channel: each input is a source clip (perhaps inside a gain) over the same time.
        TrackOut channels;
        Context inner = cx;
        inner.combiner = true;
        for (const Component& input : c.children) {
            walk(input, pos, rate, channels, inner, depth + 1, kind);
        }
        if (channels.pieces.empty()) {
            return;
        }
        Piece merged = channels.pieces.front();
        bool aligned = true;
        for (std::size_t k = 1; k < channels.pieces.size(); ++k) {
            const Piece& p = channels.pieces[k];
            if (p.start != merged.start || p.duration != merged.duration || p.sources.size() != 1) {
                aligned = false;
                break;
            }
            merged.sources.push_back(p.sources.front());
        }
        if (!aligned || merged.sources.size() != channels.pieces.size()) {
            report(ReportOutcome::Approximated, ReportCategory::AudioMapping,
                   "a multichannel clip at " + tc(pos) + " has channels of different lengths: each is imported as its own clip",
                   c.where);
            for (Piece& p : channels.pieces) {
                out.pieces.push_back(std::move(p));
            }
        } else {
            out.pieces.push_back(std::move(merged));
        }
        for (IrTransition& t : channels.transitions) {
            out.transitions.push_back(std::move(t));
        }
        return;
    }
    case K::Selector:
        if (c.children.size() >= 2 && c.children.front().kind == Component::Kind::Filler) {
            // Silence selected over a clip: how Media Composer mutes a clip. The clip comes in switched off.
            const std::size_t first = out.pieces.size();
            walk(c.children[1], pos, rate, out, cx, depth + 1, kind);
            for (std::size_t i = first; i < out.pieces.size(); ++i) {
                out.pieces[i].enabled = false;
                report(ReportOutcome::Carried, ReportCategory::Clip,
                       "muted clip '" + out.pieces[i].name + "' at " + tc(out.pieces[i].start) + " (switched off)",
                       c.where);
            }
            return;
        }
        if (!c.children.empty()) {
            if (c.alternates > 0) {
                report(ReportOutcome::Approximated, ReportCategory::Clip,
                       "a " + (c.name.empty() ? std::string("selector") : c.name) + " at " + tc(pos) + " has " +
                           std::to_string(c.alternates) + " alternative(s): the selected one is imported",
                       c.where);
            }
            walk(c.children.front(), pos, rate, out, cx, depth + 1, kind);
        }
        return;
    case K::Effect: {
        const std::string name = c.name.empty() ? std::string("an effect") : "'" + c.name + "'";
        if (c.children.empty()) {
            report(ReportOutcome::Dropped, ReportCategory::Effect, name + " at " + tc(pos) + " (" + framesLabel(*length, m_rate) +
                                                                      ") has no clip of its own: not imported",
                   c.where);
            return;
        }
        const std::size_t first = out.pieces.size();
        walk(c.children.front(), pos, rate, out, cx, depth + 1, kind);
        if (c.timeWarp) {
            // A speed change: its input runs at another rate. The clip is kept at 100 % over the effect's time.
            for (std::size_t i = first; i < out.pieces.size(); ++i) {
                Piece& p = out.pieces[i];
                p.start = std::max(p.start, pos);
                p.duration = std::min(p.end(), pos + *length) - p.start;
            }
            report(ReportOutcome::Approximated, ReportCategory::Speed,
                   name + " at " + tc(pos) + " changes the clip's speed: imported at 100%", c.where);
        } else {
            report(ReportOutcome::Dropped, ReportCategory::Effect,
                   name + " at " + tc(pos) + " is not imported (its clip is)", c.where);
        }
        return;
    }
    case K::Transition:
        return; // handled by the sequence
    case K::Other:
        if (c.length > 0) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   (c.name.empty() ? std::string("a component") : c.name) + " at " + tc(pos) + " is not supported: left out",
                   c.where);
        }
        return;
    }
}

void Engine::walkSequence(const Component& c, Ticks pos, Rate rate, TrackOut& out, const Context& cx, int depth,
                          TrackKind kind) {
    struct Segment {
        std::size_t first = 0;
        std::size_t end = 0;
        bool content = false;
    };
    struct Pending {
        Ticks start;
        Ticks length;
        Ticks cut;
        FadeCurve curve = FadeCurve::Unknown;
        std::size_t after = 0; ///< segments before it
        std::string where;
    };
    std::vector<Segment> segments;
    std::vector<Pending> transitions;
    Ticks cursor = pos;
    for (const Component& child : c.children) {
        const auto length = units(child.length, rate);
        if (!length || length->value < 0) {
            report(ReportOutcome::Dropped, ReportCategory::Clip, "a component with a length out of range was left out",
                   child.where);
            continue;
        }
        if (child.kind == Component::Kind::Transition) {
            const auto cut = units(std::clamp<std::int64_t>(child.cutPoint, 0, child.length), rate);
            Pending t;
            t.start = cursor - *length;
            t.length = *length;
            t.cut = t.start + cut.value_or(Ticks{});
            t.curve = child.curve;
            t.after = segments.size();
            t.where = child.where;
            transitions.push_back(std::move(t));
            cursor = cursor - *length;
            continue;
        }
        Segment s;
        s.first = out.pieces.size();
        walk(child, cursor, rate, out, cx, depth + 1, kind);
        s.end = out.pieces.size();
        s.content = s.end > s.first;
        segments.push_back(s);
        cursor = cursor + *length;
    }
    auto trimEnd = [&](const Segment& s, Ticks cut) {
        for (std::size_t i = s.first; i < s.end; ++i) {
            Piece& p = out.pieces[i];
            if (p.start >= cut) {
                p.duration = Ticks{};
            } else if (p.end() > cut) {
                p.duration = cut - p.start;
            }
        }
    };
    auto trimStart = [&](const Segment& s, Ticks cut) {
        for (std::size_t i = s.first; i < s.end; ++i) {
            Piece& p = out.pieces[i];
            if (p.end() <= cut) {
                p.duration = Ticks{};
            } else if (p.start < cut) {
                const Ticks delta = cut - p.start;
                p.start = cut;
                p.duration = p.duration - delta;
                for (Source& src : p.sources) {
                    src.sourceIn = src.sourceIn + delta;
                }
            }
        }
    };
    for (const Pending& t : transitions) {
        const Segment* before = t.after > 0 ? &segments[t.after - 1] : nullptr;
        const Segment* after = t.after < segments.size() ? &segments[t.after] : nullptr;
        const bool a = before && before->content;
        const bool b = after && after->content;
        if (!a && !b) {
            continue;
        }
        IrTransition ir;
        ir.kind = kind == TrackKind::Audio ? TransitionKind::AudioCrossfade : TransitionKind::Dissolve;
        if (kind == TrackKind::Audio) {
            const bool linear = t.curve == FadeCurve::Linear;
            ir.effect = linear ? "constantGain" : "constantPower";
            ir.name = linear ? "Constant Gain" : "Constant Power";
        } else {
            ir.name = "Cross Dissolve";
        }
        ir.location = loc(t.where);
        if (a && b) { // a crossfade: each side ends at the cut and takes its handle from its media
            trimEnd(*before, t.cut);
            trimStart(*after, t.cut);
            ir.cut = t.cut;
            ir.before = t.cut - t.start;
            ir.after = t.start + t.length - t.cut;
        } else if (b) { // Filler before: a fade in over the clip's first frames
            ir.cut = t.start;
            ir.after = t.length;
            ir.name = kind == TrackKind::Audio ? ir.name + " fade in" : ir.name;
        } else { // Filler after: a fade out over the clip's last frames
            ir.cut = t.start + t.length;
            ir.before = t.length;
            ir.name = kind == TrackKind::Audio ? ir.name + " fade out" : ir.name;
        }
        out.transitions.push_back(std::move(ir));
    }
}

void Engine::walkSourceClip(const Component& c, Ticks pos, Ticks length, Rate rate, TrackOut& out, const Context& cx,
                            int depth, TrackKind kind) {
    if (c.sourceId.empty() || length.value <= 0) {
        return; // silence
    }
    const Mob* target = m_graph.find(c.sourceId);
    const auto start = units(c.startTime, rate);
    if (!start) {
        report(ReportOutcome::Dropped, ReportCategory::Clip, "a clip at " + tc(pos) + " has a source position out of range",
               c.where);
        return;
    }
    if (target && target->kind == Mob::Kind::Composition) {
        // A subclip, an adjusted clip or a nested sequence: its slot's clips over this clip's time, flattened.
        const Slot* slot = nullptr;
        for (const Slot& s : target->slots) {
            if (s.id == c.sourceSlot) {
                slot = &s;
            }
        }
        if (!slot || !slot->editRate.isValid()) {
            report(ReportOutcome::Dropped, ReportCategory::Clip,
                   "a clip at " + tc(pos) + " plays a track '" + target->name + "' does not have", c.where);
            return;
        }
        const Ticks origin = units(slot->origin, slot->editRate).value_or(Ticks{});
        const Ticks base = pos - *start - origin;
        TrackOut inner;
        walk(slot->segment, base, slot->editRate, inner, cx, depth + 1, kind);
        const Ticks end = pos + length;
        for (Piece& p : inner.pieces) {
            if (p.end() <= pos || p.start >= end) {
                continue;
            }
            if (p.start < pos) {
                const Ticks delta = pos - p.start;
                p.start = pos;
                p.duration = p.duration - delta;
                for (Source& src : p.sources) {
                    src.sourceIn = src.sourceIn + delta;
                }
            }
            if (p.end() > end) {
                p.duration = end - p.start;
            }
            if (p.name.empty() || target->lowerLevel) {
                p.name = target->name.empty() ? p.name : target->name;
            }
            out.pieces.push_back(std::move(p));
            ++m_pieces;
        }
        for (IrTransition& t : inner.transitions) {
            if (t.cut > pos && t.cut < end) {
                out.transitions.push_back(std::move(t));
            }
        }
        return;
    }
    std::string why;
    auto source = resolve(c.sourceId, c.sourceSlot, *start, depth + 1, cx.combiner, why);
    if (!source) {
        // The clip stays, offline: a media item named after what it points to.
        IrMedia m;
        m.key = "missing:" + c.sourceId;
        m.name = target && !target->name.empty() ? target->name : "Missing source";
        m.reference.name = m.name;
        m.hasVideo = kind == TrackKind::Video;
        if (kind == TrackKind::Audio) {
            m.audioChannels = {1};
        }
        m.location = loc(c.where);
        Source s;
        s.media = m_media.add(std::move(m));
        s.sourceIn = *start;
        s.name = target ? target->name : std::string();
        source = s;
        report(ReportOutcome::Approximated, ReportCategory::Media,
               "the clip at " + tc(pos) + " plays a source the file does not describe (" + why + "): it is offline", c.where);
    }
    Piece p;
    p.start = pos;
    p.duration = length;
    p.name = source->name;
    p.where = c.where;
    p.sources.push_back(std::move(*source));
    p.gains = cx.gains;
    p.pan = cx.pan;
    // OMF (and old AAF) clips carry their own fades.
    if (c.fadeIn > 0) {
        if (const auto fade = units(std::min(c.fadeIn, c.length), rate); fade && fade->value > 0) {
            IrTransition t;
            t.kind = kind == TrackKind::Audio ? TransitionKind::AudioCrossfade : TransitionKind::Dissolve;
            t.effect = c.fadeInCurve == FadeCurve::Linear ? "constantGain" : "constantPower";
            t.name = c.fadeInCurve == FadeCurve::Linear ? "Constant Gain fade in" : "Constant Power fade in";
            t.cut = pos;
            t.after = *fade;
            t.location = loc(c.where);
            out.transitions.push_back(std::move(t));
        }
    }
    if (c.fadeOut > 0) {
        if (const auto fade = units(std::min(c.fadeOut, c.length), rate); fade && fade->value > 0) {
            IrTransition t;
            t.kind = kind == TrackKind::Audio ? TransitionKind::AudioCrossfade : TransitionKind::Dissolve;
            t.effect = c.fadeOutCurve == FadeCurve::Linear ? "constantGain" : "constantPower";
            t.name = c.fadeOutCurve == FadeCurve::Linear ? "Constant Gain fade out" : "Constant Power fade out";
            t.cut = pos + length;
            t.before = *fade;
            t.location = loc(c.where);
            out.transitions.push_back(std::move(t));
        }
    }
    out.pieces.push_back(std::move(p));
    ++m_pieces;
}

std::optional<Source> Engine::resolve(const std::string& key, std::uint32_t slotId, Ticks position, int depth,
                                      bool combiner, std::string& why) {
    if (depth > kMaxDepth || ++m_visits > kMaxVisits) {
        m_overBudget = true;
        why = "its sources nest too deeply";
        return std::nullopt;
    }
    const Mob* mob = m_graph.find(key);
    if (!mob) {
        why = "no such mob in the file";
        return std::nullopt;
    }
    const Slot* slot = nullptr;
    for (const Slot& s : mob->slots) {
        if (s.id == slotId) {
            slot = &s;
        }
    }
    if (mob->kind == Mob::Kind::File || mob->kind == Mob::Kind::Physical) {
        Source s;
        s.media = mediaFor(*mob, nullptr);
        Ticks origin;
        if (slot && slot->editRate.isValid()) {
            origin = units(slot->origin, slot->editRate).value_or(Ticks{});
        }
        s.sourceIn = position + origin;
        s.fileChannels = mob->channels;
        s.name = mob->name;
        if (combiner && slot && mob->channels > 1 && slot->physical >= 1 &&
            slot->physical <= static_cast<std::uint32_t>(mob->channels)) {
            s.channel = static_cast<int>(slot->physical) - 1;
        }
        return s;
    }
    if (mob->kind == Mob::Kind::Composition) {
        why = "it plays a sequence where a media file was expected";
        return std::nullopt;
    }
    if (!slot || !slot->editRate.isValid()) {
        why = "'" + mob->name + "' has no track " + std::to_string(slotId);
        return std::nullopt;
    }
    // A master mob: the component of its track at that position leads on to the file.
    const Ticks origin = units(slot->origin, slot->editRate).value_or(Ticks{});
    const Ticks at = position + origin;
    const Component* c = &slot->segment;
    Ticks cStart;
    for (int guard = 0; guard < kMaxDepth && c; ++guard) {
        if (c->kind == Component::Kind::Sequence) {
            const Component* found = nullptr;
            Ticks cursor = cStart;
            for (const Component& child : c->children) {
                const auto length = units(child.length, slot->editRate);
                if (!length) {
                    break;
                }
                if (child.kind == Component::Kind::Transition) {
                    cursor = cursor - *length;
                    continue;
                }
                if (at >= cursor && at < cursor + *length) {
                    found = &child;
                    cStart = cursor;
                    break;
                }
                cursor = cursor + *length;
            }
            c = found;
        } else if ((c->kind == Component::Kind::Selector || c->kind == Component::Kind::Gain ||
                    c->kind == Component::Kind::Pan || c->kind == Component::Kind::Effect ||
                    c->kind == Component::Kind::Combiner) &&
                   !c->children.empty()) {
            c = &c->children.front();
        } else {
            break;
        }
    }
    if (!c || c->kind != Component::Kind::SourceClip || c->sourceId.empty()) {
        why = "'" + mob->name + "' leads to no media file";
        return std::nullopt;
    }
    const auto start = units(c->startTime, slot->editRate);
    if (!start) {
        why = "'" + mob->name + "' has a source position out of range";
        return std::nullopt;
    }
    auto s = resolve(c->sourceId, c->sourceSlot, *start + (at - cStart), depth + 1, combiner, why);
    if (!s) {
        return s;
    }
    if (s->master.empty()) {
        s->master = mob->key;
        if (!mob->name.empty()) {
            s->name = mob->name;
        }
        if (const Mob* file = m_graph.find(c->sourceId); file && file->kind == Mob::Kind::File) {
            // The media item takes the master clip's name (what the editor showed).
            if (const auto it = m_mediaOfFile.find(file->key); it != m_mediaOfFile.end()) {
                IrMedia& m = m_media[it->second];
                if (m.name == file->name && !mob->name.empty()) {
                    m.name = mob->name;
                    m.reference.name = mob->name;
                }
            }
        }
        // A multichannel file played through a master track: the channel its physical track names.
        if (s->channel < 0 && s->fileChannels > 1 && slot->physical >= 1 &&
            slot->physical <= static_cast<std::uint32_t>(s->fileChannels) && combiner) {
            s->channel = static_cast<int>(slot->physical) - 1;
        }
    }
    s->speaker = speakerOfName(s->name);
    return s;
}

// --- Media --------------------------------------------------------------------------------------------------------

std::optional<fs::path> Engine::extract(const Mob& file, const std::string& name, std::string& problem) {
    if (const auto it = m_extracted.find(file.key); it != m_extracted.end()) {
        return it->second;
    }
    if (m_ctx.mediaFolder.empty()) {
        problem = "it was not written to a media folder";
        return std::nullopt;
    }
    if (m_cancelled) {
        problem = "the import was cancelled";
        return std::nullopt;
    }
    const Essence& e = *file.essence;
    std::error_code ec;
    if (!m_createdFolder && !fs::exists(m_ctx.mediaFolder, ec)) {
        m_createdFolder = true;
    }
    fs::create_directories(m_ctx.mediaFolder, ec);
    if (ec) {
        problem = "cannot create " + utf8FromPath(m_ctx.mediaFolder);
        return std::nullopt;
    }
    const std::string base = audiopost::safeFileName(name.empty() ? std::string("Embedded audio") : name);
    const std::uint64_t total = e.header.size() + e.size;
    // FNV-1a of what is written: a file already there with the same bytes (an earlier import) is used again.
    auto fnv = [](std::uint64_t h, std::string_view bytes) {
        for (const char ch : bytes) {
            h = (h ^ static_cast<std::uint8_t>(ch)) * 1099511628211ull;
        }
        return h;
    };
    const fs::path temp = m_ctx.mediaFolder / pathFromUtf8(base + e.extension + ".wabcut-partial");
    std::uint64_t hash = 1469598103934665603ull;
    std::uint64_t written = 0;
    {
        std::ofstream outFile(temp, std::ios::binary | std::ios::trunc);
        if (!outFile) {
            problem = "cannot write to " + utf8FromPath(m_ctx.mediaFolder);
            return std::nullopt;
        }
        outFile.write(e.header.data(), static_cast<std::streamsize>(e.header.size()));
        hash = fnv(hash, e.header);
        written += e.header.size();
        auto copied = e.copy([&](std::string_view block) {
            outFile.write(block.data(), static_cast<std::streamsize>(block.size()));
            hash = fnv(hash, block);
            written += block.size();
            m_essenceDone += block.size();
            return static_cast<bool>(outFile) && reportProgress(false);
        });
        outFile.close();
        if (m_cancelled || !copied || !outFile || written != total) {
            fs::remove(temp, ec);
            problem = m_cancelled ? std::string("the import was cancelled")
                      : !copied   ? copied.error().message
                                  : "writing it failed (" + std::to_string(written) + " of " + std::to_string(total) +
                                      " bytes)";
            return std::nullopt;
        }
        reportProgress(true);
    }
    for (int n = 1; n < 1000; ++n) {
        const std::string stem = n == 1 ? base : base + " (" + std::to_string(n) + ")";
        const fs::path candidate = m_ctx.mediaFolder / pathFromUtf8(stem + e.extension);
        if (m_usedNames.contains(lowerAscii(stem + e.extension))) {
            continue;
        }
        if (fs::exists(candidate, ec)) {
            if (fs::file_size(candidate, ec) != written || ec) {
                continue;
            }
            std::ifstream in(candidate, std::ios::binary);
            std::uint64_t other = 1469598103934665603ull;
            std::string block(1 << 20, '\0');
            while (in) {
                in.read(block.data(), static_cast<std::streamsize>(block.size()));
                other = fnv(other, std::string_view(block.data(), static_cast<std::size_t>(in.gcount())));
            }
            if (other != hash) {
                continue;
            }
            fs::remove(temp, ec); // the same file, from an earlier import
        } else {
            fs::rename(temp, candidate, ec);
            if (ec) {
                fs::remove(temp, ec);
                problem = "cannot write " + utf8FromPath(candidate);
                return std::nullopt;
            }
            m_createdFiles.push_back(candidate);
        }
        m_usedNames.insert(lowerAscii(stem + e.extension));
        const fs::path absolute = fs::absolute(candidate, ec);
        m_extracted.emplace(file.key, ec ? candidate : absolute);
        return m_extracted.at(file.key);
    }
    fs::remove(temp, ec);
    problem = "too many files named " + base + e.extension;
    return std::nullopt;
}

std::size_t Engine::mediaFor(const Mob& file, const Mob* master) {
    if (const auto it = m_mediaOfFile.find(file.key); it != m_mediaOfFile.end()) {
        return it->second;
    }
    IrMedia m;
    m.name = master && !master->name.empty() ? master->name : file.name;
    std::string locator;
    for (const std::string& l : file.locators) {
        if (!trim(l).empty()) {
            locator = l;
            break;
        }
    }
    if (m.name.empty() && !locator.empty()) {
        bool decoded = false;
        m.name = referenceFrom(locator, {}, decoded).name;
    }
    m.location = loc(file.where);
    bool decoded = false;
    if (file.essence) {
        std::string problem;
        if (const auto path = extract(file, m.name, problem)) {
            m.reference = referenceFrom(pathToFileUrl(utf8FromPath(*path), FileUrlStyle::Triple), m.name, decoded);
            m.reference.fileSize = static_cast<std::int64_t>(file.essence->header.size() + file.essence->size);
            m.key = "essence:" + file.key;
            m_doc.report.add(ReportOutcome::Carried, ReportCategory::Media,
                             "'" + m.name + "': its embedded audio was written to " + utf8FromPath(*path), loc(file.where));
        } else {
            m.key = "essence:" + file.key;
            m.reference.name = m.name;
            m_doc.report.add(ReportOutcome::Approximated, ReportCategory::Media,
                             "'" + m.name + "': its embedded audio could not be extracted (" + problem + "): it is offline",
                             loc(file.where));
        }
    } else if (!locator.empty()) {
        m.reference = referenceFrom(locator, m.name, decoded);
        m.key = "file:" + (decoded ? lowerAscii(m.reference.path) : locator);
        if (!file.essenceProblem.empty()) {
            m_doc.report.add(ReportOutcome::Approximated, ReportCategory::Media,
                             "'" + m.name + "': " + file.essenceProblem, loc(file.where));
        }
    } else {
        m.key = "mob:" + file.key;
        m.reference.name = m.name;
        if (!file.essenceProblem.empty()) {
            m_doc.report.add(ReportOutcome::Approximated, ReportCategory::Media,
                             "'" + m.name + "': " + file.essenceProblem + ": it is offline", loc(file.where));
        }
    }
    if (m.name.empty()) {
        m.name = "Media " + std::to_string(m_doc.media.size() + 1);
        m.reference.name = m.name;
    }
    m.hasVideo = file.video;
    m.width = file.width;
    m.height = file.height;
    if (file.video && file.sampleRate.isValid()) {
        m.frameRate = file.sampleRate.normalized();
    }
    if (file.audio || !file.video) {
        m.audioChannels = {std::clamp(file.channels, 1, kMaxSourceChannels)};
        const Rate rate = file.sampleRate.normalized();
        if (file.audio && rate.isValid() && rate.den == 1 && rate.num >= 8000 && rate.num <= 768000) {
            m.sampleRate = rate;
        }
    }
    if (file.length && *file.length > 0 && file.sampleRate.isValid()) {
        if (const auto d = framesToTicksChecked(*file.length, file.sampleRate)) {
            m.duration = *d;
        }
    }
    const std::size_t index = m_media.add(std::move(m));
    m_mediaOfFile.emplace(file.key, index);
    return index;
}

// --- Clips and tracks ------------------------------------------------------------------------------------------------

std::optional<ClipProperties> Engine::propertiesOf(const Piece& p, const Source& s, const std::string& clipName) {
    const bool gain = std::any_of(p.gains.begin(), p.gains.end(), [](const Automation& a) {
        return a.varying() || std::abs(a.constant - 1.0) > 1e-9;
    });
    const bool pan = p.pan && (p.pan->varying() || std::abs(p.pan->constant - 0.5) > 1e-9);
    if (!gain && !pan) {
        return std::nullopt;
    }
    ClipProperties props;
    // Curves are in media time: the source position under each timeline time of the clip.
    auto curveTime = [&](Ticks t) { return s.sourceIn + (t - p.start); };
    if (gain) {
        std::vector<Ticks> times;
        for (const Automation& a : p.gains) {
            for (const auto& [t, v] : a.points) {
                if (t > p.start && t < p.end()) {
                    times.push_back(t);
                }
            }
        }
        auto dbAt = [&](Ticks t) {
            double amplitude = 1.0;
            for (const Automation& a : p.gains) {
                amplitude *= a.at(t);
            }
            return decibels(amplitude);
        };
        if (times.empty() && std::none_of(p.gains.begin(), p.gains.end(), [](const Automation& a) { return a.varying(); })) {
            props[ParamId::VolumeDb] = ParamValue(dbAt(p.start));
            report(ReportOutcome::Carried, ReportCategory::Effect,
                   "clip gain " + dbLabel(dbAt(p.start)) + " dB on '" + clipName + "' at " + tc(p.start), p.where);
        } else {
            times.push_back(p.start);
            times.push_back(p.end());
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end()), times.end());
            if (times.size() > kMaxAutomationPoints) {
                times.resize(kMaxAutomationPoints);
            }
            Curve curve;
            for (const Ticks t : times) {
                curve.keys.push_back(Keyframe{curveTime(t), dbAt(t)});
            }
            props[ParamId::VolumeDb] = ParamValue(curve);
            report(ReportOutcome::Carried, ReportCategory::Effect,
                   "clip volume keyframes (" + std::to_string(curve.keys.size()) + ") on '" + clipName + "' at " + tc(p.start),
                   p.where);
        }
    }
    if (pan) {
        auto balance = [](double v) { return clampDouble(v * 2.0 - 1.0, -1.0, 1.0); };
        if (!p.pan->varying()) {
            props[ParamId::Pan] = ParamValue(balance(p.pan->constant));
        } else {
            Curve curve;
            for (const auto& [t, v] : p.pan->points) {
                if (t < p.start || t > p.end() || curve.keys.size() >= kMaxAutomationPoints) {
                    continue;
                }
                curve.keys.push_back(Keyframe{curveTime(t), balance(v)});
            }
            if (curve.keys.empty()) {
                curve.keys.push_back(Keyframe{curveTime(p.start), balance(p.pan->at(p.start))});
            }
            props[ParamId::Pan] = ParamValue(curve);
        }
        report(ReportOutcome::Carried, ReportCategory::Effect, "clip pan on '" + clipName + "' at " + tc(p.start), p.where);
    }
    if (const auto bad = props.firstInvalid()) {
        report(ReportOutcome::Dropped, ReportCategory::Effect,
               "the volume or pan of '" + clipName + "' at " + tc(p.start) + " is out of range: not imported", p.where);
        return std::nullopt;
    }
    return props;
}

IrClip Engine::clipOf(const Piece& p, const Source& s, TrackKind kind) {
    IrClip c;
    c.name = p.name.empty() ? s.name : p.name;
    c.media = s.media;
    c.stream = StreamRef{kind == TrackKind::Video ? StreamKind::Video : StreamKind::Audio, 0};
    c.start = p.start;
    c.duration = p.duration;
    c.sourceIn = s.sourceIn;
    c.masterKey = s.master;
    c.enabled = p.enabled;
    c.location = loc(p.where);
    if (kind == TrackKind::Audio) {
        c.properties = propertiesOf(p, s, c.name.empty() ? std::string("clip") : c.name);
    }
    return c;
}

std::vector<IrTrack> Engine::makeTracks(const Slot& slot, TrackOut& out, TrackKind kind, std::vector<Automation> trackGains,
                                        std::optional<Automation> trackPan, IrSequence& seq) {
    std::vector<Piece> pieces;
    for (Piece& p : out.pieces) {
        if (p.duration.value <= 0 || p.sources.empty()) {
            continue;
        }
        // A clip may not start before its media: what lies before the first frame is left out.
        Ticks shift;
        for (const Source& s : p.sources) {
            if (s.sourceIn.value < 0) {
                shift = std::max(shift, Ticks{-s.sourceIn.value});
            }
        }
        if (shift.value > 0) {
            if (shift >= p.duration) {
                continue;
            }
            report(ReportOutcome::Approximated, ReportCategory::Clip,
                   "'" + p.name + "' at " + tc(p.start) + " starts before its media: its first " + framesLabel(shift, m_rate) +
                       " were left out",
                   p.where);
            p.start = p.start + shift;
            p.duration = p.duration - shift;
            for (Source& s : p.sources) {
                s.sourceIn = s.sourceIn + shift;
            }
        }
        if (p.start.value < 0 || p.end().value > kMaxTimelineTicks) {
            report(ReportOutcome::Dropped, ReportCategory::Clip, "'" + p.name + "' lies outside the sequence: left out", p.where);
            continue;
        }
        pieces.push_back(std::move(p));
    }
    std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.start < b.start; });

    IrTrack base;
    base.kind = kind;
    base.name = slot.name;
    base.location = loc(slot.segment.where);
    base.transitions = std::move(out.transitions);
    // Unnamed tracks (Media Composer's) go by their physical track: "A3".
    const std::string label = !slot.name.empty() ? "'" + slot.name + "'"
                              : slot.physical > 0 ? std::string(kind == TrackKind::Video ? "V" : "A") + std::to_string(slot.physical)
                                                  : std::string(kind == TrackKind::Video ? "a video track" : "an audio track");

    // Track automation (fader and panner) in timeline time.
    if (kind == TrackKind::Audio) {
        if (!trackGains.empty()) {
            std::vector<Ticks> times;
            for (const Automation& a : trackGains) {
                for (const auto& [t, v] : a.points) {
                    times.push_back(t);
                }
            }
            auto dbAt = [&](Ticks t) {
                double amplitude = 1.0;
                for (const Automation& a : trackGains) {
                    amplitude *= a.at(t);
                }
                return decibels(amplitude);
            };
            if (times.empty()) {
                if (std::abs(dbAt(Ticks{})) > 1e-9) {
                    base.volume = ParamValue(dbAt(Ticks{}));
                }
            } else {
                std::sort(times.begin(), times.end());
                times.erase(std::unique(times.begin(), times.end()), times.end());
                times.resize(std::min(times.size(), kMaxAutomationPoints));
                Curve curve;
                for (const Ticks t : times) {
                    curve.keys.push_back(Keyframe{t, dbAt(t)});
                }
                base.volume = ParamValue(curve);
            }
            if (base.volume) {
                report(ReportOutcome::Carried, ReportCategory::Track,
                       "track volume" + std::string(times.empty() ? "" : " automation (" + std::to_string(times.size()) + " points)") +
                           " on " + label,
                       slot.segment.where);
            }
        }
        if (trackPan) {
            auto balance = [](double v) { return clampDouble(v * 2.0 - 1.0, -1.0, 1.0); };
            if (!trackPan->varying()) {
                if (std::abs(trackPan->constant - 0.5) > 1e-9) {
                    base.pan = ParamValue(balance(trackPan->constant));
                }
            } else {
                Curve curve;
                for (const auto& [t, v] : trackPan->points) {
                    if (curve.keys.size() < kMaxAutomationPoints && t.value >= 0) {
                        curve.keys.push_back(Keyframe{t, balance(v)});
                    }
                }
                if (!curve.keys.empty()) {
                    base.pan = ParamValue(curve);
                }
            }
            if (base.pan) {
                report(ReportOutcome::Carried, ReportCategory::Track,
                       std::string(trackPan->varying() ? "track pan automation" : "track pan") + " on " + label,
                       slot.segment.where);
            }
        }
    }

    // A channel combiner of separate files: one track per channel.
    std::size_t channels = 1;
    bool separate = false;
    for (const Piece& p : pieces) {
        if (p.sources.size() > 1) {
            channels = std::max(channels, p.sources.size());
            std::set<std::size_t> medias;
            for (const Source& s : p.sources) {
                medias.insert(s.media);
            }
            if (medias.size() > 1) {
                separate = true;
            }
        }
    }
    channels = std::min<std::size_t>(channels, kMaxClipChannels);
    if (kind == TrackKind::Video || channels == 1 || !separate) {
        IrTrack track = std::move(base);
        int widest = 1;
        for (const Piece& p : pieces) {
            const Source& s = p.sources.front();
            IrClip c = clipOf(p, s, kind);
            if (kind == TrackKind::Audio && p.sources.size() > 1) {
                // One multichannel file: its channels in the order the combiner lists them.
                AudioChannelMap map;
                map.format = formatForChannels(static_cast<int>(std::min(p.sources.size(), channels)));
                bool identity = s.fileChannels == static_cast<int>(p.sources.size());
                for (std::size_t k = 0; k < p.sources.size() && k < channels; ++k) {
                    ChannelRoute route;
                    const int ch = p.sources[k].channel >= 0 ? p.sources[k].channel : static_cast<int>(k);
                    route.sources = {ch};
                    identity = identity && ch == static_cast<int>(k);
                    map.channels.push_back(std::move(route));
                }
                if (!identity) {
                    c.channels = std::move(map);
                }
                widest = std::max(widest, static_cast<int>(p.sources.size()));
            } else if (kind == TrackKind::Audio) {
                widest = std::max(widest, std::max(s.fileChannels, 1));
            }
            track.clips.push_back(std::move(c));
        }
        if (kind == TrackKind::Audio) {
            track.format = widest <= 1 ? ChannelFormat::Mono : formatForChannels(std::min(widest, kMaxClipChannels));
            if (track.format == ChannelFormat::Surround51) {
                seq.masterFormat = ChannelFormat::Surround51;
                report(ReportOutcome::Carried, ReportCategory::AudioMapping, "5.1 track " + label + " (the master is 5.1)",
                       slot.segment.where);
            }
        }
        return {std::move(track)};
    }

    // Separate files per channel: linked mono tracks, stereo panned left and right, 5.1 routed to a 5.1 master.
    std::vector<int> speakers(channels, -1);
    bool named = true;
    for (const Piece& p : pieces) {
        if (p.sources.size() != channels) {
            continue;
        }
        std::set<int> seen;
        for (std::size_t k = 0; k < channels; ++k) {
            const int sp = p.sources[k].speaker;
            if (sp < 0 || !seen.insert(sp).second) {
                named = false;
            }
            speakers[k] = sp;
        }
        break;
    }
    const bool stereo = channels == 2;
    const bool surround = channels == 6;
    if (stereo && !(named && speakers[0] <= 1 && speakers[1] <= 1)) {
        speakers = {0, 1};
    }
    if (surround && !named) {
        speakers = {0, 1, 2, 3, 4, 5};
        report(ReportOutcome::Approximated, ReportCategory::AudioMapping,
               "the channels of 5.1 track " + label + " do not name their speakers: taken as L R C LFE Ls Rs", slot.segment.where);
    }
    std::vector<IrTrack> tracks(channels);
    const std::string baseName = !slot.name.empty() ? slot.name
                                 : slot.physical > 0 ? "Audio " + std::to_string(slot.physical)
                                                     : std::string("Audio");
    for (std::size_t k = 0; k < channels; ++k) {
        IrTrack& t = tracks[k];
        t.kind = TrackKind::Audio;
        t.location = base.location;
        t.volume = base.volume;
        t.transitions = base.transitions;
        if (stereo) {
            t.name = baseName + (speakers[k] == 0 ? " Left" : " Right");
            t.format = ChannelFormat::Mono;
            t.pan = ParamValue(speakers[k] == 0 ? -1.0 : 1.0);
        } else if (surround) {
            t.name = baseName + " " + suffixOfSpeaker(speakers[k]);
            t.format = ChannelFormat::Adaptive;
        } else {
            t.name = baseName + " " + std::to_string(k + 1);
            t.format = ChannelFormat::Mono;
        }
    }
    for (const Piece& p : pieces) {
        const std::int64_t group = p.sources.size() > 1 ? m_nextLink++ : -1;
        for (std::size_t k = 0; k < p.sources.size() && k < channels; ++k) {
            IrClip c = clipOf(p, p.sources[k], TrackKind::Audio);
            if (!p.sources[k].name.empty()) {
                c.name = p.sources[k].name; // each channel's own file ("Score.C")
            }
            c.linkGroup = group;
            if (p.sources[k].channel >= 0 && p.sources[k].fileChannels > 1) {
                c.channels = AudioChannelMap::mono(p.sources[k].channel);
            }
            if (surround) {
                AudioChannelMap map;
                map.format = ChannelFormat::Adaptive;
                map.channels.resize(6);
                map.channels[static_cast<std::size_t>(speakers[k])].sources = {
                    p.sources[k].channel >= 0 && p.sources[k].fileChannels > 1 ? p.sources[k].channel : 0};
                c.channels = std::move(map);
            }
            tracks[k].clips.push_back(std::move(c));
        }
    }
    if (surround) {
        seq.masterFormat = ChannelFormat::Surround51;
        report(ReportOutcome::Approximated, ReportCategory::AudioMapping,
               "5.1 track " + label + " plays six mono files: it became six linked tracks routed to the 5.1 master's L R C LFE Ls Rs",
               slot.segment.where);
    } else if (stereo) {
        report(ReportOutcome::Approximated, ReportCategory::AudioMapping,
               "stereo track " + label + " plays two mono files: it became two linked mono tracks panned left and right",
               slot.segment.where);
    } else {
        report(ReportOutcome::Approximated, ReportCategory::AudioMapping,
               std::to_string(channels) + "-channel track " + label + " became " + std::to_string(channels) +
                   " linked mono tracks (Wabcut mixes up to 5.1)",
               slot.segment.where);
    }
    return tracks;
}

void Engine::routeSurroundGroups(IrSequence& seq) {
    // Six neighbouring mono tracks named <name> L / R / C / LFE / Ls / Rs (a 5.1 track broken out to mono, as Wabcut,
    // Premiere and Pro Tools export it): routed to the 5.1 master's channels.
    auto& tracks = seq.audioTracks;
    for (std::size_t i = 0; i + 6 <= tracks.size();) {
        std::array<int, 6> speakers{};
        std::set<int> seen;
        const std::string base = withoutSpeaker(tracks[i].name);
        bool group = !base.empty();
        for (std::size_t k = 0; k < 6 && group; ++k) {
            const IrTrack& t = tracks[i + k];
            speakers[k] = speakerOfName(t.name);
            group = speakers[k] >= 0 && seen.insert(speakers[k]).second && withoutSpeaker(t.name) == base &&
                    (!t.format || *t.format == ChannelFormat::Mono) &&
                    std::all_of(t.clips.begin(), t.clips.end(), [](const IrClip& c) { return !c.channels; });
        }
        if (!group) {
            ++i;
            continue;
        }
        for (std::size_t k = 0; k < 6; ++k) {
            IrTrack& t = tracks[i + k];
            t.format = ChannelFormat::Adaptive;
            t.pan.reset();
            for (IrClip& c : t.clips) {
                AudioChannelMap map;
                map.format = ChannelFormat::Adaptive;
                map.channels.resize(6);
                map.channels[static_cast<std::size_t>(speakers[k])].sources = {0};
                c.channels = std::move(map);
            }
        }
        seq.masterFormat = ChannelFormat::Surround51;
        report(ReportOutcome::Carried, ReportCategory::AudioMapping,
               "5.1 layout: tracks '" + tracks[i].name + "' to '" + tracks[i + 5].name +
                   "' play to the 5.1 master's L R C LFE Ls Rs",
               tracks[i].location.element);
        i += 6;
    }
}

namespace {

/// "Dialogue.L" -> {"Dialogue", 0}, "Dialogue_R" -> {"Dialogue", 1} (also " L" / " R" and ".Left" / ".Right"): the
/// halves of a stereo clip that Pro Tools and Media Composer write as two mono master clips. Else {"", -1}.
std::pair<std::string, int> splitMonoHalf(const std::string& name) {
    static constexpr std::array<std::pair<std::string_view, int>, 12> kSuffixes{{
        {".L", 0}, {".R", 1}, {"_L", 0}, {"_R", 1}, {" L", 0}, {" R", 1},
        {".Left", 0}, {".Right", 1}, {"_Left", 0}, {"_Right", 1}, {" Left", 0}, {" Right", 1},
    }};
    for (const auto& [suffix, side] : kSuffixes) {
        if (name.size() > suffix.size() && name.ends_with(suffix)) {
            return {name.substr(0, name.size() - suffix.size()), side};
        }
    }
    return {std::string(), -1};
}

} // namespace

void Engine::linkByMaster(IrSequence& seq) {
    // Clips of one master clip (MasterMob) at the same time and source position on different tracks (picture and
    // sound, or a stereo master on two mono tracks) were one clip in the editor: linked. Two clips that only share a
    // name ("Room Tone") are not. Clips without a master (a composition that plays file mobs directly) fall back to
    // their name. Fix round 1 (ARGON-08): keyed by the master, not the display name.
    using Key = std::tuple<std::string, std::int64_t, std::int64_t>;
    std::map<Key, std::vector<std::pair<std::size_t, IrClip*>>> groups;
    std::map<Key, std::vector<std::pair<std::size_t, IrClip*>>> halves; // split-mono "<name>.L" / "<name>.R"
    std::size_t trackIndex = 0;
    for (auto* list : {&seq.videoTracks, &seq.audioTracks}) {
        for (IrTrack& t : *list) {
            for (IrClip& c : t.clips) {
                if (c.linkGroup >= 0 || (c.masterKey.empty() && c.name.empty())) {
                    continue;
                }
                const std::string offset = "\n" + std::to_string(c.sourceIn.value - c.start.value);
                const std::string id = c.masterKey.empty() ? "name:" + c.name : "master:" + c.masterKey;
                groups[{id + offset, c.start.value, c.duration.value}].emplace_back(trackIndex, &c);
            }
            ++trackIndex;
        }
    }
    auto linkAll = [&](std::vector<std::pair<std::size_t, IrClip*>>& members) {
        if (members.size() < 2 || members.size() > 16) {
            return false;
        }
        std::set<std::size_t> tracksOf;
        for (const auto& [track, clip] : members) {
            tracksOf.insert(track);
        }
        if (tracksOf.size() != members.size()) {
            return false; // two on one track: not one clip
        }
        const std::int64_t group = m_nextLink++;
        for (auto& [track, clip] : members) {
            clip->linkGroup = group;
        }
        return true;
    };
    for (auto& [key, members] : groups) {
        if (linkAll(members)) {
            continue;
        }
        // A lone audio clip named as one half of a stereo pair: matched with its other half below.
        if (members.size() == 1 && members.front().first >= seq.videoTracks.size()) {
            IrClip& c = *members.front().second;
            if (const auto [base, side] = splitMonoHalf(c.name); side >= 0 && !base.empty()) {
                halves[{base + "\n" + std::to_string(c.sourceIn.value - c.start.value), c.start.value,
                        c.duration.value}]
                    .push_back(members.front());
            }
        }
    }
    for (auto& [key, members] : halves) {
        if (members.size() != 2) {
            continue;
        }
        const int a = splitMonoHalf(members[0].second->name).second;
        const int b = splitMonoHalf(members[1].second->name).second;
        if (a != b) {
            linkAll(members); // the left and the right half of one stereo clip
        }
    }
}

void Engine::addMarkers(const Slot& slot, IrSequence& seq) {
    std::vector<const Component*> markers;
    if (slot.segment.kind == Component::Kind::Marker) {
        markers.push_back(&slot.segment);
    }
    for (const Component& c : slot.segment.children) {
        if (c.kind == Component::Kind::Marker) {
            markers.push_back(&c);
        }
    }
    const Ticks origin = units(slot.origin, slot.editRate).value_or(Ticks{});
    for (const Component* c : markers) {
        const auto at = units(c->position, slot.editRate);
        if (!at || *at - origin < Ticks{}) {
            report(ReportOutcome::Dropped, ReportCategory::Marker, "a marker before the sequence's start was left out", c->where);
            continue;
        }
        IrMarker m;
        m.marker.start = *at - origin;
        if (c->length > 0) {
            if (const auto d = units(c->length, slot.editRate)) {
                m.marker.duration = *d;
            }
        }
        std::string text = c->comment;
        if (const auto colon = text.find(": "); colon != std::string::npos && colon > 0) {
            m.marker.name = text.substr(0, colon);
            m.marker.comment = text.substr(colon + 2);
        } else {
            m.marker.name = text;
        }
        if (c->color) {
            m.marker.colorIndex = markerColorFromRgb((*c->color)[0], (*c->color)[1], (*c->color)[2]);
        }
        m.location = loc(c->where);
        seq.markers.push_back(std::move(m));
    }
}

// --- A composition --------------------------------------------------------------------------------------------------

DataKind kindOf(const Component& c, int depth = 0) {
    if (c.data != DataKind::Other || depth > 8) {
        return c.data;
    }
    for (const Component& child : c.children) {
        if (const DataKind k = kindOf(child, depth + 1); k != DataKind::Other) {
            return k;
        }
    }
    return DataKind::Other;
}

const Component* findTimecode(const Component& c, int depth = 0) {
    if (c.kind == Component::Kind::Timecode) {
        return &c;
    }
    if (depth > 4) {
        return nullptr;
    }
    for (const Component& child : c.children) {
        if (const Component* t = findTimecode(child, depth + 1)) {
            return t;
        }
    }
    return nullptr;
}

std::expected<void, Error> Engine::buildSequence(const Mob& comp) {
    IrSequence seq;
    seq.name = comp.name.empty() ? std::string("Sequence") : comp.name;
    seq.location = loc(comp.where);
    m_seqName = seq.name;

    // Rates and start timecode.
    const Slot* timecodeSlot = nullptr;
    const Component* timecode = nullptr;
    std::optional<Rate> pictureRate;
    std::optional<Rate> soundRate;
    for (const Slot& s : comp.slots) {
        if (!s.editRate.isValid() || s.event) {
            continue;
        }
        if (!timecode) {
            if (const Component* t = findTimecode(s.segment)) {
                timecode = t;
                timecodeSlot = &s;
                continue;
            }
        }
        const DataKind k = kindOf(s.segment);
        if (k == DataKind::Picture && !pictureRate) {
            pictureRate = s.editRate.normalized();
        } else if (k == DataKind::Sound && !soundRate) {
            soundRate = s.editRate.normalized();
        }
    }
    Rate frameRate = kRate30;
    if (pictureRate && pictureRate->toDouble() < 1000) {
        frameRate = *pictureRate;
    } else if (timecodeSlot && timecodeSlot->editRate.toDouble() < 1000) {
        frameRate = timecodeSlot->editRate.normalized();
    } else if (soundRate && soundRate->toDouble() < 1000) {
        frameRate = *soundRate;
    } else if (timecode && timecode->fps > 0) {
        frameRate = Rate{timecode->fps, 1};
    }
    seq.frameRate = frameRate;
    m_rate = frameRate.isExact() ? frameRate : kRate30;
    m_dropFrame = false;
    m_startTc = {};
    if (timecode) {
        seq.dropFrame = timecode->drop;
        m_dropFrame = timecode->drop;
        const Rate tcRate = timecodeSlot->editRate.isValid() &&
                                    std::llround(timecodeSlot->editRate.toDouble()) == timecode->fps
                                ? timecodeSlot->editRate
                                : Rate{std::max(timecode->fps, 1), 1};
        if (const auto start = units(timecode->tcStart, tcRate); start && start->value > 0) {
            seq.startTimecode = *start;
            m_startTc = *start;
        }
    }

    // Tracks.
    std::vector<const Slot*> order;
    for (const Slot& s : comp.slots) {
        order.push_back(&s);
    }
    bool physicalOrder = true;
    for (const Slot* s : order) {
        physicalOrder = physicalOrder && s->physical > 0;
    }
    if (physicalOrder) {
        std::stable_sort(order.begin(), order.end(), [](const Slot* a, const Slot* b) { return a->physical < b->physical; });
    }
    Ticks longest;
    std::map<std::int32_t, std::size_t> mediaRates;
    for (const Slot* slot : order) {
        if (slot == timecodeSlot) {
            continue;
        }
        if (!slot->editRate.isValid()) {
            report(ReportOutcome::Dropped, ReportCategory::Track,
                   "track '" + slot->name + "' has no valid edit rate: not imported", slot->segment.where);
            continue;
        }
        if (slot->event) {
            if (m_opt.video) {
                addMarkers(*slot, seq);
            } else {
                report(ReportOutcome::Dropped, ReportCategory::Marker,
                       std::string(displayName(m_opt.format)) + " markers are not imported", slot->segment.where);
            }
            continue;
        }
        const DataKind data = kindOf(slot->segment);
        if (data != DataKind::Sound && data != DataKind::Picture) {
            continue; // timecode, edgecode, descriptive metadata and empty tracks
        }
        const TrackKind kind = data == DataKind::Sound ? TrackKind::Audio : TrackKind::Video;
        if (kind == TrackKind::Video && !m_opt.video) {
            std::size_t clips = 0;
            std::vector<const Component*> todo{&slot->segment};
            while (!todo.empty() && clips < 1'000'000) {
                const Component* c = todo.back();
                todo.pop_back();
                clips += c->kind == Component::Kind::SourceClip ? 1 : 0;
                for (const Component& child : c->children) {
                    todo.push_back(&child);
                }
            }
            if (clips > 0) {
                report(ReportOutcome::Dropped, ReportCategory::Track,
                       "video track '" + slot->name + "' (" + std::to_string(clips) + " clips): " +
                           std::string(displayName(m_opt.format)) + " import brings in the audio",
                       slot->segment.where);
            }
            continue;
        }
        if (const auto length = units(slot->segment.length, slot->editRate)) {
            longest = std::max(longest, *length);
        }
        // Gain and pan around the whole track are the track's fader and panner.
        const Ticks origin = units(slot->origin, slot->editRate).value_or(Ticks{});
        const Component* segment = &slot->segment;
        std::vector<Automation> trackGains;
        std::optional<Automation> trackPan;
        for (int guard = 0; guard < kMaxDepth && kind == TrackKind::Audio; ++guard) {
            if ((segment->kind != Component::Kind::Gain && segment->kind != Component::Kind::Pan) || segment->children.empty() ||
                segment->children.front().kind == Component::Kind::SourceClip) {
                break;
            }
            const Ticks length = units(segment->length, slot->editRate).value_or(Ticks{});
            if (segment->kind == Component::Kind::Gain) {
                for (const Control& c : segment->controls) {
                    trackGains.push_back(automationOf(c, -origin, length));
                }
            } else if (!segment->controls.empty()) {
                trackPan = automationOf(segment->controls.front(), -origin, length);
            }
            segment = &segment->children.front();
        }
        TrackOut out;
        walk(*segment, -origin, slot->editRate, out, Context{}, 0, kind);
        for (const Piece& p : out.pieces) {
            for (const Source& s : p.sources) {
                if (s.media != kNoMedia && kind == TrackKind::Audio) {
                    const Rate r = m_media[s.media].sampleRate;
                    if (r.isValid() && r.den == 1) {
                        ++mediaRates[r.num];
                    }
                }
            }
        }
        for (IrTrack& t : makeTracks(*slot, out, kind, std::move(trackGains), std::move(trackPan), seq)) {
            (kind == TrackKind::Video ? seq.videoTracks : seq.audioTracks).push_back(std::move(t));
        }
    }
    routeSurroundGroups(seq);
    linkByMaster(seq);
    if (longest.value > 0) {
        seq.duration = longest;
    }

    // Audio rate: the sound tracks' sample rate, else the media's most common, else 48 kHz.
    Rate audioRate = kRate48kHz;
    if (soundRate && soundRate->den == 1 && soundRate->num >= 8000 && soundRate->num <= 768000) {
        audioRate = *soundRate;
    } else if (!mediaRates.empty()) {
        const auto most = std::max_element(mediaRates.begin(), mediaRates.end(),
                                           [](const auto& a, const auto& b) { return a.second < b.second; });
        audioRate = Rate{most->first, 1};
    }
    seq.audioRate = audioRate;
    for (const auto& [rate, count] : mediaRates) {
        if (rate != audioRate.num) {
            report(ReportOutcome::Carried, ReportCategory::AudioMapping,
                   std::to_string(count) + " clip(s) play " + std::to_string(rate) + " Hz media, converted to the sequence's " +
                       std::to_string(audioRate.num) + " Hz as they play",
                   comp.where);
        }
    }
    // Frame size: the first picture media's.
    for (const IrTrack& t : seq.videoTracks) {
        for (const IrClip& c : t.clips) {
            if (c.media != kNoMedia && m_media[c.media].width > 0 && m_media[c.media].height > 0) {
                seq.width = m_media[c.media].width;
                seq.height = m_media[c.media].height;
                break;
            }
        }
        if (seq.width != 1920 || seq.height != 1080) {
            break;
        }
    }
    m_doc.sequences.push_back(std::move(seq));
    return {};
}

} // namespace

// --- Helpers --------------------------------------------------------------------------------------------------------

std::optional<AudioHeader> parseAudioHeader(std::string_view b) {
    auto le16 = [&](std::size_t at) { return static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at]) | (static_cast<std::uint8_t>(b[at + 1]) << 8)); };
    auto le32 = [&](std::size_t at) { return le16(at) | (le16(at + 2) << 16); };
    auto be16 = [&](std::size_t at) { return static_cast<std::uint32_t>((static_cast<std::uint8_t>(b[at]) << 8) | static_cast<std::uint8_t>(b[at + 1])); };
    auto be32 = [&](std::size_t at) { return (be16(at) << 16) | be16(at + 2); };
    if (b.size() >= 12 && (b.starts_with("RIFF") || b.starts_with("RF64")) && b.substr(8, 4) == "WAVE") {
        std::size_t at = 12;
        AudioHeader h;
        bool fmt = false;
        for (int guard = 0; guard < 64 && at + 8 <= b.size(); ++guard) {
            const std::string_view id = b.substr(at, 4);
            const std::uint64_t size = le32(at + 4);
            if (id == "fmt " && at + 8 + 16 <= b.size()) {
                h.channels = static_cast<std::int32_t>(le16(at + 10));
                h.sampleRate = static_cast<std::int32_t>(le32(at + 12));
                h.bits = static_cast<std::int32_t>(le16(at + 22));
                fmt = true;
            } else if (id == "data") {
                const std::uint32_t align = h.channels > 0 && h.bits > 0 ? static_cast<std::uint32_t>(h.channels * ((h.bits + 7) / 8)) : 0;
                if (align > 0 && size != 0xFFFFFFFFu) {
                    h.frames = size / align;
                }
                break;
            }
            at += 8 + static_cast<std::size_t>(size) + (size & 1);
            if (size > b.size()) {
                break;
            }
        }
        if (!fmt || h.channels <= 0 || h.channels > kMaxSourceChannels) {
            return std::nullopt;
        }
        return h;
    }
    if (b.size() >= 12 && b.starts_with("FORM") && (b.substr(8, 4) == "AIFF" || b.substr(8, 4) == "AIFC")) {
        std::size_t at = 12;
        AudioHeader h;
        h.aiff = true;
        for (int guard = 0; guard < 64 && at + 8 <= b.size(); ++guard) {
            const std::string_view id = b.substr(at, 4);
            const std::uint64_t size = be32(at + 4);
            if (id == "COMM" && at + 8 + 18 <= b.size()) {
                h.channels = static_cast<std::int32_t>(be16(at + 8));
                h.frames = be32(at + 10);
                h.bits = static_cast<std::int32_t>(be16(at + 14));
                // 80-bit extended sample rate.
                const int exponent = static_cast<int>(be16(at + 16) & 0x7FFF) - 16383;
                std::uint64_t mantissa = 0;
                for (int i = 0; i < 8; ++i) {
                    mantissa = (mantissa << 8) | static_cast<std::uint8_t>(b[at + 18 + static_cast<std::size_t>(i)]);
                }
                const double rate = std::ldexp(static_cast<double>(mantissa), exponent - 63);
                h.sampleRate = std::isfinite(rate) && rate > 0 && rate < 1e7 ? static_cast<std::int32_t>(std::llround(rate)) : 0;
                if (h.channels <= 0 || h.channels > kMaxSourceChannels) {
                    return std::nullopt;
                }
                return h;
            }
            at += 8 + static_cast<std::size_t>(size) + (size & 1);
            if (size > b.size()) {
                break;
            }
        }
    }
    return std::nullopt;
}

std::string wavHeader(std::int32_t channels, std::int32_t sampleRate, std::int32_t bits, std::uint64_t dataBytes) {
    std::string h;
    auto put16 = [&](std::uint32_t v) {
        h.push_back(static_cast<char>(v & 0xFF));
        h.push_back(static_cast<char>((v >> 8) & 0xFF));
    };
    auto put32 = [&](std::uint32_t v) {
        put16(v & 0xFFFF);
        put16(v >> 16);
    };
    const std::uint32_t data = static_cast<std::uint32_t>(std::min<std::uint64_t>(dataBytes, 0xFFFFFFFFull - 36));
    const std::uint32_t align = static_cast<std::uint32_t>(channels * ((bits + 7) / 8));
    h += "RIFF";
    put32(36 + data);
    h += "WAVEfmt ";
    put32(16);
    put16(1); // PCM
    put16(static_cast<std::uint32_t>(channels));
    put32(static_cast<std::uint32_t>(sampleRate));
    put32(static_cast<std::uint32_t>(sampleRate) * align);
    put16(align);
    put16(static_cast<std::uint32_t>(bits));
    h += "data";
    put32(data);
    return h;
}

std::string textToUtf8(std::string_view text) {
    std::string out;
    if (isValidUtf8(text)) {
        out.reserve(text.size());
        for (const char ch : text) {
            if (static_cast<unsigned char>(ch) >= 0x20 || ch == '\t') {
                out.push_back(ch);
            }
        }
        return out;
    }
    for (const char ch : text) {
        const auto u = static_cast<unsigned char>(ch);
        if (u < 0x20) {
            continue;
        }
        if (u < 0x80) {
            out.push_back(ch);
        } else {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

namespace {

struct SpeakerSuffix {
    std::string_view text;
    int speaker;
};
// Longest first, so "Ls" is not read as "L" and "LFE" not as "L".
constexpr SpeakerSuffix kSuffixes[] = {
    {"left surround", 4}, {"right surround", 5}, {"surround left", 4}, {"surround right", 5}, {"centre", 2},
    {"center", 2},        {"left", 0},           {"right", 1},         {"lfe", 3},          {"sub", 3},
    {"ls", 4},            {"rs", 5},             {"lss", 4},           {"rss", 5},          {"l", 0},
    {"r", 1},             {"c", 2},
};

std::optional<std::size_t> suffixAt(std::string_view name, int& speaker) {
    const std::string lower = toLowerAscii(name);
    std::size_t best = 0;
    for (const SpeakerSuffix& s : kSuffixes) {
        if (lower.size() <= s.text.size() || !std::string_view(lower).ends_with(s.text)) {
            continue;
        }
        const char before = lower[lower.size() - s.text.size() - 1];
        if (before != ' ' && before != '.' && before != '_' && before != '-') {
            continue;
        }
        if (s.text.size() > best) {
            best = s.text.size();
            speaker = s.speaker;
        }
    }
    if (best == 0) {
        return std::nullopt;
    }
    return name.size() - best - 1;
}

} // namespace

int speakerOfName(std::string_view name) {
    int speaker = -1;
    return suffixAt(trim(name), speaker) ? speaker : -1;
}

std::string withoutSpeaker(std::string_view name) {
    const std::string_view n = trim(name);
    int speaker = -1;
    const auto at = suffixAt(n, speaker);
    return std::string(at ? trim(n.substr(0, *at)) : n);
}

std::expected<IrDocument, Error> buildDocument(Graph graph, const ParseContext& context, const BuildOptions& options) {
    Engine engine(graph, context, options);
    return engine.run();
}

} // namespace wabcut::interchange::detail::graph
