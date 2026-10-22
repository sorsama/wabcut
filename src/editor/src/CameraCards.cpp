#include <wabcut/editor/CameraCards.h>

#include <wabcut/editor/MediaImport.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/SpannedClip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <utility>

namespace wabcut::editor {

namespace fs = std::filesystem;

namespace {

std::string upper(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

std::string nameOf(const fs::path& p) { return pathToUtf8(p.filename()); }
std::string stemOf(const fs::path& p) { return pathToUtf8(p.stem()); }
std::string extOf(const fs::path& p) { return upper(pathToUtf8(p.extension())); }

bool isDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

/// The regular files of `dir` (not recursive), sorted by name; at most `limit`.
std::vector<fs::path> filesIn(const fs::path& dir, std::size_t limit = 20000) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code fileEc;
        if (it->is_regular_file(fileEc)) {
            out.push_back(it->path());
            if (out.size() >= limit) {
                break;
            }
        }
    }
    std::ranges::sort(out, [](const fs::path& a, const fs::path& b) { return upper(nameOf(a)) < upper(nameOf(b)); });
    return out;
}

std::vector<fs::path> dirsIn(const fs::path& dir, std::size_t limit = 20000) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code dirEc;
        if (it->is_directory(dirEc)) {
            out.push_back(it->path());
            if (out.size() >= limit) {
                break;
            }
        }
    }
    std::ranges::sort(out, [](const fs::path& a, const fs::path& b) { return upper(nameOf(a)) < upper(nameOf(b)); });
    return out;
}

std::optional<std::string> readSmallFile(const fs::path& p, std::size_t max = 4 * 1024 * 1024) {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec || size > max) {
        return std::nullopt;
    }
    std::ifstream in(p, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!in) {
        return std::nullopt;
    }
    return bytes;
}

// ---- AVCHD ---------------------------------------------------------------------------------------------------

/// The BDMV folder of an AVCHD card root.
std::optional<fs::path> avchdBdmv(const fs::path& root) {
    for (const fs::path candidate : {root / "PRIVATE" / "AVCHD" / "BDMV", root / "AVCHD" / "BDMV", root / "BDMV"}) {
        if (isDir(candidate / "STREAM")) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::uint32_t be32(std::string_view b, std::size_t at) {
    if (at + 4 > b.size()) {
        return 0;
    }
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 3]));
}

std::uint16_t be16(std::string_view b, std::size_t at) {
    if (at + 2 > b.size()) {
        return 0;
    }
    return static_cast<std::uint16_t>((static_cast<unsigned char>(b[at]) << 8) | static_cast<unsigned char>(b[at + 1]));
}

