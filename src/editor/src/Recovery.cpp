#include <wabcut/editor/Recovery.h>

#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/export/RenderedVideoSource.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <system_error>

namespace wabcut::editor {

namespace {

bool isFile(const std::filesystem::path& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p, ec);
}

void addFileFacts(ProjectSummary& s, const std::filesystem::path& file) {
    std::error_code ec;
    s.bytes = static_cast<std::int64_t>(std::filesystem::file_size(file, ec));
    s.modified = std::filesystem::last_write_time(file, ec);
}

RecoveryEntry entryOf(const std::filesystem::path& file, RecoveryEntry::Kind kind, int index,
                      const std::filesystem::path& project, std::filesystem::file_time_type projectTime) {
    RecoveryEntry e;
    e.file = file;
    e.kind = kind;
    e.index = index;
    e.project = project;
    e.summary = summarizeProjectFile(file);
    e.newerThanProject = kind == RecoveryEntry::Kind::UntitledAutosave || e.summary.modified > projectTime;
    return e;
}

} // namespace

ProjectSummary summarizeState(const ProjectState& state) {
    ProjectSummary s;
    s.loadable = true;
    s.sequences = static_cast<int>(state.sequences.size());
    s.media = static_cast<int>(std::ranges::count_if(state.media, [](const MediaPtr& m) { return m && !m->isGenerator(); }));
    for (const SequencePtr& seq : state.sequences) {
        if (!seq) {
            continue;
        }
        seq->forEachTrack([&](const Track& t) { s.clips += static_cast<int>(t.clips.size()); });
        for (const CaptionTrack& c : seq->captionTracks) {
            s.captionCues += static_cast<int>(c.cues.size());
        }
    }
    if (!state.sequences.empty() && state.sequences.front()) {
        const Sequence& main = *state.sequences.front();
        s.mainSequence = main.name;
        s.duration = std::max(main.duration, main.contentEnd());
        s.frameRate = main.frameRate;
        s.width = main.width;
        s.height = main.height;
    }
    return s;
}

ProjectSummary summarizeProjectFile(const std::filesystem::path& file) {
    ProjectSummary s;
    auto loaded = io::loadProject(file);
    if (loaded) {
        s = summarizeState(*loaded);
    } else {
        s.error = loaded.error().message;
    }
    addFileFacts(s, file);
    return s;
}

std::vector<RecoveryEntry> recoveryEntries(const std::filesystem::path& projectFile, const std::filesystem::path& untitledDir,
                                           int autosaveCopies) {
    std::vector<RecoveryEntry> versions;
    std::error_code ec;
    if (!projectFile.empty()) {
        const bool exists = isFile(projectFile);
        const auto projectTime =
            exists ? std::filesystem::last_write_time(projectFile, ec) : std::filesystem::file_time_type::min();
        for (int n = 1; n <= std::max(1, autosaveCopies); ++n) {
            std::filesystem::path file = io::autosavePath(projectFile);
            if (n > 1) {
                file += std::to_string(n);
            }
            if (isFile(file)) {
                versions.push_back(entryOf(file, RecoveryEntry::Kind::Autosave, n, projectFile, projectTime));
            }
        }
        for (int n = 1; n <= io::kDefaultBackupCount; ++n) {
            const std::filesystem::path file = io::backupPath(projectFile, n);
            if (isFile(file)) {
                versions.push_back(entryOf(file, RecoveryEntry::Kind::Backup, n, projectFile, projectTime));
            }
        }
        std::ranges::stable_sort(versions, [](const RecoveryEntry& a, const RecoveryEntry& b) {
            return a.summary.modified > b.summary.modified;
        });
        if (exists) {
            versions.insert(versions.begin(), entryOf(projectFile, RecoveryEntry::Kind::Current, 0, projectFile, projectTime));
        }
    }
    std::vector<RecoveryEntry> untitled;
    if (!untitledDir.empty()) {
        for (const RecoveryFile& r : ProjectDocument::untitledRecoveryFiles(untitledDir)) {
            untitled.push_back(entryOf(r.file, RecoveryEntry::Kind::UntitledAutosave, 1, {},
                                       std::filesystem::file_time_type::min()));
        }
    }
    versions.insert(versions.end(), std::make_move_iterator(untitled.begin()), std::make_move_iterator(untitled.end()));
    return versions;
}

std::expected<PosterImage, Error> renderPoster(const ProjectState& state, const std::filesystem::path& projectFile,
                                               int width) {
    if (state.sequences.empty() || !state.sequences.front()) {
        return std::unexpected(Error{ErrorCode::NotFound, "The project has no sequence"});
    }
    const ProjectState checked = withMediaChecked(state, projectFile); // relative paths, offline media
    const Sequence& seq = *checked.sequences.front();
    const Rate rate = seq.frameRate.isValid() ? seq.frameRate : Rate{30, 1};
    const Ticks content = seq.contentEnd();
    // A third of the way in, on the frame grid.
    const std::int64_t index = ticksToFrames(Ticks{content.value / 3}, rate, Rounding::Floor).value_or(0);
    const Ticks at = framesToTicks(index, rate, Rounding::Floor).value_or(Ticks{});
    PosterImage poster;
    poster.width = std::max(16, width & ~1);
    poster.height = std::max(2, static_cast<int>(std::lround(static_cast<double>(poster.width) * seq.height /
                                                             std::max(1, seq.width))) & ~1);
    poster.time = at;
    exporter::RenderedSourceOptions options;
    options.renderThreads = 1;
    options.maxInFlight = 1;
    auto source = exporter::renderedVideoSourceFactory(options)(
        checked, seq, exporter::VideoSourceFormat{poster.width, poster.height, rate, at, 1});
    if (!source) {
        return std::unexpected(source.error());
    }
    if (auto r = (*source)->request(0); !r) {
        return std::unexpected(r.error());
    }
    auto picture = (*source)->next();
    (*source)->stop();
    if (!picture) {
        return std::unexpected(picture.error());
    }
    const media::CpuPlanes* cpu = picture->cpu();
    if (!cpu || picture->format != media::PixelFormat::RGBA8) {
        return std::unexpected(Error{ErrorCode::Unsupported, "The renderer delivered no RGBA picture"});
    }
    poster.width = picture->width;
    poster.height = picture->height;
    poster.rgba.resize(static_cast<std::size_t>(poster.width) * poster.height * 4);
    const media::Plane& plane = cpu->planes[0];
    for (int y = 0; y < poster.height; ++y) {
        std::memcpy(poster.rgba.data() + static_cast<std::size_t>(y) * poster.width * 4,
                    plane.data + static_cast<std::ptrdiff_t>(y) * plane.stride, static_cast<std::size_t>(poster.width) * 4);
    }
    return poster;
}

} // namespace wabcut::editor
