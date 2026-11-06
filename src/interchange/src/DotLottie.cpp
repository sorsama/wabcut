// dotLottie (.lottie) import, 0.7 (card 178): Lottie.h.
//
// The format (https://dotlottie.io/spec/): a ZIP holding "manifest.json" - { version, generator, animations: [{ id, ... }],
// initial: { animation }, themes, stateMachines } - and the animations as Lottie JSON: "animations/<id>.json" in version 2
// files, "a/<id>.json" in version 1 files; images in "images/" ("i/"), themes in "themes/" ("t/") and state machines in
// "states/" ("s/"). Wabcut plays one animation as a motion graphic: the one the caller names, else the manifest's initial
// one, else the first. The ZIP is untrusted: Zip.h reads it bounded, an animation id from the manifest is only ever used
// to build the name of an entry that is then looked up (never a path on disk), and nothing is written.

#include <wabcut/interchange/Lottie.h>
#include <wabcut/interchange/Zip.h>

#include <wabcut/model/ProjectIO.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace wabcut::interchange {

namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxManifestBytes = 1024 * 1024;
constexpr std::size_t kMaxAnimations = 256;

/// An animation id as the files of an archive name it: ASCII letters, digits and "_.- ", at most 128 bytes, no "..".
bool plainId(const std::string& id) {
    if (id.empty() || id.size() > 128 || id.find("..") != std::string::npos) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == '-' || c == ' ';
    });
}

ZipLimits dotLottieLimits() {
    ZipLimits limits;
    limits.maxEntries = 2048;
    limits.maxEntryBytes = kMaxLottieBytes;
    limits.maxTotalBytes = 3 * static_cast<std::uint64_t>(kMaxLottieBytes);
    limits.maxArchiveBytes = kMaxDotLottieBytes;
    return limits;
}

const ZipEntry* animationEntry(const ZipArchive& zip, const std::string& id) {
    for (const char* folder : {"animations/", "a/"}) {
        if (const ZipEntry* e = zip.find(std::string(folder) + id + ".json")) {
            return e;
        }
    }
    return nullptr;
}

struct Manifest {
    std::vector<std::string> ids;
    std::string initial;
    bool hasThemes = false;
    bool hasStates = false;
    bool damaged = false;
};

Manifest readManifest(const ZipArchive& zip) {
    Manifest out;
    if (const ZipEntry* entry = zip.find("manifest.json")) {
        auto text = entry->size <= kMaxManifestBytes ? zip.read(*entry) : std::expected<std::string, Error>(std::string());
        if (entry->size > kMaxManifestBytes || !text || !io::jsonNestingWithin(*text)) {
            out.damaged = true;
        } else {
            const Json root = Json::parse(*text, nullptr, false);
            if (root.is_discarded() || !root.is_object()) {
                out.damaged = true;
            } else {
                if (const auto it = root.find("animations"); it != root.end() && it->is_array()) {
                    for (std::size_t i = 0; i < it->size() && out.ids.size() < kMaxAnimations; ++i) {
                        const Json& a = (*it)[i];
                        if (a.is_object() && a.contains("id") && a["id"].is_string() && plainId(a["id"].get<std::string>())) {
                            out.ids.push_back(a["id"].get<std::string>());
                        }
                    }
                }
                if (const auto it = root.find("initial"); it != root.end() && it->is_object() && it->contains("animation") &&
                                                           (*it)["animation"].is_string()) {
                    out.initial = (*it)["animation"].get<std::string>();
                }
                {
                    const auto it = root.find("themes");
                    out.hasThemes = it != root.end() && it->is_array() && !it->empty();
                }
                for (const char* key : {"stateMachines", "states"}) {
                    const auto it = root.find(key);
                    out.hasStates = out.hasStates || (it != root.end() && it->is_array() && !it->empty());
                }
            }
        }
    }
    return out;
}

/// The animations the archive holds: the manifest's (those with a file), else every animation file by name.
std::vector<std::string> animationIds(const ZipArchive& zip, const Manifest& manifest) {
    std::vector<std::string> ids;
    for (const std::string& id : manifest.ids) {
        if (animationEntry(zip, id) && std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
        }
    }
    if (ids.empty()) {
        for (const ZipEntry& e : zip.entries()) {
            for (const std::string_view folder : {std::string_view("animations/"), std::string_view("a/")}) {
                if (e.isFolder() || e.name.size() <= folder.size() + 5 || e.name.compare(0, folder.size(), folder) != 0 ||
                    e.name.compare(e.name.size() - 5, 5, ".json") != 0) {
                    continue;
                }
                const std::string id = e.name.substr(folder.size(), e.name.size() - folder.size() - 5);
                if (plainId(id) && id.find('/') == std::string::npos && std::find(ids.begin(), ids.end(), id) == ids.end() &&
                    ids.size() < kMaxAnimations) {
                    ids.push_back(id);
                }
            }
        }
    }
    return ids;
}