/// A playlist's runs of clip names joined seamlessly (connection condition 5 or 6 joins an item to the one before).
std::vector<std::vector<std::string>> playlistRuns(std::string_view mpls) {
    std::vector<std::vector<std::string>> runs;
    if (mpls.size() < 20 || mpls.substr(0, 4) != "MPLS") {
        return runs;
    }
    const std::size_t list = be32(mpls, 8);
    if (list + 10 > mpls.size()) {
        return runs;
    }
    const int items = be16(mpls, list + 6);
    std::size_t at = list + 10;
    for (int i = 0; i < items && at + 2 + 12 <= mpls.size(); ++i) {
        const std::size_t length = be16(mpls, at);
        const std::string name(mpls.substr(at + 2, 5));
        const int condition = mpls[at + 2 + 9 + 1] & 0x0F;
        if (std::ranges::all_of(name, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
            if (runs.empty() || (condition != 5 && condition != 6)) {
                runs.push_back({name});
            } else {
                runs.back().push_back(name);
            }
        }
        at += 2 + length;
    }
    return runs;
}

std::vector<CameraClip> avchdClips(const fs::path& root) {
    std::vector<CameraClip> out;
    const auto bdmv = avchdBdmv(root);
    if (!bdmv) {
        return out;
    }
    std::map<std::string, fs::path> streams; // "00001" -> its file
    for (const fs::path& f : filesIn(*bdmv / "STREAM")) {
        if (const std::string e = extOf(f); e == ".MTS" || e == ".M2TS") {
            streams[stemOf(f)] = f;
        }
    }
    std::set<std::string> used;
    for (const fs::path& playlist : filesIn(*bdmv / "PLAYLIST")) {
        if (const std::string e = extOf(playlist); e != ".MPL" && e != ".MPLS") {
            continue;
        }
        const auto bytes = readSmallFile(playlist);
        if (!bytes) {
            continue;
        }
        for (const std::vector<std::string>& run : playlistRuns(*bytes)) {
            CameraClip clip;
            clip.card = CameraCard::Avchd;
            for (const std::string& name : run) {
                const auto it = streams.find(name);
                if (it != streams.end() && !used.contains(name)) {
                    clip.files.push_back(it->second);
                    used.insert(name);
                }
            }
            if (!clip.files.empty()) {
                clip.name = stemOf(clip.files.front());
                out.push_back(std::move(clip));
            }
        }
    }
    for (const auto& [name, file] : streams) { // stream files no playlist names: one clip each
        if (!used.contains(name)) {
            out.push_back({name, CameraCard::Avchd, {file}, {}});
        }
    }
    return out;
}

// ---- Canon XF, XDCAM, XAVC --------------------------------------------------------------------------------------

std::vector<CameraClip> canonXfClips(const fs::path& root) {
    std::vector<CameraClip> out;
    for (const fs::path& clips : dirsIn(root / "CONTENTS")) {
        if (!upper(nameOf(clips)).starts_with("CLIPS")) {
            continue;
        }
        for (const fs::path& folder : dirsIn(clips)) {
            CameraClip clip;
            clip.card = CameraCard::CanonXf;
            clip.name = nameOf(folder);
            for (const fs::path& f : filesIn(folder)) {
                if (extOf(f) == ".MXF") {
                    clip.files.push_back(f); // <clip>01.MXF, <clip>02.MXF ...: sorted by name = playing order
                }
            }
            if (!clip.files.empty()) {
                out.push_back(std::move(clip));
            }
        }
    }
    return out;
}

std::vector<CameraClip> xdcamExClips(const fs::path& root) {
    std::vector<CameraClip> out;
    for (const fs::path& folder : dirsIn(root / "BPAV" / "CLPR")) {
        for (const fs::path& f : filesIn(folder)) {
            if (extOf(f) == ".MP4") {
                out.push_back({nameOf(folder), CameraCard::XdcamEx, {f}, {}});
                break;
            }
        }
    }
    return out;
}

std::vector<CameraClip> flatClips(const fs::path& dir, CameraCard card, std::string_view extension) {
    std::vector<CameraClip> out;
    for (const fs::path& f : filesIn(dir)) {
        if (extOf(f) == extension) {
            out.push_back({stemOf(f), card, {f}, {}});
        }
    }
    return out;
}

// ---- P2 --------------------------------------------------------------------------------------------------

/// The text of the first <tag>...</tag> in `xml`, or "".
std::string xmlValue(std::string_view xml, std::string_view tag) {
    const std::string open = "<" + std::string(tag) + ">";
    const std::size_t a = xml.find(open);
    if (a == std::string_view::npos) {
        return {};
    }
    const std::size_t b = xml.find('<', a + open.size());
    return b == std::string_view::npos ? std::string{} : std::string(xml.substr(a + open.size(), b - a - open.size()));
}

std::vector<CameraClip> p2Clips(const fs::path& root) {
    const fs::path contents = root / "CONTENTS";
    struct Piece {
        std::string name;
        std::string shot;
        long long offset = 0;
        fs::path video;
        std::vector<fs::path> audio;
    };
    std::vector<Piece> pieces;
    const std::vector<fs::path> audioFiles = filesIn(contents / "AUDIO");
    for (const fs::path& f : filesIn(contents / "VIDEO")) {
        if (extOf(f) != ".MXF") {
            continue;
        }
        Piece p;
        p.name = stemOf(f);
        p.video = f;
        for (const fs::path& a : audioFiles) { // <clip>00.MXF, <clip>01.MXF, ...: one per channel
            const std::string stem = stemOf(a);
            if (extOf(a) == ".MXF" && stem.size() == p.name.size() + 2 && upper(stem).starts_with(upper(p.name))) {
                p.audio.push_back(a);
            }
        }
        if (const auto xml = readSmallFile(contents / "CLIP" / (p.name + ".XML"))) {
            p.shot = xmlValue(*xml, "GlobalShotID");
            try {
                p.offset = std::stoll(xmlValue(*xml, "OffsetInShot"));
            } catch (...) {
                p.offset = 0;
            }
        }
        pieces.push_back(std::move(p));
    }
    // Clips of one shot (a recording that spanned cards or files) in their order in the shot.
    std::map<std::string, std::vector<Piece>> shots;
    std::vector<CameraClip> out;
    for (Piece& p : pieces) {
        if (p.shot.empty()) {
            CameraClip clip{p.name, CameraCard::P2, {p.video}, {}};
            for (const fs::path& a : p.audio) {
                clip.audio.push_back({a});
            }
            out.push_back(std::move(clip));
        } else {
            shots[p.shot].push_back(std::move(p));
        }
    }
    for (auto& [shot, list] : shots) {
        std::ranges::stable_sort(list, [](const Piece& a, const Piece& b) { return a.offset < b.offset; });
        CameraClip clip;
        clip.card = CameraCard::P2;
        clip.name = list.front().name;
        const std::size_t channels = list.front().audio.size();
        clip.audio.resize(channels);
        for (const Piece& p : list) {
            clip.files.push_back(p.video);
            for (std::size_t c = 0; c < channels; ++c) {
                if (c < p.audio.size()) {
                    clip.audio[c].push_back(p.audio[c]);
                }
            }
        }
        std::erase_if(clip.audio, [&](const std::vector<fs::path>& a) { return a.size() != clip.files.size(); });
        out.push_back(std::move(clip));
    }
    return out;
}

// ---- GoPro ------------------------------------------------------------------------------------------------

/// A GoPro file's recording and chapter: GX010042.MP4 -> ("GX0042", 1); GOPR0042.MP4 -> ("GP0042", 0);
/// GP010042.MP4 -> ("GP0042", 1).
std::optional<std::pair<std::string, int>> goProChapter(const fs::path& f) {
    const std::string name = upper(nameOf(f));
    if (name.size() != 12 || !name.ends_with(".MP4")) {
        return std::nullopt;
    }
    const auto digits = [&](std::size_t from, std::size_t n) {
        return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(from),
                           name.begin() + static_cast<std::ptrdiff_t>(from + n),
                           [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
    };
    if (name.starts_with("GOPR") && digits(4, 4)) {
        return std::pair{"GP" + name.substr(4, 4), 0};
    }
    if (name[0] == 'G' && std::string_view("HXLPS").find(name[1]) != std::string_view::npos && digits(2, 6)) {
        return std::pair{name.substr(0, 2) + name.substr(4, 4), std::stoi(name.substr(2, 2))};
    }
    return std::nullopt;
}

/// The media files of a plain folder as clips, GoPro chapters joined (each recording named after its first chapter).
std::vector<CameraClip> plainClips(const std::vector<fs::path>& files) {
    std::vector<CameraClip> out;
    std::map<std::string, std::vector<std::pair<int, fs::path>>> chapters;
    for (const fs::path& f : files) {
        if (!isBrowsableMedia(f)) {
            continue;
        }
        if (const auto chapter = goProChapter(f)) {
            chapters[chapter->first].emplace_back(chapter->second, f);
        } else {
            out.push_back({stemOf(f), CameraCard::None, {f}, {}});
        }
    }
    for (auto& [recording, list] : chapters) {
        std::ranges::sort(list, [](const auto& a, const auto& b) { return a.first < b.first; });
        CameraClip clip;
        for (const auto& [n, f] : list) {
            clip.files.push_back(f);
        }
        clip.name = stemOf(clip.files.front());
        out.push_back(std::move(clip));
    }
    std::ranges::sort(out, [](const CameraClip& a, const CameraClip& b) { return upper(a.name) < upper(b.name); });
    return out;
}

/// Microseconds `file` plays: its video stream's duration (whole frames), else the container's.
std::expected<std::int64_t, Error> partMicroseconds(const fs::path& file) {
    auto info = media::probe(pathToUtf8(file));
    if (!info) {
        return std::unexpected(info.error());
    }
    Ticks d = info->duration;
    if (!info->video.empty() && info->video.front().duration.value > 0) {
        d = info->video.front().duration;
    } else if (!info->audio.empty() && info->audio.front().duration.value > 0 && d.value <= 0) {
        d = info->audio.front().duration;
    }
    const auto micros = mulDiv(d.value, 1'000'000, kTicksPerSecond, Rounding::Nearest);
    if (!micros || *micros <= 0) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Cannot tell how long " + pathToUtf8(file) + " plays"});
    }
    return *micros;
}

} // namespace

