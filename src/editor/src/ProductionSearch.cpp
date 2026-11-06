#include <wabcut/editor/ProductionSearch.h>

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/TextFind.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <unordered_map>

namespace wabcut::editor {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxTokens = 16;

/// `text` cut to kMaxIndexedFieldBytes at a code point boundary.
std::string clipped(std::string_view text) {
    if (text.size() <= kMaxIndexedFieldBytes) {
        return std::string(text);
    }
    std::size_t end = kMaxIndexedFieldBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return std::string(text.substr(0, end));
}

bool isOneOf(std::string_view value, const std::string_view* begin, const std::string_view* end) {
    return std::find(begin, end, value) != end;
}

bool isField(std::string_view value) {
    return isOneOf(value, std::begin(kProductionSearchFields), std::end(kProductionSearchFields));
}
bool isType(std::string_view value) {
    return isOneOf(value, std::begin(kProductionSearchTypes), std::end(kProductionSearchTypes));
}

/// The bins' paths ("Footage / Day 1") by bin id; search bins hold nothing and are left out. A damaged project (a parent
/// loop) cannot make this run away: the climb stops after 32 levels.
std::unordered_map<std::uint64_t, std::string> binPaths(const ProjectState& state) {
    std::unordered_map<std::uint64_t, const Bin*> byId;
    for (const Bin& bin : state.bins) {
        if (!bin.isSearch()) {
            byId.emplace(bin.id.value, &bin);
        }
    }
    std::unordered_map<std::uint64_t, std::string> out;
    for (const auto& [id, bin] : byId) {
        std::vector<const Bin*> chain;
        const Bin* at = bin;
        for (int depth = 0; at && depth < 32; ++depth) {
            chain.push_back(at);
            if (!at->parent.isValid()) {
                break;
            }
            const auto up = byId.find(at->parent.value);
            at = up == byId.end() ? nullptr : up->second;
        }
        std::string path;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            if (!path.empty()) {
                path += " / ";
            }
            path += (*it)->name;
        }
        out.emplace(id, clipped(path));
    }
    return out;
}

void addField(SearchItem& item, const char* field, std::string_view text) {
    if (text.empty()) {
        return;
    }
    SearchItem::Field f;
    f.field = field;
    f.text = clipped(text);
    f.folded = foldCase(f.text);
    item.fields.push_back(std::move(f));
}

std::string markerText(const std::vector<Marker>& markers) {
    std::string out;
    for (const Marker& m : markers) {
        if (m.name.empty() && m.comment.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += '\n';
        }
        out += m.name;
        if (!m.comment.empty()) {
            out += out.empty() || out.back() == '\n' ? "" : " ";
            out += m.comment;
        }
        if (out.size() >= kMaxIndexedFieldBytes) {
            break;
        }
    }
    return out;
}

std::string kindOf(const MediaItem& m) {
    if (m.isSubclip()) {
        return "subclip";
    }
    if (m.isGenerator()) {
        return "title";
    }
    if (m.info.isStillImage) {
        return "still";
    }
    return m.info.hasVideo() ? "video" : "audio";
}

bool wordStart(const std::string& text, std::size_t at) {
    if (at == 0) {
        return true;
    }
    const unsigned char before = static_cast<unsigned char>(text[at - 1]);
    return before < 0x80 && !std::isalnum(before);
}

/// How well `token` matches `folded` as the `field` of an item; 0 = not at all.
int fieldScore(std::string_view field, const std::string& folded, const std::string& token) {
    const std::size_t at = folded.find(token);
    if (at == std::string::npos) {
        return 0;
    }
    if (field == "name") {
        if (folded == token) {
            return 100;
        }
        if (at == 0) {
            return 80;
        }
        return wordStart(folded, at) ? 70 : 60;
    }
    if (field == "path") {
        const std::size_t fileName = folded.find_last_of("/\\");
        return fileName != std::string::npos && at > fileName ? 35 : 25; // the file's own name counts more than its folders
    }
    if (field == "marker") {
        return 15;
    }
    if (field == "bin") {
        return 10;
    }
    return 20;
}

/// The words of a query: quoted text is one phrase; everything is case folded; at most kMaxTokens.
std::vector<std::string> tokensOf(std::string_view text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (i < text.size() && out.size() < kMaxTokens) {
        while (i < text.size() && space(text[i])) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        std::string token;
        if (text[i] == '"') {
            const std::size_t close = text.find('"', i + 1);
            const std::size_t end = close == std::string_view::npos ? text.size() : close;
            token = std::string(text.substr(i + 1, end - i - 1));
            i = close == std::string_view::npos ? text.size() : close + 1;
            while (!token.empty() && space(token.back())) {
                token.pop_back();
            }
            std::size_t first = 0;
            while (first < token.size() && space(token[first])) {
                ++first;
            }
            token.erase(0, first);
        } else {
            const std::size_t start = i;
            while (i < text.size() && !space(text[i])) {
                ++i;
            }
            token = std::string(text.substr(start, i - start));
        }
        if (!token.empty()) {
            out.push_back(foldCase(token));
        }
    }
    return out;
}

