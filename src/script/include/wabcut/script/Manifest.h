#pragma once

// An extension's manifest, wabcut-extension.json (docs/extensions.md): who made it, which capabilities it asks for and
// where its code starts. Extensions are untrusted input (they come from anywhere, and per-project ones arrive inside
// shared projects), so parseManifest() bounds and checks every field before anything else reads it.
//
//   {"manifestVersion": 1, "id": "com.example.beat-markers", "name": "Beat Markers", "version": "1.2.0",
//    "author": "Example", "licence": "MIT", "description": "...", "minWabcutVersion": "0.9.0",
//    "capabilities": ["Read", "EditSequence"], "main": "main.js", "scripts": ["lib/beats.js"],
//    "commands": [{"id": "addBeatMarkers", "title": "Add Markers at Every Beat"}]}

#include <wabcut/command/Command.h>

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::script {

using Json = command::Json;

inline constexpr std::string_view kManifestFileName = "wabcut-extension.json";
inline constexpr int kManifestVersion = 1;
/// Bounds of a manifest; far above any real one.
inline constexpr std::size_t kMaxManifestBytes = 64 * 1024;
inline constexpr std::size_t kMaxManifestNesting = 8;
inline constexpr std::size_t kMaxExtensionIdBytes = 100;
inline constexpr std::size_t kMaxNameBytes = 64;
inline constexpr std::size_t kMaxDescriptionBytes = 2000;
inline constexpr std::size_t kMaxScriptPathBytes = 200;
inline constexpr std::size_t kMaxScripts = 32; ///< "scripts" plus "main"
inline constexpr std::size_t kMaxDeclaredCommands = 64;

/// A semantic version, MAJOR.MINOR.PATCH with an optional pre-release ("1.0.0-beta.2"); build metadata is not
/// accepted. Numbers stay below 1,000,000.
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease;

    friend bool operator==(const Version&, const Version&) = default;
};

std::optional<Version> parseVersion(std::string_view text);
std::string toString(const Version& version);
/// <0, 0 or >0 as semver orders them (a pre-release sorts before its release; pre-releases compare as text).
int compareVersions(const Version& a, const Version& b);

/// A command the extension adds to the command palette (and the shortcut editor), known before it starts.
struct DeclaredCommand {
    std::string id;    ///< [A-Za-z][A-Za-z0-9._-]{0,63}, unique in the extension
    std::string title; ///< palette text
};

struct Manifest {
    std::string id;   ///< reverse-DNS, lower case: "com.example.beat-markers"
    std::string name; ///< shown in the extension manager and the undo history
    Version version;
    std::string author;
    std::string licence; ///< SPDX identifier or free text ("license" is read too)
    std::string description;
    Version minWabcutVersion;
    /// What the extension asks for; a subset of grantableCapabilities(), in the order given, without duplicates.
    std::vector<command::Capability> capabilities;
    std::string main;                 ///< entry script, a relative path ("main.js")
    std::vector<std::string> scripts; ///< scripts evaluated before `main`, in order (shared helpers)
    std::vector<DeclaredCommand> commands;
    /// Keys the parser does not know (ignored): a newer manifest still loads, and the manager can show them.
    std::vector<std::string> warnings;

    command::CapabilitySet requested() const;
    /// `scripts` then `main`: every file the extension consists of besides the manifest.
    std::vector<std::string> files() const;
};

struct ManifestIssue {
    std::string field; ///< "id", "capabilities[2]", "" for the document
    std::string message;
};

struct ManifestError {
    std::vector<ManifestIssue> issues;
    /// "field: message; field: message"
    std::string summary() const;
};

/// Parses and checks a manifest. With `running` (this Wabcut's version), an extension that needs a newer one is
/// refused too.
std::expected<Manifest, ManifestError> parseManifest(std::string_view text, const std::optional<Version>& running = {});

/// The manifest as JSON in the file's format (normalised: "licence", versions as text).
Json toJson(const Manifest& manifest);

/// Capabilities an extension may be granted: Read, EditSequence, EditProject and MediaAnalysis. Network never:
/// extensions have no network access, and cloud AI is reachable only through commands (with the host's consent,
/// spend cap and no-cloud guards).
command::CapabilitySet grantableCapabilities();

bool isValidExtensionId(std::string_view id);
/// Relative, '/'-separated, segments of [A-Za-z0-9_.-] that are not "." or "..", ending in ".js".
bool isValidScriptPath(std::string_view path);
bool isValidCommandId(std::string_view id);
/// Text a person reads (the permission dialog, notifications, prompts): no control characters (new lines only where
/// `multiline`), and no character that draws nothing or rearranges the text around it (S5-P2-2): format characters
/// (bidi marks, overrides and isolates, joiners, soft hyphen, tags), line and paragraph separators, variation
/// selectors, private-use characters, Hangul fillers and noncharacters. `text` must be valid UTF-8.
bool isDisplayText(std::string_view text, bool multiline);

/// The reserved word `name` starts with or imitates ("Wabcut", "Adobe", "Premiere", ...: case, fullwidth forms,
/// accents, look-alike Cyrillic and Greek letters, digits and spacing do not matter), or nullopt. parseManifest()
/// refuses such names, so an extension cannot title its prompts and notifications as the editor (S5-P2-2).
std::optional<std::string> reservedName(std::string_view name);

} // namespace wabcut::script