std::string_view displayName(CameraCard card) {
    switch (card) {
    case CameraCard::Avchd:
        return "AVCHD";
    case CameraCard::CanonXf:
        return "Canon XF";
    case CameraCard::P2:
        return "P2";
    case CameraCard::XdcamEx:
        return "XDCAM EX";
    case CameraCard::XdcamHd:
        return "XDCAM HD";
    case CameraCard::Xavc:
        return "XAVC";
    case CameraCard::None:
        break;
    }
    return "";
}

std::uintmax_t CameraClip::bytes() const {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const fs::path& f : files) {
        const auto size = fs::file_size(f, ec);
        total += ec ? 0 : size;
    }
    for (const auto& channel : audio) {
        for (const fs::path& f : channel) {
            const auto size = fs::file_size(f, ec);
            total += ec ? 0 : size;
        }
    }
    return total;
}

bool isBrowsableMedia(const fs::path& file) {
    static constexpr std::array<std::string_view, 48> kExtensions{
        ".MP4", ".MOV", ".MKV", ".MXF", ".AVI", ".M4V", ".WEBM", ".MTS", ".M2TS", ".TS", ".MPG", ".MPEG",
        ".M2V", ".DV", ".FLV", ".WMV", ".3GP", ".WAV", ".MP3", ".AAC", ".M4A", ".FLAC", ".OGG", ".OPUS",
        ".AIF", ".AIFF", ".WMA", ".PNG", ".JPG", ".JPEG", ".TIF", ".TIFF", ".BMP", ".GIF", ".TGA", ".DPX",
        ".EXR", ".WEBP", ".HEIC", ".PSD", ".PSB", ".DNG", ".R3D", ".BRAW", ".ARI", ".CRM", ".INSV", ".LRV"};
    const std::string e = extOf(file);
    return e != ".LRV" && std::ranges::find(kExtensions, e) != kExtensions.end();
}

