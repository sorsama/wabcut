#pragma once

// An extension on disk: a folder with wabcut-extension.json and the JavaScript files it names (docs/extensions.md).
//
// loadExtension() is the only code that reads an extension's files. It checks the manifest, refuses links and
// reparse points (a file must not lead outside its folder) and bounds every file, then hands the sources to the
// script host as text: the host process itself never opens a file. The content hash covers the manifest and every
// script, so trusting a per-project extension (Permissions.h) holds for exactly the code that was reviewed.

#include <wabcut/model/Error.h>
#include <wabcut/script/Manifest.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::script {

inline constexpr std::size_t kMaxScriptBytes = 1024 * 1024;          ///< one .js file
inline constexpr std::size_t kMaxExtensionBytes = 4 * 1024 * 1024;   ///< every file of an extension
/// The per-project extensions of a project: <project folder>/.wabcut/extensions/<extension>/.
inline constexpr std::string_view kProjectExtensionsDir = ".wabcut/extensions";

/// Where an extension is installed. User extensions run once the user grants them; project extensions come with a
/// project (untrusted input) and also need the user's trust for that project and that exact content.
enum class ExtensionScope { User, Project };
std::string_view toString(ExtensionScope scope);

struct ScriptSource {
    std::string path;   ///< as the manifest names it ("lib/beats.js")
    std::string source; ///< UTF-8 JavaScript
};

struct ExtensionBundle {
    Manifest manifest;
    std::string manifestText; ///< the manifest file as read (installing copies it)
    std::filesystem::path folder;
    ExtensionScope scope = ExtensionScope::User;
    /// In evaluation order: manifest.scripts, then manifest.main.
    std::vector<ScriptSource> scripts;
    /// SHA-256 (hex) of the manifest and every script, with their paths.
    std::string contentHash;
    std::size_t bytes = 0;
};

/// Reads and checks the extension in `folder`. `running` is this Wabcut's version (Manifest.h). Errors:
/// NotFound (no folder or manifest), ParseError (the manifest's issues), InvalidArgument (a link, a file outside the
/// folder, a file too large, not UTF-8 or with NUL bytes), IoError.
std::expected<ExtensionBundle, Error> loadExtension(const std::filesystem::path& folder, ExtensionScope scope,
                                                    const std::optional<Version>& running = {});

/// The extensions in `root` (each subfolder holding a manifest), loaded; folders that fail to load are reported
/// with their error instead.
struct FoundExtension {
    std::filesystem::path folder;
    std::expected<ExtensionBundle, Error> bundle;
};
std::vector<FoundExtension> findExtensions(const std::filesystem::path& root, ExtensionScope scope,
                                           const std::optional<Version>& running = {});

/// <project folder>/.wabcut/extensions for the project file `project`.
std::filesystem::path projectExtensionsRoot(const std::filesystem::path& project);

} // namespace wabcut::script