bool validUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const std::size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += length;
    }
    return true;
}

long long writeTimeOf(const fs::path& file, std::error_code& ec) {
    const auto written = fs::last_write_time(file, ec);
    // long long: libc++'s file clock counts in an __int128.
    return ec ? 0LL : static_cast<long long>(written.time_since_epoch().count());
}

} // namespace

ProjectIndex indexProject(const ProjectState& state) {
    ProjectIndex out;
    const auto bins = binPaths(state);
    const auto binOf = [&](BinId bin) -> std::string {
        if (!bin.isValid()) {
            return {};
        }
        const auto found = bins.find(bin.value);
        return found == bins.end() ? std::string() : found->second;
    };
    for (const SequencePtr& q : state.sequences) {
        if (!q) {
            continue;
        }
        if (out.items.size() >= kMaxIndexedItemsPerProject) {
            out.truncated = true;
            break;
        }
        SearchItem item;
        item.sequence = true;
        item.id = q->id.value;
        item.kind = "sequence";
        item.name = clipped(q->name);
        item.nameFolded = foldCase(item.name);
        item.location = binOf(q->bin);
        item.width = q->width;
        item.height = q->height;
        item.duration = q->duration;
        item.frameRate = q->frameRate;
        addField(item, "name", q->name);
        addField(item, "marker", markerText(q->markers));
        addField(item, "bin", item.location);
        out.items.push_back(std::move(item));
    }
    for (const MediaPtr& m : state.media) {
        if (!m) {
            continue;
        }
        if (out.items.size() >= kMaxIndexedItemsPerProject) {
            out.truncated = true;
            break;
        }
        SearchItem item;
        item.id = m->id.value;
        item.kind = kindOf(*m);
        item.name = clipped(m->name);
        item.nameFolded = foldCase(item.name);
        item.location = binOf(m->bin);
        item.duration = m->info.duration;
        if (m->info.hasVideo()) {
            item.width = m->info.video.front().width;
            item.height = m->info.video.front().height;
            item.frameRate = m->info.video.front().frameRate;
        }
        item.offline = !m->isGenerator() && m->locator.offline;
        const ClipMetadata& md = m->metadata;
        addField(item, "name", m->name);
        addField(item, "comment", m->comment);
        addField(item, "description", m->description);
        addField(item, "scene", md.scene);
        addField(item, "shot", md.shot);
        addField(item, "take", md.take);
        addField(item, "logNote", md.logNote);
        addField(item, "client", md.client);
        addField(item, "tapeName", m->timecode ? m->timecode->reelName : std::string());
        addField(item, "status", md.status);
        addField(item, "cameraAngle", md.cameraAngle);
        addField(item, "cameraLabel", md.cameraLabel);
        addField(item, "cameraModel", md.cameraModel);
        addField(item, "shotDate", md.shotDate);
        addField(item, "shotLocation", md.shotLocation);
        addField(item, "path", m->locator.path);
        addField(item, "marker", markerText(m->markers));
        addField(item, "bin", item.location);
        out.items.push_back(std::move(item));
    }
    return out;
}

ProductionIndex scanProduction(const Production& production, const ScanOptions& options, std::stop_token stop) {
    ProductionIndex out;
    out.folder = production.folder();
    std::vector<ProductionEntry> projects;
    for (ProductionEntry& entry : production.entries()) {
        if (!entry.folder) {
            projects.push_back(std::move(entry));
        }
    }
    const int total = static_cast<int>(projects.size());
    int done = 0;
    for (const ProductionEntry& entry : projects) {
        if (stop.stop_requested()) {
            break;
        }
        if (options.progress) {
            options.progress(done, total, pathToUtf8(entry.path.filename()));
            if (stop.stop_requested()) {
                break; // asked to stop by the progress callback itself (a Stop button pressed meanwhile)
            }
        }
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(entry.path, ec);
        const bool sized = !ec;
        const long long written = writeTimeOf(entry.path, ec);
        const bool stamped = !ec;
        const ProjectIndex* before = nullptr;
        if (options.previous) {
            const auto found = std::find_if(options.previous->projects.begin(), options.previous->projects.end(),
                                            [&](const ProjectIndex& p) { return p.file == entry.path; });
            before = found == options.previous->projects.end() ? nullptr : &*found;
        }
        ProjectIndex index;
        if (const auto open = options.openState ? options.openState(entry.path) : nullptr) {
            index = indexProject(*open); // as it is in memory: unsaved changes included
            index.open = true;
        } else if (before && !before->open && sized && stamped && before->size == static_cast<std::int64_t>(size) &&
                   before->modified == written) {
            index = *before; // the file did not change (an error stays too: it is read again when the file is saved)
            index.open = false;
        } else {
            auto loaded = io::loadProject(entry.path);
            if (loaded) {
                index = indexProject(*loaded);
            } else {
                index.error = loaded.error().message;
            }
        }
        index.file = entry.path;
        index.name = entry.name;
        index.relative = entry.relative;
        index.size = sized ? static_cast<std::int64_t>(size) : -1;
        index.modified = stamped ? written : 0;
        out.projects.push_back(std::move(index));
        ++done;
    }
    if (options.progress && !stop.stop_requested()) {
        options.progress(done, total, {});
    }
    return out;
}