std::optional<std::pair<CameraCard, fs::path>> cameraCardAt(const fs::path& folder) {
    std::error_code ec;
    fs::path at = fs::absolute(folder, ec);
    for (int depth = 0; depth < 8 && !at.empty(); ++depth) {
        if (avchdBdmv(at) && (isDir(at / "PRIVATE" / "AVCHD") || isDir(at / "AVCHD") || isDir(at / "BDMV" / "PLAYLIST"))) {
            return std::pair{CameraCard::Avchd, at};
        }
        if (isDir(at / "CONTENTS" / "VIDEO") && isDir(at / "CONTENTS" / "CLIP")) {
            return std::pair{CameraCard::P2, at};
        }
        if (isDir(at / "CONTENTS")) {
            for (const fs::path& d : dirsIn(at / "CONTENTS", 64)) {
                if (upper(nameOf(d)).starts_with("CLIPS")) {
                    return std::pair{CameraCard::CanonXf, at};
                }
            }
        }
        if (isDir(at / "BPAV" / "CLPR")) {
            return std::pair{CameraCard::XdcamEx, at};
        }
        if (isDir(at / "PRIVATE" / "M4ROOT" / "CLIP")) {
            return std::pair{CameraCard::Xavc, at};
        }
        if (isDir(at / "Clip") && (isFile(at / "MEDIAPRO.XML") || isFile(at / "DISCMETA.XML") || isDir(at / "Sub"))) {
            return std::pair{CameraCard::XdcamHd, at};
        }
        const fs::path parent = at.parent_path();
        if (parent == at) {
            break;
        }
        at = parent;
    }
    return std::nullopt;
}

std::vector<CameraClip> cameraCardClips(CameraCard kind, const fs::path& root) {
    std::vector<CameraClip> out;
    switch (kind) {
    case CameraCard::Avchd:
        out = avchdClips(root);
        break;
    case CameraCard::CanonXf:
        out = canonXfClips(root);
        break;
    case CameraCard::P2:
        out = p2Clips(root);
        break;
    case CameraCard::XdcamEx:
        out = xdcamExClips(root);
        break;
    case CameraCard::XdcamHd:
        out = flatClips(root / "Clip", CameraCard::XdcamHd, ".MXF");
        break;
    case CameraCard::Xavc:
        out = flatClips(root / "PRIVATE" / "M4ROOT" / "CLIP", CameraCard::Xavc, ".MP4");
        break;
    case CameraCard::None:
        break;
    }
    std::ranges::stable_sort(out, [](const CameraClip& a, const CameraClip& b) { return upper(a.name) < upper(b.name); });
    return out;
}

std::vector<BrowserEntry> browseFolder(const fs::path& folder, std::size_t limit) {
    std::vector<BrowserEntry> out;
    if (const auto card = cameraCardAt(folder)) {
        for (CameraClip& clip : cameraCardClips(card->first, card->second)) {
            if (out.size() >= limit) {
                break;
            }
            BrowserEntry e;
            e.name = clip.name;
            e.path = clip.files.front();
            e.clip = std::move(clip);
            out.push_back(std::move(e));
        }
        return out;
    }
    for (const fs::path& d : dirsIn(folder)) {
        const std::string name = nameOf(d);
        if (name.empty() || name.front() == '.' || name.front() == '$' || upper(name) == "SYSTEM VOLUME INFORMATION") {
            continue;
        }
        if (out.size() >= limit) {
            return out;
        }
        out.push_back({true, name, d, std::nullopt});
    }
    for (CameraClip& clip : plainClips(filesIn(folder))) {
        if (out.size() >= limit) {
            break;
        }
        BrowserEntry e;
        e.name = clip.name;
        e.path = clip.files.front();
        e.clip = std::move(clip);
        out.push_back(std::move(e));
    }
    return out;
}

