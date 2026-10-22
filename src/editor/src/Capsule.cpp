#include <wabcut/editor/Capsule.h>

#include "PathSafety.h"

#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/export/TrimmedCopy.h>
#include <wabcut/model/ImageSequence.h>
#include <wabcut/model/SpannedClip.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/TimeRemap.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <system_error>

namespace wabcut::editor {

namespace {

constexpr std::int64_t kSpaceMargin = 64ll * 1024 * 1024;

bool isFile(const std::filesystem::path& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p, ec);
}

std::int64_t sizeOf(const std::filesystem::path& p) {
    std::error_code ec;
    const auto s = std::filesystem::file_size(p, ec);
    return ec ? 0 : static_cast<std::int64_t>(s);
}

/// The keys of `remap` that bound the segments curve times [from, to) cross: the first and last index (C.6 segments
/// between keys, and the holds before the first and after the last key).
std::pair<std::size_t, std::size_t> keysAround(const Curve& remap, Ticks from, Ticks to) {
    std::size_t first = remap.keys.size() - 1;
    std::size_t last = 0;
    for (std::size_t i = 0; i < remap.keys.size(); ++i) {
        const Ticks t = remap.keys[i].time;
        // Key i bounds the segment that ends at it (if that one starts before `to`) and the one that starts at it
        // (if that one ends after `from`).
        const bool before = t > from && (i == 0 || remap.keys[i - 1].time < to);
        const bool after = t < to && (i + 1 == remap.keys.size() || remap.keys[i + 1].time > from);
        if (before || after || (t >= from && t < to)) {
            first = std::min(first, i);
            last = std::max(last, i);
        }
    }
    if (first > last) { // [from, to) lies in a hold before the first or after the last key
        first = last = remap.keys.front().time >= to ? 0 : remap.keys.size() - 1;
    }
    return {first, last};
}

/// The media time a clip shows: [sourceIn, sourceOut) at its speed (a freeze frame holds one `frame`). A remapped clip
/// (C.6: sourceIn and sourceOut are curve times) shows what its curve maps them to, so the range is the lowest to the
/// highest source time shown, widened to the positions of the keys bounding those segments: the trimmed media then
/// holds every position the shifted curve interpolates between (Codex wave-4 P1-7).
TimeRange shownMediaRange(const Clip& clip, Ticks frame) {
    if (const Curve* remap = clip.timeRemap(); remap && !remap->keys.empty()) {
        const Ticks to = std::max(clip.sourceOut(), clip.sourceIn + Ticks{1});
        auto [lo, hi] = remapShownRange(*remap, clip.sourceIn, to);
        const auto [first, last] = keysAround(*remap, clip.sourceIn, to);
        for (std::size_t i = first; i <= last; ++i) {
            const Ticks position{static_cast<std::int64_t>(remap->keys[i].value)};
            lo = std::min(lo, position);
            hi = std::max(hi, position);
        }
        return TimeRange::fromStartEnd(std::max(lo, Ticks{}), std::max(hi, lo) + frame); // hi starts the last frame
    }
    return TimeRange::fromStartEnd(clip.sourceIn, std::max(clip.sourceOut(), clip.sourceIn + frame));
}

/// The source range every clip of every sequence uses, per media item (a hull: one range per file).
std::map<std::uint64_t, TimeRange> usedRanges(const ProjectState& state) {
    std::map<std::uint64_t, TimeRange> out;
    for (const SequencePtr& seq : state.sequences) {
        if (!seq) {
            continue;
        }
        seq->forEachTrack([&](const Track& track) {
            for (const Clip& clip : track.clips) {
                if (!clip.media.isValid()) {
                    continue;
                }
                const Rate rate = seq->frameRate.isValid() ? seq->frameRate : Rate{30, 1};
                const Ticks frame = framesToTicks(1, rate, Rounding::Floor).value_or(Ticks{1});
                const TimeRange r = shownMediaRange(clip, frame);
                auto [it, inserted] = out.emplace(clip.media.value, r);
                if (!inserted) {
                    it->second = TimeRange::fromStartEnd(std::min(it->second.start, r.start), std::max(it->second.end(), r.end()));
                }
            }
        });
    }
    return out;
}

/// Moves every media-time value of `clip` `shift` earlier. A plain clip: its In point, keyframe curves and effect
/// curves (all media time). A remapped clip keeps its In point and its curves, which are curve times; only the remap's
/// key positions are media time (Codex wave-4 P1-7). Keys bounding no segment the clip shows are dropped first: their
/// positions may lie before the trimmed media, and they change nothing the clip plays.
void shiftMediaTime(Clip& clip, Ticks shift) {
    if (const Curve* remap = clip.timeRemap(); remap && !remap->keys.empty()) {
        const auto [first, last] = keysAround(*remap, clip.sourceIn, std::max(clip.sourceOut(), clip.sourceIn + Ticks{1}));
        ClipContent content = clip.content();
        std::vector<Keyframe> keys(content.timeRemap->keys.begin() + static_cast<std::ptrdiff_t>(first),
                                   content.timeRemap->keys.begin() + static_cast<std::ptrdiff_t>(last) + 1);
        for (Keyframe& k : keys) {
            k.value -= static_cast<double>(shift.value);
        }
        content.timeRemap->keys = std::move(keys);
        clip.setContent(std::move(content));
        return;
    }
    clip.sourceIn = clip.sourceIn - shift;
    const auto shiftValue = [&](ParamValue& v) {
        if (auto* curve = std::get_if<Curve>(&v.data)) {
            for (Keyframe& k : curve->keys) {
                k.time = k.time - shift;
            }
        }
    };
    ClipProperties props = clip.properties();
    for (ParamValue& v : props.values) {
        shiftValue(v);
    }
    clip.setProperties(props);
    EffectStack effects = clip.effects();
    for (EffectInstance& e : effects) {
        for (EffectParam& p : e.params) {
            shiftValue(p.value);
        }
    }
    clip.setEffects(std::move(effects));
}

/// Every path the capsule holds, case-folded (Codex wave-3 P1-4): on Windows and macOS "A.wav" and "a.wav" are one
/// file, so names that differ only by case must never be handed out twice, in any folder of the capsule.
struct CapsuleNames {
    std::set<std::string> taken;

