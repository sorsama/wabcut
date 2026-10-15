#include <wabcut/script/Extension.h>

#include <wabcut/ai/Sha256.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <fstream>

namespace wabcut::script {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxFoundExtensions = 256;

/// Well-formed UTF-8 (no overlong forms, surrogates or code points past U+10FFFF) without NUL bytes.
bool isCleanUtf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b == 0) {
            return false;
        }
        if (b < 0x80) {
            ++i;
            continue;
        }
        const int extra = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
        if (extra < 0 || i + static_cast<std::size_t>(extra) >= s.size()) {
            return false;
        }
        char32_t cp = b & (extra == 1 ? 0x1Fu : extra == 2 ? 0x0Fu : 0x07u);
        for (int k = 1; k <= extra; ++k) {
            const auto c = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
            if ((c & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (c & 0x3Fu);
        }
        const char32_t minimum = extra == 1 ? 0x80 : extra == 2 ? 0x800 : 0x10000;
        if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

/// Whether `path` (below `root`) and every folder between them are plain directories / a plain file: no symbolic
/// links, junctions or other reparse points that could lead outside the extension.
bool isPlainPathBelow(const fs::path& root, const std::string& relative, bool file) {
    std::error_code ec;
    fs::path at = root;
    const fs::path rel = pathFromUtf8(relative);
    std::size_t index = 0;
    const std::size_t count = static_cast<std::size_t>(std::distance(rel.begin(), rel.end()));
    for (const fs::path& part : rel) {
        at /= part;
        const fs::file_status status = fs::symlink_status(at, ec);
        if (ec) {
            return false;
        }
        const bool last = ++index == count;
        const fs::file_type want = last && file ? fs::file_type::regular : fs::file_type::directory;
        if (status.type() != want) {
            return false;
        }
    }
    return true;
}

std::expected<std::string, Error> readBounded(const fs::path& file, std::size_t maxBytes, const std::string& name) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(Error{ErrorCode::IoError, "cannot read " + name});
    }
    std::string text(maxBytes + 1, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    const auto got = static_cast<std::size_t>(in.gcount());
    if (got > maxBytes) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     name + " is larger than " + std::to_string(maxBytes / 1024) + " KiB"});
    }
    text.resize(got);
    return text;
}

void hashPart(ai::Sha256& hash, std::string_view name, std::string_view bytes) {
    hash.update(name);
    hash.update(std::string_view("\0", 1));
    hash.update(std::to_string(bytes.size()));
    hash.update(std::string_view("\0", 1));
    hash.update(bytes);
}

} // namespace

std::string_view toString(ExtensionScope scope) { return scope == ExtensionScope::User ? "user" : "project"; }

std::expected<ExtensionBundle, Error> loadExtension(const fs::path& folder, ExtensionScope scope,
                                                    const std::optional<Version>& running) {
    std::error_code ec;
    const fs::path root = fs::canonical(folder, ec);
    if (ec || !fs::is_directory(root, ec)) {
        return std::unexpected(Error{ErrorCode::NotFound, "no extension folder " + pathToUtf8(folder)});
    }
    if (!isPlainPathBelow(root, std::string(kManifestFileName), true)) {
        return std::unexpected(Error{ErrorCode::NotFound, "no " + std::string(kManifestFileName) + " (a plain file) in " +
                                                              pathToUtf8(folder)});
    }
    auto manifestText = readBounded(root / pathFromUtf8(std::string(kManifestFileName)), kMaxManifestBytes,
                                    std::string(kManifestFileName));
    if (!manifestText) {
        return std::unexpected(manifestText.error());
    }
    auto manifest = parseManifest(*manifestText, running);
    if (!manifest) {
        return std::unexpected(Error{ErrorCode::ParseError, std::string(kManifestFileName) + ": " +
                                                                manifest.error().summary()});
    }

    ExtensionBundle bundle;
    bundle.folder = root;
    bundle.scope = scope;
    bundle.bytes = manifestText->size();
    ai::Sha256 hash;
    hashPart(hash, kManifestFileName, *manifestText);
    for (const std::string& path : manifest->files()) {
        if (!isPlainPathBelow(root, path, true)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                                         path + " is missing, or is a link or not a plain file inside the extension"});
        }
        auto source = readBounded(root / pathFromUtf8(path), kMaxScriptBytes, path);
        if (!source) {
            return std::unexpected(source.error());
        }
        if (!isCleanUtf8(*source)) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, path + " is not UTF-8 text (or holds NUL bytes)"});
        }
        bundle.bytes += source->size();
        if (bundle.bytes > kMaxExtensionBytes) {
            return std::unexpected(Error{ErrorCode::InvalidArgument,
                                         "the extension's files add up to more than " +
                                             std::to_string(kMaxExtensionBytes / (1024 * 1024)) + " MiB"});
        }
        hashPart(hash, path, *source);
        bundle.scripts.push_back({path, std::move(*source)});
    }
    bundle.manifest = std::move(*manifest);
    bundle.manifestText = std::move(*manifestText);
    bundle.contentHash = hash.finishHex();
    return bundle;
}

std::vector<FoundExtension> findExtensions(const fs::path& root, ExtensionScope scope,
                                           const std::optional<Version>& running) {
    std::vector<FoundExtension> found;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return found;
    }
    std::vector<fs::path> folders;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end && folders.size() < kMaxFoundExtensions;
         it.increment(ec)) {
        if (it->symlink_status(ec).type() == fs::file_type::directory) {
            folders.push_back(it->path());
        }
    }
    std::sort(folders.begin(), folders.end());
    for (const fs::path& folder : folders) {
        if (fs::exists(folder / pathFromUtf8(std::string(kManifestFileName)), ec)) {
            found.push_back({folder, loadExtension(folder, scope, running)});
        }
    }
    return found;
}

fs::path projectExtensionsRoot(const fs::path& project) {
    return project.parent_path() / pathFromUtf8(std::string(kProjectExtensionsDir));
}

} // namespace wabcut::script