CameraClip cameraClipOf(const fs::path& file) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(file, ec);
    if (const auto card = cameraCardAt(absolute.parent_path())) {
        for (CameraClip& clip : cameraCardClips(card->first, card->second)) {
            if (!clip.files.empty() && fs::equivalent(clip.files.front(), absolute, ec)) {
                return clip;
            }
        }
    }
    if (goProChapter(absolute)) {
        for (CameraClip& clip : plainClips(filesIn(absolute.parent_path()))) {
            if (std::ranges::any_of(clip.files, [&](const fs::path& f) { return fs::equivalent(f, absolute, ec); })) {
                return clip;
            }
        }
    }
    return {stemOf(absolute), CameraCard::None, {absolute}, {}};
}

std::expected<CameraClip, Error> copyCameraClip(const CameraClip& clip, const fs::path& destination) {
    std::error_code ec;
    std::string folderName;
    for (const char c : clip.name) { // the clip's name as a folder name: no separators or reserved characters
        folderName += std::string_view("<>:\"/\\|?*").find(c) == std::string_view::npos && c >= ' ' ? c : '_';
    }
    if (folderName.empty() || folderName == "." || folderName == "..") {
        folderName = "Clip";
    }
    const fs::path folder = destination / pathFromUtf8(folderName);
    fs::create_directories(folder, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::IoError, "Cannot create " + pathToUtf8(folder) + ": " + ec.message()});
    }
    const auto copy = [&](const fs::path& from) -> std::expected<fs::path, Error> {
        const fs::path to = folder / from.filename();
        std::error_code sizeEc;
        if (fs::exists(to, sizeEc) && fs::file_size(to, sizeEc) == fs::file_size(from, sizeEc) && !sizeEc) {
            return to; // copied before
        }
        std::error_code copyEc;
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, copyEc);
        if (copyEc) {
            return std::unexpected(Error{ErrorCode::IoError, "Cannot copy " + pathToUtf8(from) + ": " + copyEc.message()});
        }
        return to;
    };
    CameraClip out = clip;
    for (fs::path& f : out.files) {
        auto to = copy(f);
        if (!to) {
            return std::unexpected(to.error());
        }
        f = *to;
    }
    for (auto& channel : out.audio) {
        for (fs::path& f : channel) {
            auto to = copy(f);
            if (!to) {
                return std::unexpected(to.error());
            }
            f = *to;
        }
    }
    return out;
}

std::expected<std::string, Error> spannedMediaPath(const std::vector<fs::path>& files) {
    if (files.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "no files"});
    }
    if (files.size() == 1) {
        return pathToUtf8(files.front());
    }
    SpannedClipSpec spec;
    spec.directory = files.front().parent_path();
    for (const fs::path& f : files) {
        if (f.parent_path() != spec.directory) {
            return std::unexpected(Error{ErrorCode::Unsupported, "The parts of a spanned clip must be in one folder"});
        }
        auto micros = partMicroseconds(f);
        if (!micros) {
            return std::unexpected(micros.error());
        }
        spec.parts.push_back({nameOf(f), *micros});
    }
    std::string path = spannedClipPath(spec);
    if (path.empty()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "The spanned clip's files cannot be joined"});
    }
    return path;
}

std::expected<std::vector<MediaItem>, Error> describeCameraClip(const CameraClip& clip, const fs::path& projectFile) {
    std::vector<MediaItem> out;
    const auto describe = [&](const std::vector<fs::path>& files, std::string name) -> std::expected<void, Error> {
        auto path = spannedMediaPath(files);
        if (!path) {
            return std::unexpected(path.error());
        }
        auto item = describeMediaFile(pathFromUtf8(*path), projectFile);
        if (!item) {
            return std::unexpected(item.error());
        }
        item->name = std::move(name);
        out.push_back(std::move(*item));
        return {};
    };
    if (auto video = describe(clip.files, clip.name); !video) {
        return std::unexpected(video.error());
    }
    for (std::size_t c = 0; c < clip.audio.size(); ++c) {
        if (auto audio = describe(clip.audio[c], clip.name + " A" + std::to_string(c + 1)); !audio) {
            return std::unexpected(audio.error());
        }
    }
    return out;
}

} // namespace wabcut::editor