    /// A capsule moves between computers: names must differ on the strictest file system it may land on (I.7).
    static std::string keyOf(const std::filesystem::path& relative) {
        std::string k = pathToUtf8(relative.lexically_normal());
        std::ranges::replace(k, '\\', '/');
        return paths::portableNameKey(k);
    }
    bool claim(const std::filesystem::path& relative) { return taken.insert(keyOf(relative)).second; }
    /// `folder / name` with the name made valid on every OS (paths::portableName), or "<stem> (n)<ext>" in that folder
    /// when the name (in any case or Unicode normalisation) is taken.
    std::filesystem::path unique(const std::filesystem::path& folder, const std::filesystem::path& sourceName) {
        const std::filesystem::path name = pathFromUtf8(paths::portableName(pathToUtf8(sourceName)));
        const std::string stem = pathToUtf8(name.stem());
        const std::string ext = pathToUtf8(name.extension());
        std::filesystem::path candidate = folder / name;
        for (int n = 2; !claim(candidate); ++n) {
            candidate = folder / pathFromUtf8(stem + " (" + std::to_string(n) + ")" + ext);
        }
        return candidate;
    }
};

std::string randomSuffix() {
    std::random_device rd;
    return std::to_string(rd() % 1'000'000);
}

/// Removes a folder unless committed (cancel / error: no partial capsule).
struct FolderGuard {
    std::filesystem::path path;
    bool keep = false;
    ~FolderGuard() {
        if (!keep && !path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
};

struct MediaPlan {
    MediaPtr item;
    std::filesystem::path source;
    std::optional<TimeRange> keep; ///< trimmed copy of this range (source time); nullopt = the whole file
    /// 0.2: an item whose path names several files (an image sequence's frames, a spanned clip's parts): they are
    /// copied together into a folder of their own, keeping their names. Empty for one-file items (a layered file's
    /// item copies its PSD like any file).
    std::vector<std::filesystem::path> files;
};

/// 0.2: the files of an image sequence (its frames from the first, up to the first gap) or a spanned clip (its
/// parts), or nothing for an item of one file.
std::vector<std::filesystem::path> filesOfMedia(const std::string& path) {
    std::vector<std::filesystem::path> out;
    if (const auto sequence = parseImageSequencePath(path)) {
        if (auto found = findImageSequence(sequence->firstFramePath(), sequence->frameRate)) {
            for (std::int64_t n = 0; n < found->count; ++n) {
                out.push_back(found->framePath(found->first + n));
            }
        }
    } else if (const auto spanned = parseSpannedClipPath(path)) {
        for (std::size_t i = 0; i < spanned->parts.size(); ++i) {
            out.push_back(spanned->partPath(i));
        }
    }
    return out;
}

std::optional<TimeRange> keepRange(const MediaItem& item, const TimeRange& used, Ticks handles) {
    const Ticks duration = item.info.duration;
    const Ticks start = std::max(Ticks{}, used.start - handles);
    Ticks end = used.end() + handles;
    if (duration.value > 0) {
        end = std::min(end, duration);
    }
    if (!(start < end)) {
        return std::nullopt;
    }
    return TimeRange::fromStartEnd(start, end);
}

std::expected<std::vector<MediaPlan>, Error> planMedia(const ProjectState& state, const std::filesystem::path& projectFile,
                                                       const CapsuleOptions& options) {
    const auto used = usedRanges(state);
    std::vector<MediaPlan> plan;
    for (const MediaPtr& m : state.media) {
        if (!m || m->isGenerator()) {
            continue;
        }
        const auto u = used.find(m->id.value);
        if (u == used.end() && options.excludeUnused) {
            continue;
        }
        const auto found = resolveMediaPath(m->locator, projectFile);
        if (!found || m->locator.offline) {
            return std::unexpected(Error{ErrorCode::NotFound, m->name + " is offline: relink it before collecting"});
        }
        MediaPlan p;
        p.item = m;
        p.source = *found;
        p.files = filesOfMedia(pathToUtf8(*found)); // 0.2: an image sequence or a spanned clip
        if (p.files.empty() && pathToUtf8(*found).find('?') != std::string::npos) {
            p.source = mediaFileToCheck(*found); // a layered file's item: its PSD
        }
        if (options.trim && p.files.empty() && u != used.end() && !m->info.isStillImage &&
            exporter::canTrimCopy(p.source)) {
            p.keep = keepRange(*m, u->second, options.handles);
        }
        plan.push_back(std::move(p));
    }
    return plan;
}

std::int64_t estimate(const MediaPlan& p) {
    if (!p.files.empty()) {
        std::int64_t total = 0;
        for (const std::filesystem::path& f : p.files) {
            total += sizeOf(f);
        }
        return total;
    }
    const std::int64_t size = sizeOf(p.source);
    if (!p.keep || p.item->info.duration.value <= 0) {
        return size;
    }
    // A share of the file, plus a GOP or two on each side.
    const double share = std::clamp(p.keep->duration.toSeconds() / p.item->info.duration.toSeconds(), 0.0, 1.0);
    return std::min(size, static_cast<std::int64_t>(size * share * 1.1) + 4 * 1024 * 1024);
}

/// The asset references ("asset:<id>") effects in the project use.
std::set<std::string> referencedAssets(const ProjectState& state) {
    std::set<std::string> out;
    const auto add = [&](const EffectStack& stack) {
        for (const EffectInstance& e : stack) {
            for (const EffectData& d : e.data) {
                if (d.text()) {
                    out.insert(*d.text());
                }
            }
        }
    };
    for (const SequencePtr& seq : state.sequences) {
        if (!seq) {
            continue;
        }
        add(seq->effects);
        add(seq->audioEffects);
        seq->forEachTrack([&](const Track& t) {
            add(t.effects);
            for (const Clip& c : t.clips) {
                add(c.effects());
            }
        });
    }
    return out;
}

/// The assets the capsule carries: those effects use (or all of them unless unused files are left out).
std::vector<const ProjectAsset*> assetsToCollect(const ProjectState& state, const CapsuleOptions& options) {
    const std::set<std::string> used = referencedAssets(state);
    std::vector<const ProjectAsset*> out;
    for (const ProjectAsset& a : state.assets) {
        if (!options.excludeUnused || used.contains(assetReference(a.id))) {
            out.push_back(&a);
        }
    }
    return out;
}

} // namespace

std::int64_t capsuleBytesNeeded(const ProjectState& state, const std::filesystem::path& projectFile,
                                const CapsuleOptions& options) {
    std::int64_t total = 1024 * 1024; // project + manifest
    if (auto plan = planMedia(state, projectFile, options)) {
        for (const MediaPlan& p : *plan) {
            total += estimate(p);
            if (options.includeProxies && !options.trim && p.item->locator.proxy) {
                total += sizeOf(pathFromUtf8(p.item->locator.proxy->path));
            }
        }
    }
    for (const CapsuleExtraFile& e : options.extras) {
        total += sizeOf(e.source);
    }
    for (const ProjectAsset* a : assetsToCollect(state, options)) {
        if (const auto found = resolveMediaPath(a->locator, projectFile)) {
            total += sizeOf(*found);
        }
    }
    return total;
}

std::expected<CapsuleResult, Error> collectProject(const ProjectState& state, const std::filesystem::path& projectFile,
                                                   const CapsuleOptions& options, std::stop_token stop,
                                                   const std::function<void(const CapsuleProgress&)>& onProgress) {
    const auto cancelled = [] { return std::unexpected(Error{ErrorCode::Cancelled, "Collect Files cancelled"}); };
    const std::string requested =
        !options.name.empty() ? options.name : !projectFile.empty() ? pathToUtf8(projectFile.stem()) : std::string("Untitled");
    // The capsule's own folder: one plain name, valid on every OS (no "../", no separators; I.7 portability audit).
    if (!paths::isPlainName(requested)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "\"" + requested + "\" is not a folder name"});
    }
    const std::string name = paths::portableName(requested);
    const std::filesystem::path folder = options.destination / pathFromUtf8(name);
    const std::filesystem::path finalProject = folder / pathFromUtf8(name + std::string(io::kProjectExtension));
    std::error_code ec;
    if (std::filesystem::exists(folder, ec) && !std::filesystem::is_empty(folder, ec)) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, pathToUtf8(folder) + " already exists"});
    }
    CapsuleProgress progress;
    const auto report = [&](std::string phase, std::string current = {}) {
        progress.phase = std::move(phase);
        progress.current = std::move(current);
        if (onProgress) {
            onProgress(progress);
        }
    };
    report("checking");

    auto plan = planMedia(state, projectFile, options);
    if (!plan) {
        return std::unexpected(plan.error());
    }
    const std::int64_t needed = capsuleBytesNeeded(state, projectFile, options);
    std::filesystem::create_directories(options.destination, ec);
    const auto space = std::filesystem::space(options.destination, ec);
    if (!ec && static_cast<std::int64_t>(space.available) < needed + kSpaceMargin) {
        return std::unexpected(Error{ErrorCode::IoError,
                                     std::format("Not enough disk space in {}: the capsule needs about {} MB, {} MB are free",
                                                 pathToUtf8(options.destination), (needed + kSpaceMargin) >> 20,
                                                 static_cast<std::int64_t>(space.available >> 20))});
    }
    progress.bytesTotal = needed;
    const std::vector<const ProjectAsset*> assets = assetsToCollect(state, options);
    progress.filesTotal = static_cast<int>(plan->size() + options.extras.size() + assets.size());

    FolderGuard temp{options.destination / pathFromUtf8(name + ".wabcut-partial-" + randomSuffix())};
    if (std::filesystem::exists(temp.path, ec)) {
        temp.keep = true; // not ours: never removed
        return std::unexpected(Error{ErrorCode::IoError, pathToUtf8(temp.path) + " already exists"});
    }
    std::filesystem::create_directories(temp.path / "Media", ec);
    if (ec || paths::isLink(temp.path)) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot create " + pathToUtf8(temp.path) + ": " + ec.message()});
    }
    // Names inside the capsule: the project and the manifest first, then every copied file (case-folded).
    CapsuleNames names;
    names.claim(finalProject.filename());
    names.claim("manifest.json");

    // Copies a file in 4 MiB chunks (progress, cancel) and keeps its modification time (caches keyed by it stay
    // valid for the copy). The copy is created exclusively: nothing in the capsule is ever overwritten.
    const auto copyFile = [&](const std::filesystem::path& from, const std::filesystem::path& to) -> std::expected<void, Error> {
        std::filesystem::create_directories(to.parent_path(), ec);
        std::ifstream in(from, std::ios::binary);
        std::ofstream out(to, std::ios::binary | std::ios::noreplace);
        if (!in || !out) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot copy " + pathToUtf8(from) + " to " + pathToUtf8(to)});
        }
        std::vector<char> buffer(4 * 1024 * 1024);
        while (in) {
            if (stop.stop_requested()) {
                return std::unexpected(Error{ErrorCode::Cancelled, "Collect Files cancelled"});
            }
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize n = in.gcount();
            if (n > 0) {
                out.write(buffer.data(), n);
                progress.bytesDone += n;
                report(progress.phase, progress.current);
            }
        }
        out.close();
        if (!out) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot write " + pathToUtf8(to)});
        }
        std::error_code timeEc;
        std::filesystem::last_write_time(to, std::filesystem::last_write_time(from, timeEc), timeEc);
        return {};
    };

    CapsuleResult result;
    ProjectState out = state;
    std::map<std::uint64_t, MediaPtr> replaced;
    std::map<std::uint64_t, Ticks> shifts;
    const std::string proxyFolderName = name + " Proxies";
    for (const MediaPlan& p : *plan) {
        if (stop.stop_requested()) {
            return cancelled();
        }
        if (!p.files.empty()) {
            // 0.2: an image sequence's frames or a spanned clip's parts, together in a folder of their own.
            const std::filesystem::path relativeFolder = names.unique("Media", mediaFileToCheck(p.source).stem());
            for (const std::filesystem::path& f : p.files) {
                report("copying", pathToUtf8(f.filename()));
                const std::filesystem::path relativeFile = relativeFolder / f.filename();
                if (auto c = copyFile(f, temp.path / relativeFile); !c) {
                    return std::unexpected(c.error());
                }
                result.files.push_back({relativeFile, "media", sizeOf(temp.path / relativeFile),
                                        fingerprintFile(temp.path / relativeFile).value_or(SourceFingerprint{}).sampleHash,
                                        pathToUtf8(f), false});
            }
            const std::filesystem::path firstFinal = folder / relativeFolder / p.files.front().filename();
            const std::string finalPath = relinkedMediaPath(pathToUtf8(p.source), firstFinal);
            const auto fingerprint = fingerprintFile(temp.path / relativeFolder / p.files.front().filename());
            if (!fingerprint) {
                return std::unexpected(fingerprint.error());
            }
            auto item = std::make_shared<MediaItem>(*p.item);
            item->locator.path = finalPath;
            item->locator.relativePath = relativeMediaPath(finalProject, pathFromUtf8(finalPath));
            item->locator.fingerprint = *fingerprint;
            item->locator.offline = false;
            item->locator.proxy.reset();
            if (p.item->locator.proxy && options.includeProxies) {
                result.notes.push_back(p.item->name + ": proxy left out (make it again from the collected files)");
            }
            ++result.mediaFiles;
            ++progress.filesDone;
            replaced[p.item->id.value] = std::move(item);
            continue;
        }
        const std::filesystem::path relative = names.unique("Media", p.source.filename());
        const std::filesystem::path staged = temp.path / relative;
        if (std::filesystem::exists(staged, ec)) {
            return std::unexpected(Error{ErrorCode::IoError, pathToUtf8(staged) + " already exists"});
        }
        auto item = std::make_shared<MediaItem>(*p.item);
        bool trimmed = false;
        if (p.keep) {
            report("trimming", pathToUtf8(p.source.filename()));
            const std::int64_t before = progress.bytesDone;
            const std::int64_t share = estimate(p);
            auto copy = exporter::trimmedCopy(pathToUtf8(p.source), staged, *p.keep, stop, [&](double f) {
                progress.bytesDone = before + static_cast<std::int64_t>(f * static_cast<double>(share));
                report("trimming", pathToUtf8(p.source.filename()));
            });
            if (!copy) {
                if (copy.error().code == ErrorCode::Cancelled) {
                    return cancelled();
                }
                result.notes.push_back(p.item->name + ": copied whole (" + copy.error().message + ")");
            } else {
                progress.bytesDone = before + share;
                auto described = describeMediaFile(staged, {});
                if (!described) {
                    return std::unexpected(described.error());
                }
                item->info = described->info;
                shifts[p.item->id.value] = copy->shift;
                trimmed = true;
                ++result.trimmedFiles;
            }
        }
        if (!trimmed) {
            report("copying", pathToUtf8(p.source.filename()));
            if (auto c = copyFile(p.source, staged); !c) {
                return std::unexpected(c.error());
            }
        }
        const auto fingerprint = fingerprintFile(staged);
        if (!fingerprint) {
            return std::unexpected(fingerprint.error());
        }
        const std::filesystem::path finalPath = folder / relative;
        // 0.2: a layered file's item keeps its layers (its path names the PSD and them).
        item->locator.path = relinkedMediaPath(p.item->locator.path, finalPath);
        item->locator.relativePath = relativeMediaPath(finalProject, pathFromUtf8(item->locator.path));
        item->locator.fingerprint = *fingerprint;
        item->locator.offline = false;
        item->locator.proxy.reset();
        if (p.item->locator.proxy && options.includeProxies) {
            const std::filesystem::path proxySource = pathFromUtf8(p.item->locator.proxy->path);
            if (trimmed) {
                result.notes.push_back(p.item->name + ": proxy left out (a trimmed copy needs its own)");
            } else if (isFile(proxySource)) {
                const std::filesystem::path proxyRelative = names.unique(pathFromUtf8(proxyFolderName), proxySource.filename());
                report("copying", pathToUtf8(proxySource.filename()));
                if (auto c = copyFile(proxySource, temp.path / proxyRelative); !c) {
                    return std::unexpected(c.error());
                }
                ProxyMedia proxy = *p.item->locator.proxy;
                proxy.path = pathToUtf8(folder / proxyRelative);
                item->locator.proxy = std::move(proxy);
                result.files.push_back({proxyRelative, "proxy", sizeOf(temp.path / proxyRelative),
                                        fingerprintFile(temp.path / proxyRelative).value_or(SourceFingerprint{}).sampleHash,
                                        pathToUtf8(proxySource), false});
            }
        }
        result.files.push_back({relative, "media", sizeOf(staged), fingerprint->sampleHash, pathToUtf8(p.source), trimmed});
        ++result.mediaFiles;
        ++progress.filesDone;
        replaced[p.item->id.value] = std::move(item);
    }
    for (const CapsuleExtraFile& e : options.extras) {
        if (!paths::isContainedRelative(e.relative)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                                         pathToUtf8(e.relative) + " is not a place inside the capsule"});
        }
        if (!isFile(e.source)) {
            result.notes.push_back(pathToUtf8(e.source.filename()) + ": not found, left out");
            continue;
        }
        const std::filesystem::path relative = names.unique(e.relative.lexically_normal().parent_path(), e.relative.filename());
        report("copying", pathToUtf8(e.source.filename()));
        if (auto c = copyFile(e.source, temp.path / relative); !c) {
            return std::unexpected(c.error());
        }
        result.files.push_back({relative, e.role, sizeOf(temp.path / relative),
                                fingerprintFile(temp.path / relative).value_or(SourceFingerprint{}).sampleHash,
                                pathToUtf8(e.source), false});
        ++progress.filesDone;
    }

    // LUT assets (I.5): copied whole into "LUTs/", their locators pointed there; an offline one stays offline (noted).
    std::vector<ProjectAsset> collectedAssets;
    for (const ProjectAsset* a : assets) {
        if (stop.stop_requested()) {
            return cancelled();
        }
        ProjectAsset copy = *a;
        const auto found = a->locator.offline ? std::nullopt : resolveMediaPath(a->locator, projectFile);
        if (!found) {
            result.notes.push_back(a->name + ": the LUT file is offline, left out (relink it and collect again)");
            collectedAssets.push_back(std::move(copy));
            continue;
        }
        const std::filesystem::path relative = names.unique("LUTs", found->filename());
        report("copying", pathToUtf8(found->filename()));
        if (auto c = copyFile(*found, temp.path / relative); !c) {
            return std::unexpected(c.error());
        }
        const auto fingerprint = fingerprintFile(temp.path / relative);
        const std::filesystem::path finalPath = folder / relative;
        copy.locator.path = pathToUtf8(finalPath);
        copy.locator.relativePath = relativeMediaPath(finalProject, finalPath);
        copy.locator.fingerprint = fingerprint.value_or(SourceFingerprint{});
        copy.locator.offline = false;
        result.files.push_back({relative, "lut", sizeOf(temp.path / relative), copy.locator.fingerprint.sampleHash,
                                pathToUtf8(*found), false});
        ++result.lutFiles;
        ++progress.filesDone;
        collectedAssets.push_back(std::move(copy));
    }
    out.assets = std::move(collectedAssets);

    // The project: collected media only, locators into the capsule, media time moved for trimmed copies.
    report("saving");
    std::vector<MediaPtr> media;
    for (const MediaPtr& m : out.media) {
        if (!m) {
            continue;
        }
        if (m->isGenerator()) {
            media.push_back(m); // titles: no file
        } else if (const auto it = replaced.find(m->id.value); it != replaced.end()) {
            media.push_back(it->second);
        }
    }
    out.media = std::move(media);
    if (!shifts.empty()) {
        for (SequencePtr& seqPtr : out.sequences) {
            if (!seqPtr) {
                continue;
            }
            auto seq = std::make_shared<Sequence>(*seqPtr);
            for (std::vector<TrackPtr>* list : {&seq->videoTracks, &seq->audioTracks}) {
                for (TrackPtr& trackPtr : *list) {
                    if (!trackPtr) {
                        continue;
                    }
                    std::vector<Clip> clips(trackPtr->clips.begin(), trackPtr->clips.end());
                    bool changed = false;
                    for (Clip& clip : clips) {
                        if (const auto s = shifts.find(clip.media.value); s != shifts.end() && s->second.value != 0) {
                            shiftMediaTime(clip, s->second);
                            changed = true;
                        }
                    }
                    if (changed) {
                        auto track = std::make_shared<Track>(*trackPtr);
                        track->clips = ClipList(std::move(clips));
                        trackPtr = std::move(track);
                    }
                }
            }
            seqPtr = std::move(seq);
        }
    }
    const std::filesystem::path stagedProject = temp.path / finalProject.filename();
    if (auto saved = io::saveProject(out, stagedProject, io::SaveOptions{0}); !saved) {
        return std::unexpected(saved.error());
    }
    result.files.insert(result.files.begin(),
                        CapsuleFile{finalProject.filename(), "project", sizeOf(stagedProject),
                                    fingerprintFile(stagedProject).value_or(SourceFingerprint{}).sampleHash,
                                    pathToUtf8(projectFile), false});

    // The manifest.
    nlohmann::json files = nlohmann::json::array();
    for (const CapsuleFile& f : result.files) {
        result.bytes += f.bytes;
        std::string relative = pathToUtf8(f.relative);
        std::ranges::replace(relative, '\\', '/');
        files.push_back({{"path", relative},
                         {"role", f.role},
                         {"bytes", f.bytes},
                         {"sampleHash", f.sampleHash},
                         {"source", f.source},
                         {"trimmed", f.trimmed}});
    }
    const auto now = std::chrono::system_clock::now();
    const nlohmann::json manifest = {
        {"format", "wabcut-capsule"},
        {"version", 1},
        {"wabcutVersion", options.wabcutVersion},
        {"created", std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(now))},
        {"project", pathToUtf8(finalProject.filename())},
        {"sourceProject", pathToUtf8(projectFile)},
        {"trimmed", options.trim},
        {"handlesSeconds", options.handles.toSeconds()},
        {"files", files},
        {"notes", result.notes},
    };
    {
        std::ofstream m(temp.path / "manifest.json", std::ios::binary | std::ios::trunc);
        m << manifest.dump(2);
        if (!m) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot write the manifest"});
        }
    }
    if (stop.stop_requested()) {
        return cancelled();
    }

    if (options.verify) {
        // Before the capsule is committed: copied somewhere else, as if on another machine, only the relative paths
        // can find the media. A capsule that fails is not kept (Codex wave-3 P1-4: never a "successful" damaged one).
        report("verifying");
        std::error_code tempEc;
        const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(tempEc);
        FolderGuard check{tempEc ? std::filesystem::path{} : tempRoot / pathFromUtf8("wabcut-capsule-check-" + randomSuffix())};
        ec = tempEc;
        if (!ec) {
            std::filesystem::create_directories(check.path / folder.filename(), ec);
        }
        if (!ec) {
            std::filesystem::copy(temp.path, check.path / folder.filename(), std::filesystem::copy_options::recursive, ec);
        }
        const std::filesystem::path copiedProject = check.path / folder.filename() / finalProject.filename();
        auto loaded = io::loadProject(copiedProject);
        if (ec || !loaded) {
            result.notes.push_back("verification could not open the copy: " +
                                   (ec ? ec.message() : loaded ? std::string() : loaded.error().message));
        } else {
            ProjectState elsewhere = *loaded;
            for (MediaPtr& m : elsewhere.media) {
                if (m && !m->isGenerator()) {
                    auto moved = std::make_shared<MediaItem>(*m);
                    moved->locator.path.clear(); // the capsule's absolute paths would still find the originals here
                    m = std::move(moved);
                }
            }
            for (ProjectAsset& a : elsewhere.assets) {
                if (!a.locator.offline) {
                    a.locator.path.clear(); // the same for LUTs
                }
            }
            const ProjectState checked = withMediaChecked(elsewhere, copiedProject);
            result.offlineAfterReopen = static_cast<int>(std::ranges::count_if(
                checked.media, [](const MediaPtr& m) { return m && !m->isGenerator() && m->locator.offline; }));
            // LUTs that were collected must be found too (ones already offline were noted above).
            for (const ProjectAsset& a : checked.assets) {
                const ProjectAsset* before = out.findAsset(a.id);
                if (a.locator.offline && before && !before->locator.offline) {
                    ++result.offlineAfterReopen;
                }
            }
            for (const CapsuleFile& f : result.files) {
                const auto fp = fingerprintFile(check.path / folder.filename() / f.relative);
                if (!fp || fp->sampleHash != f.sampleHash) {
                    ++result.fingerprintMismatches;
                }
            }
            result.verified = result.offlineAfterReopen == 0 && result.fingerprintMismatches == 0;
        }
        if (!result.verified) {
            std::string why = result.notes.empty() ? std::string() : " (" + result.notes.back() + ")";
            if (result.fingerprintMismatches > 0 || result.offlineAfterReopen > 0) {
                why = std::format(": {} file(s) differ from what was copied, {} clip(s) would be offline",
                                  result.fingerprintMismatches, result.offlineAfterReopen);
            }
            return std::unexpected(Error{ErrorCode::IoError, "The capsule failed its check and was not kept" + why});
        }
    }

    std::filesystem::remove(folder, ec); // an empty folder of the same name
    std::filesystem::rename(temp.path, folder, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot finish the capsule: " + ec.message()});
    }
    temp.keep = true;
    result.folder = folder;
    result.projectFile = finalProject;
    result.manifest = folder / "manifest.json";
    report("done");
    return result;
}

} // namespace wabcut::editor