bool hasFolder(const ZipArchive& zip, std::initializer_list<const char*> folders) {
    return std::any_of(zip.entries().begin(), zip.entries().end(), [&](const ZipEntry& e) {
        return std::any_of(folders.begin(), folders.end(), [&](const char* f) { return e.name.rfind(f, 0) == 0 && !e.isFolder(); });
    });
}

} // namespace

bool looksLikeZip(std::string_view bytes) {
    return bytes.size() >= 4 && bytes[0] == 'P' && bytes[1] == 'K' && (bytes[2] == 3 || bytes[2] == 5) && (bytes[3] == 4 || bytes[3] == 6);
}

std::expected<std::vector<std::string>, Error> dotLottieAnimationIds(std::string_view bytes) {
    if (bytes.size() > kMaxDotLottieBytes) {
        return std::unexpected(Error{ErrorCode::ParseError, "the dotLottie file is larger than 64 MiB"});
    }
    auto zip = ZipArchive::open(bytes, dotLottieLimits());
    if (!zip) {
        return std::unexpected(Error{ErrorCode::ParseError, "dotLottie: " + zip.error().message});
    }
    return animationIds(*zip, readManifest(*zip));
}

std::expected<LottieImport, Error> importDotLottie(std::string_view bytes, std::string_view fileName, std::string_view animationId) {
    if (bytes.size() > kMaxDotLottieBytes) {
        return std::unexpected(Error{ErrorCode::ParseError, "the dotLottie file is larger than 64 MiB"});
    }
    auto zip = ZipArchive::open(bytes, dotLottieLimits());
    if (!zip) {
        return std::unexpected(Error{ErrorCode::ParseError, "dotLottie: " + zip.error().message});
    }
    const Manifest manifest = readManifest(*zip);
    const std::vector<std::string> ids = animationIds(*zip, manifest);
    if (ids.empty()) {
        return std::unexpected(Error{ErrorCode::ParseError, "dotLottie: the archive holds no animation (animations/<id>.json)"});
    }
    std::string chosen = ids.front();
    if (!animationId.empty()) {
        const auto it = std::find(ids.begin(), ids.end(), std::string(animationId));
        if (it == ids.end()) {
            std::string list;
            for (const std::string& id : ids) {
                list += (list.empty() ? "" : ", ") + id;
            }
            return std::unexpected(Error{ErrorCode::ParseError, "dotLottie: no animation '" + std::string(animationId).substr(0, 64) +
                                                                    "' in the file; it holds: " + list.substr(0, 400)});
        }
        chosen = *it;
    } else if (!manifest.initial.empty() && std::find(ids.begin(), ids.end(), manifest.initial) != ids.end()) {
        chosen = manifest.initial;
    }
    const ZipEntry* entry = animationEntry(*zip, chosen);
    auto json = zip->read(*entry);
    if (!json) {
        return std::unexpected(Error{ErrorCode::ParseError, "dotLottie: " + json.error().message});
    }
    auto imported = importLottie(*json, std::string(fileName) + ":" + chosen);
    if (!imported) {
        return imported;
    }
    const auto note = [&](ReportOutcome outcome, std::string reason, const char* where) {
        imported->report.add(outcome, ReportCategory::Graphic, std::move(reason), SourceLocation{std::string(fileName), where, 0});
    };
    if (manifest.damaged) {
        note(ReportOutcome::Approximated, "the archive's manifest.json is damaged: the animation was found by its file name", "/manifest.json");
    }
    if (ids.size() > 1) {
        std::string others;
        for (const std::string& id : ids) {
            if (id != chosen && others.size() < 300) {
                others += (others.empty() ? "" : ", ") + id;
            }
        }
        note(ReportOutcome::Dropped,
             "the file holds " + std::to_string(ids.size()) + " animations; '" + chosen + "' is imported, not " + others +
                 " (choose another with the animation id)",
             "/animations");
    }
    if (manifest.hasThemes || hasFolder(*zip, {"themes/", "t/"})) {
        note(ReportOutcome::Dropped, "dotLottie themes are not applied (the animation keeps its own colors)", "/themes");
    }
    if (manifest.hasStates || hasFolder(*zip, {"states/", "s/"})) {
        note(ReportOutcome::Dropped, "dotLottie state machines are not imported (the animation plays from start to end)", "/stateMachines");
    }
    if (imported->graphic.name.empty()) {
        imported->graphic.name = chosen;
    }
    return imported;
}

} // namespace wabcut::interchange
