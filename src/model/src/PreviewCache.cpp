// Preview render cache (C.15): the render bar, content hashes, the index and invalidation (PreviewCache.h).

#include <wabcut/model/PreviewCache.h>

#include <wabcut/model/Effects.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/Resolve.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <system_error>
#include <tuple>

namespace wabcut {

namespace {

using json = nlohmann::json;

std::string hex64(std::uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return std::string(buf, 16);
}

std::uint64_t fnv1a64(std::string_view text) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

bool isHex16(std::string_view s) {
    return s.size() == 16 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

/// What a plan costs and why (renderSegments).
struct Tally {
    double cost = 0.0;
    int layers = 0;
    bool nested = false;
    bool adjustment = false;
    bool transition = false;
    bool blend = false;
    bool matte = false;
    std::set<std::string> effects; ///< display names of the evaluated effects that cost something
};

void tallyStack(const EffectStack& stack, Tally& t) {
    for (const EffectInstance& e : stack) {
        if (!e.enabled || !isEvaluable(e, EffectKind::Video)) {
            continue;
        }
        const double c = effectRenderCost(e.effect);
        t.cost += c;
        if (c > 0.0) {
            const EffectDescriptor* d = EffectRegistry::builtin().find(e.effect);
            t.effects.insert(d ? std::string(d->displayName) : e.effect);
        }
    }
}

void tally(const CompositePlan& plan, Tally& t, int depth) {
    if (depth > 16) {
        return;
    }
    for (const CompositeStep& s : plan.steps) {
        if (s.isAdjustment()) {
            t.adjustment = true;
            t.cost += 1.0;
        } else {
            ++t.layers;
            t.cost += 1.0;
        }
        tallyStack(s.clip->effects(), t);
        if (s.transition.transition) {
            t.transition = true;
            t.cost += 0.5;
        }
        if (s.blend != BlendMode::Normal) {
            t.blend = true;
            t.cost += 0.5;
        }
        if (s.nested) {
            t.nested = true;
            tally(*s.nested, t, depth + 1);
        }
        if (s.matte) {
            t.matte = true;
            if (s.matte->plan) {
                tally(*s.matte->plan, t, depth + 1);
            }
        }
    }
    if (plan.masterEffects) {
        tallyStack(*plan.masterEffects, t);
    }
}

RenderSegment classify(const ProjectState& state, SequenceId sequence, TimeRange range) {
    RenderSegment seg;
    seg.range = range;
    const CompositePlan plan = resolveComposite(state, sequence, range.start);
    Tally t;
    tally(plan, t, 0);
    seg.cost = t.cost;
    std::string effects;
    for (const std::string& name : t.effects) {
        effects += (effects.empty() ? "" : ", ") + name;
    }
    if (t.nested || t.adjustment) {
        seg.status = RenderStatus::Red;
        seg.reason = t.nested && t.adjustment ? "nested sequence and adjustment layer"
                     : t.nested               ? "nested sequence"
                                              : "adjustment layer";
    } else if (t.cost > kRealTimeBudget) {
        seg.status = RenderStatus::Red;
        seg.reason = "heavier than real time" + (effects.empty() ? std::string() : ": " + effects);
    } else if (!t.effects.empty() || t.transition || t.blend || t.matte) {
        seg.status = RenderStatus::Yellow;
        std::vector<std::string> why;
        if (!effects.empty()) {
            why.push_back("effects: " + effects);
        }
        if (t.transition) {
            why.push_back("transition");
        }
        if (t.blend) {
            why.push_back("blend mode");
        }
        if (t.matte) {
            why.push_back("track matte");
        }
        for (const std::string& w : why) {
            seg.reason += (seg.reason.empty() ? "" : "; ") + w;
        }
    }
    return seg;
}

} // namespace

std::string_view toString(RenderStatus status) {
    switch (status) {
    case RenderStatus::None: return "none";
    case RenderStatus::Yellow: return "yellow";
    case RenderStatus::Red: return "red";
    case RenderStatus::Rendered: return "rendered";
    }
    return "none";
}

double effectRenderCost(std::string_view id) {
    static const std::map<std::string_view, double> kCosts{
        {"wabcut.color.basic", 0.3}, {"wabcut.vignette", 0.2}, {"wabcut.color.grade", 0.8},
        {"wabcut.blur.gaussian", 1.5}, {"wabcut.sharpen", 0.6}, {"wabcut.crop", 0.1},
        {"wabcut.dropShadow", 1.0}, {"wabcut.title.style", 0.0}, {"wabcut.mask", 0.3},
        {"wabcut.mosaic", 0.4}, {kTrackMatteEffect, 0.3}, {kUltraKeyEffect, 2.0}, {kBorderEffect, 0.3},
    };
    const auto it = kCosts.find(id);
    return it == kCosts.end() ? 1.0 : it->second;
}

std::vector<RenderSegment> renderSegments(const ProjectState& state, SequenceId sequence, std::optional<TimeRange> within) {
    std::vector<RenderSegment> out;
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        return out;
    }
    const TimeRange span = within ? *within : TimeRange{Ticks{}, seq->duration};
    if (span.duration.value <= 0) {
        return out;
    }
    std::vector<Ticks> points{span.start, span.end()};
    const auto add = [&](Ticks t) {
        if (t > span.start && t < span.end()) {
            points.push_back(t);
        }
    };
    for (const TrackPtr& track : seq->videoTracks) {
        const std::size_t first = track->clips.upperBoundStart(span.start);
        for (std::size_t i = first > 0 ? first - 1 : 0; i < track->clips.size(); ++i) {
            const Clip& c = track->clips[i];
            if (c.start() >= span.end()) {
                break;
            }
            add(c.start());
            add(c.end());
        }
        for (const Transition& tr : track->transitions) {
            if (const auto cut = transitionEditPoint(*track, tr)) {
                add(*cut - tr.before);
                add(*cut + tr.after);
            }
        }
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        out.push_back(classify(state, sequence, TimeRange::fromStartEnd(points[i], points[i + 1])));
    }
    return out;
}

std::string renderContentHash(const ProjectState& state, SequenceId sequence, TimeRange range) {
    const std::string key = io::renderKeyJson(state, sequence, range);
    return key.empty() ? std::string() : hex64(fnv1a64(key));
}

// ---- PreviewCache ---------------------------------------------------------------------------------------------------

PreviewCache::PreviewCache(std::filesystem::path folder, std::int64_t budgetBytes)
    : m_folder(std::move(folder)), m_budget(std::max<std::int64_t>(budgetBytes, 0)) {}

std::int64_t PreviewCache::budget() const {
    const std::lock_guard lock(m_mutex);
    return m_budget;
}

std::vector<PreviewEntry> PreviewCache::setBudget(std::int64_t bytes) {
    const std::lock_guard lock(m_mutex);
    retryDeletesLocked();
    m_budget = std::max<std::int64_t>(bytes, 0);
    return evictLocked();
}

bool PreviewCache::isPlainFileName(std::string_view name) {
    if (name.empty() || name.size() > 128 || name.front() == '.') {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

std::string PreviewCache::indexJson(const std::vector<PreviewEntry>& entries) {
    json list = json::array();
    for (const PreviewEntry& e : entries) {
        list.push_back(json{{"sequence", e.sequence.value},
                            {"start", e.range.start.value},
                            {"duration", e.range.duration.value},
                            {"hash", e.hash},
                            {"file", e.file},
                            {"bytes", e.bytes},
                            {"lastUse", e.lastUse}});
    }
    return json{{"format", "wabcut-preview-cache"}, {"version", 1}, {"entries", std::move(list)}}.dump(1);
}

std::expected<std::vector<PreviewEntry>, Error> PreviewCache::parseIndex(std::string_view text) {
    const auto bad = [](std::string why) {
        return std::unexpected(Error{ErrorCode::ParseError, "preview cache index: " + std::move(why)});
    };
    if (text.size() > std::size_t{64} * 1024 * 1024 || !io::jsonNestingWithin(text, 8)) {
        return bad("too large or too deeply nested");
    }
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return bad("not a JSON object");
    }
    const auto format = root.find("format");
    const auto version = root.find("version");
    const auto list = root.find("entries");
    if (format == root.end() || !format->is_string() || format->get<std::string>() != "wabcut-preview-cache" ||
        version == root.end() || !version->is_number_integer() || version->get<std::int64_t>() != 1) {
        return bad("not a version 1 preview cache index");
    }
    if (list == root.end() || !list->is_array() || list->size() > kMaxEntries) {
        return bad("entries missing or more than " + std::to_string(kMaxEntries));
    }
    std::vector<PreviewEntry> out;
    out.reserve(list->size());
    const auto integer = [](const json& o, const char* key, std::int64_t lo, std::int64_t hi) -> std::optional<std::int64_t> {
        const auto it = o.find(key);
        if (it == o.end() || !it->is_number_integer()) {
            return std::nullopt;
        }
        const auto v = it->is_number_unsigned() ? (it->get<std::uint64_t>() > static_cast<std::uint64_t>(hi)
                                                       ? std::optional<std::int64_t>()
                                                       : std::optional<std::int64_t>(static_cast<std::int64_t>(it->get<std::uint64_t>())))
                                                : std::optional<std::int64_t>(it->get<std::int64_t>());
        if (!v || *v < lo || *v > hi) {
            return std::nullopt;
        }
        return v;
    };
    constexpr std::int64_t kMaxTime = std::int64_t{1} << 61;
    for (const json& e : *list) {
        if (!e.is_object()) {
            return bad("an entry is not an object");
        }
        const auto sequence = integer(e, "sequence", 1, std::numeric_limits<std::int64_t>::max());
        const auto start = integer(e, "start", 0, kMaxTime);
        const auto duration = integer(e, "duration", 1, kMaxTime);
        const auto bytes = integer(e, "bytes", 0, std::int64_t{1} << 50);
        const auto lastUse = integer(e, "lastUse", 0, std::numeric_limits<std::int64_t>::max());
        const auto hash = e.find("hash");
        const auto file = e.find("file");
        if (!sequence || !start || !duration || !bytes || !lastUse || hash == e.end() || !hash->is_string() ||
            !isHex16(hash->get<std::string>()) || file == e.end() || !file->is_string() ||
            !isPlainFileName(file->get<std::string>())) {
            return bad("entry " + std::to_string(out.size()) + " is malformed");
        }
        PreviewEntry entry;
        entry.sequence = SequenceId{static_cast<std::uint64_t>(*sequence)};
        entry.range = TimeRange{Ticks{*start}, Ticks{*duration}};
        entry.hash = hash->get<std::string>();
        entry.file = file->get<std::string>();
        entry.bytes = *bytes;
        entry.lastUse = *lastUse;
        out.push_back(std::move(entry));
    }
    std::sort(out.begin(), out.end(), [](const PreviewEntry& a, const PreviewEntry& b) {
        return a.sequence.value != b.sequence.value ? a.sequence.value < b.sequence.value : a.range.start < b.range.start;
    });
    for (std::size_t i = 1; i < out.size(); ++i) {
        if (out[i].sequence == out[i - 1].sequence && out[i].range.overlaps(out[i - 1].range)) {
            return bad("entries of one sequence overlap");
        }
    }
    return out;
}

std::expected<void, Error> PreviewCache::load() {
    const std::lock_guard lock(m_mutex);
    m_entries.clear();
    std::error_code ec;
    const std::filesystem::path index = m_folder / std::string(kIndexFile);
    if (!std::filesystem::exists(index, ec)) {
        return {};
    }
    std::ifstream in(index, std::ios::binary);
    if (!in) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read the preview cache index"});
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto parsed = parseIndex(text);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    m_entries = std::move(*parsed);
    for (const PreviewEntry& e : m_entries) {
        m_useCounter = std::max(m_useCounter, e.lastUse);
    }
    // C.16: preview files the index does not name (left by a delete that failed before a restart) go. Renders in
    // progress write "<name>.wabcut-partial-<hex>.wpv" and are left alone.
    for (const auto& file : std::filesystem::directory_iterator(m_folder, ec)) {
        const std::string name = pathToUtf8(file.path().filename());
        const bool preview = name.size() > 4 && name.ends_with(".wpv") && name.find(".wabcut-partial-") == std::string::npos;
        if (preview && std::none_of(m_entries.begin(), m_entries.end(), [&](const PreviewEntry& e) { return e.file == name; })) {
            deleteName(name);
        }
    }
    return {};
}

std::expected<void, Error> PreviewCache::save() const {
    std::string text;
    {
        const std::lock_guard lock(m_mutex);
        retryDeletesLocked();
        text = indexJson(m_entries);
    }
    std::error_code ec;
    std::filesystem::create_directories(m_folder, ec);
    return io::writeFileAtomically(m_folder / std::string(kIndexFile), text);
}

std::vector<PreviewEntry> PreviewCache::entries() const {
    const std::lock_guard lock(m_mutex);
    return m_entries;
}

std::int64_t PreviewCache::totalBytes() const {
    const std::lock_guard lock(m_mutex);
    std::int64_t total = 0;
    for (const PreviewEntry& e : m_entries) {
        total += e.bytes;
    }
    return total;
}

std::optional<PreviewEntry> PreviewCache::lookup(SequenceId sequence, Ticks t) {
    const std::lock_guard lock(m_mutex);
    for (PreviewEntry& e : m_entries) {
        if (e.sequence == sequence && e.range.contains(t)) {
            e.lastUse = ++m_useCounter;
            return e;
        }
    }
    return std::nullopt;
}

std::optional<PreviewEntry> PreviewCache::validAt(const ProjectState& state, SequenceId sequence, Ticks t) {
    std::optional<PreviewEntry> hit = lookup(sequence, t);
    if (!hit || renderContentHash(state, sequence, hit->range) != hit->hash) {
        return std::nullopt;
    }
    return hit;
}

std::vector<RenderSegment> PreviewCache::renderBar(const ProjectState& state, SequenceId sequence,
                                                   std::optional<TimeRange> within) const {
    std::vector<RenderSegment> bar = renderSegments(state, sequence, within);
    const std::vector<PreviewEntry> list = entries();
    std::map<std::pair<std::int64_t, std::int64_t>, std::string> hashes; // per entry range: the current hash
    for (RenderSegment& seg : bar) {
        for (const PreviewEntry& e : list) {
            if (e.sequence != sequence || e.range.start > seg.range.start || e.range.end() < seg.range.end()) {
                continue;
            }
            const auto key = std::pair{e.range.start.value, e.range.duration.value};
            auto it = hashes.find(key);
            if (it == hashes.end()) {
                it = hashes.emplace(key, renderContentHash(state, sequence, e.range)).first;
            }
            if (it->second == e.hash) {
                seg.status = RenderStatus::Rendered;
            }
            break;
        }
    }
    return bar;
}

std::string PreviewCache::fileNameFor(SequenceId sequence, TimeRange range, std::string_view hash) {
    return std::to_string(sequence.value) + "-" + std::to_string(range.start.value) + "-" + std::string(hash) + ".wpv";
}

void PreviewCache::deleteFile(const PreviewEntry& entry) const { deleteName(entry.file); }

void PreviewCache::deleteName(const std::string& name) const {
    if (!isPlainFileName(name)) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(m_folder / name, ec);
    std::error_code exists;
    if (std::filesystem::exists(m_folder / name, exists) &&
        std::find(m_undeleted.begin(), m_undeleted.end(), name) == m_undeleted.end()) {
        m_undeleted.push_back(name); // open elsewhere (Windows): retried later
    }
}

void PreviewCache::retryDeletesLocked() const {
    std::vector<std::string> pending;
    pending.swap(m_undeleted);
    for (const std::string& name : pending) {
        deleteName(name);
    }
}

std::size_t PreviewCache::retryDeletes() {
    const std::lock_guard lock(m_mutex);
    retryDeletesLocked();
    return m_undeleted.size();
}

std::vector<PreviewEntry> PreviewCache::evictLocked() {
    std::vector<PreviewEntry> evicted;
    std::int64_t total = 0;
    for (const PreviewEntry& e : m_entries) {
        total += e.bytes;
    }
    while (total > m_budget && !m_entries.empty()) {
        const auto oldest = std::min_element(m_entries.begin(), m_entries.end(),
                                             [](const PreviewEntry& a, const PreviewEntry& b) { return a.lastUse < b.lastUse; });
        total -= oldest->bytes;
        deleteFile(*oldest);
        evicted.push_back(*oldest);
        m_entries.erase(oldest);
    }
    return evicted;
}

std::vector<PreviewEntry> PreviewCache::record(PreviewEntry entry) {
    const std::lock_guard lock(m_mutex);
    retryDeletesLocked();
    std::vector<PreviewEntry> out;
    if (!isPlainFileName(entry.file) || entry.range.duration.value <= 0) {
        return out;
    }
    std::vector<PreviewEntry> kept;
    for (PreviewEntry& e : m_entries) {
        if (e.sequence == entry.sequence && e.range.overlaps(entry.range)) {
            if (e.file != entry.file) {
                deleteFile(e);
            }
            out.push_back(std::move(e));
        } else {
            kept.push_back(std::move(e));
        }
    }
    entry.lastUse = ++m_useCounter;
    kept.push_back(std::move(entry));
    std::sort(kept.begin(), kept.end(), [](const PreviewEntry& a, const PreviewEntry& b) {
        return a.sequence.value != b.sequence.value ? a.sequence.value < b.sequence.value : a.range.start < b.range.start;
    });
    m_entries = std::move(kept);
    for (PreviewEntry& e : evictLocked()) {
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<PreviewEntry> PreviewCache::invalidate(const ProjectState& after, const ChangeSet& changes) {
    const std::lock_guard lock(m_mutex);
    retryDeletesLocked();
    std::vector<PreviewEntry> dropped;
    std::vector<PreviewEntry> kept;
    const bool mediaChanged =
        !changes.modifiedMedia.empty() || !changes.removedMedia.empty() || changes.redactionsChanged;
    std::map<std::tuple<std::uint64_t, std::int64_t, std::int64_t>, std::string> hashes;
    for (PreviewEntry& e : m_entries) {
        if (!after.findSequence(e.sequence)) {
            deleteFile(e);
            dropped.push_back(std::move(e));
            continue;
        }
        bool recheck = mediaChanged;
        for (const SequenceChange& sc : changes.sequences) {
            if (recheck) {
                break;
            }
            if (sc.sequence != e.sequence) {
                recheck = sequenceContains(after, e.sequence, sc.sequence); // a nested sequence it plays changed
                continue;
            }
            recheck = sc.settingsChanged || sc.trackListChanged;
            for (const TrackChange& tc : sc.tracks) {
                recheck = recheck || tc.headerChanged || tc.transitionsChanged || tc.added || tc.removed ||
                          std::any_of(tc.ranges.begin(), tc.ranges.end(), [&](const TimeRange& r) { return r.overlaps(e.range); });
            }
        }
        if (recheck) {
            const auto key = std::tuple{e.sequence.value, e.range.start.value, e.range.duration.value};
            auto it = hashes.find(key);
            if (it == hashes.end()) {
                it = hashes.emplace(key, renderContentHash(after, e.sequence, e.range)).first;
            }
            if (it->second != e.hash) {
                deleteFile(e);
                dropped.push_back(std::move(e));
                continue;
            }
        }
        kept.push_back(std::move(e));
    }
    m_entries = std::move(kept);
    return dropped;
}

std::vector<PreviewEntry> PreviewCache::remove(SequenceId sequence, std::optional<TimeRange> range) {
    const std::lock_guard lock(m_mutex);
    retryDeletesLocked();
    std::vector<PreviewEntry> removed;
    std::vector<PreviewEntry> kept;
    for (PreviewEntry& e : m_entries) {
        const bool hit = (!sequence.isValid() || e.sequence == sequence) && (!range || e.range.overlaps(*range));
        if (hit) {
            deleteFile(e);
            removed.push_back(std::move(e));
        } else {
            kept.push_back(std::move(e));
        }
    }
    m_entries = std::move(kept);
    return removed;
}

} // namespace wabcut