std::optional<std::string> searchProblem(const SearchQuery& query) {
    if (query.text.size() > kMaxSearchTextBytes) {
        return "the text is longer than " + std::to_string(kMaxSearchTextBytes) + " bytes";
    }
    if (!validUtf8(query.text)) {
        return "the text is not UTF-8";
    }
    if (!query.field.empty() && !isField(query.field)) {
        return "unknown field '" + query.field + "'";
    }
    if (!query.type.empty() && !isType(query.type)) {
        return "unknown type '" + query.type + "'";
    }
    if (query.project.size() > 4096) {
        return "the project name is too long";
    }
    return std::nullopt;
}

SearchResult searchProduction(const ProductionIndex& index, const SearchQuery& query) {
    SearchResult result;
    if (searchProblem(query)) {
        return result;
    }
    const std::vector<std::string> tokens = tokensOf(query.text);
    const bool listing = tokens.empty();
    if (listing && query.type.empty()) {
        return result; // nothing asked
    }
    struct Candidate {
        const ProjectIndex* project = nullptr;
        const SearchItem* item = nullptr;
        int score = 0;
        std::vector<std::string> matched;
    };
    std::vector<Candidate> found;
    for (const ProjectIndex& project : index.projects) {
        if (!query.project.empty() && project.relative != query.project && project.name != query.project) {
            continue;
        }
        ++result.projects;
        if (!project.error.empty()) {
            result.unreadable.push_back(pathToUtf8(project.file.filename()) + ": " + project.error);
            continue;
        }
        for (const SearchItem& item : project.items) {
            if (!query.type.empty() && item.kind != query.type) {
                continue;
            }
            Candidate c;
            c.project = &project;
            c.item = &item;
            bool all = true;
            for (const std::string& token : tokens) {
                int best = 0;
                const std::string* bestField = nullptr;
                for (const SearchItem::Field& f : item.fields) {
                    if (!query.field.empty() && f.field != query.field) {
                        continue;
                    }
                    const int s = fieldScore(f.field, f.folded, token);
                    if (s > best) {
                        best = s;
                        bestField = &f.field;
                    }
                }
                if (best == 0) {
                    all = false;
                    break;
                }
                c.score += best;
                if (std::find(c.matched.begin(), c.matched.end(), *bestField) == c.matched.end()) {
                    c.matched.push_back(*bestField);
                }
            }
            if (all) {
                found.push_back(std::move(c));
            }
        }
    }
    result.total = found.size();
    std::stable_sort(found.begin(), found.end(), [&](const Candidate& a, const Candidate& b) {
        if (!listing && a.score != b.score) {
            return a.score > b.score;
        }
        if (listing && a.project != b.project) {
            return a.project->name < b.project->name;
        }
        if (a.item->nameFolded != b.item->nameFolded) {
            return a.item->nameFolded < b.item->nameFolded;
        }
        if (a.project->name != b.project->name) {
            return a.project->name < b.project->name;
        }
        return a.item->id < b.item->id;
    });
    const std::size_t limit = std::min(query.limit == 0 ? std::size_t{200} : query.limit, kMaxSearchHits);
    for (std::size_t i = 0; i < found.size() && i < limit; ++i) {
        const Candidate& c = found[i];
        SearchHit hit;
        hit.project = c.project->file;
        hit.projectName = c.project->name;
        hit.projectRelative = c.project->relative;
        hit.projectOpen = c.project->open;
        hit.id = c.item->id;
        hit.sequence = c.item->sequence;
        hit.kind = c.item->kind;
        hit.name = c.item->name;
        hit.location = c.item->location;
        hit.width = c.item->width;
        hit.height = c.item->height;
        hit.duration = c.item->duration;
        hit.frameRate = c.item->frameRate;
        hit.offline = c.item->offline;
        hit.matched = c.matched;
        hit.score = c.score;
        result.hits.push_back(std::move(hit));
    }
    return result;
}

} // namespace wabcut::editor
