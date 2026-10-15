#pragma once

// What the user decided about extensions (docs/extensions.md, "Permissions"), kept in one JSON file in the user's
// settings folder.
//
// The flow behind the app's dialogs:
//   1. review(bundle) lists the capabilities the extension asks for, each with what it allows, which of them are
//      already granted, and for a per-project extension whether the user trusts it for this project;
//   2. grant(bundle, chosen) records the chosen capabilities (never more than the manifest asks for) for exactly
//      the files that were reviewed - their content hash, author and folder - and enables the extension;
//      revoke(id) takes every grant back and disables it;
//   3. effective(bundle) is what a running extension gets: granted AND requested AND grantable, and nothing at all
//      once its files, author or folder are not the ones reviewed (S5-P2-4): a new version, a reinstall over it or
//      a copy elsewhere claiming its id runs only after the user reviews it (needsReview).
// Per-project extensions also need trust(project, bundle), which is bound to the extension's content hash: once a
// file changes (a teammate updated the project), the trust lapses and the user is asked again.
// Crashes are counted per extension: after kMaxCrashes in a row the extension is disabled with a reason, so a
// broken extension cannot crash its host over and over.

#include <wabcut/model/Error.h>
#include <wabcut/script/Extension.h>

#include <expected>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::script {

inline constexpr int kMaxCrashes = 3;

/// What a capability lets an extension do, for the permission dialog.
struct CapabilityInfo {
    command::Capability capability;
    std::string title;       ///< "Edit your sequences"
    std::string explanation; ///< one sentence
};
CapabilityInfo describe(command::Capability capability);

struct ExtensionGrant {
    bool enabled = false;
    command::CapabilitySet granted;
    std::string reviewedVersion; ///< the version the user last reviewed
    /// What the user reviewed (S5-P2-4): the grants hold for these exact files in this folder only.
    std::string reviewedHash;   ///< ExtensionBundle::contentHash
    std::string reviewedAuthor; ///< Manifest::author, to point out a change of hands
    std::string reviewedFolder; ///< the extension's folder (canonical)
    int crashes = 0;             ///< in a row; reset by a clean run or by enabling again
    std::string disabledReason;  ///< why it is off when the user did not switch it off ("crashed 3 times")
};

struct CapabilityRequest {
    CapabilityInfo info;
    bool granted = false; ///< already granted (an update)
};

/// Everything the install / enable dialog shows.
struct ExtensionReview {
    Manifest manifest;
    ExtensionScope scope = ExtensionScope::User;
    std::filesystem::path folder;
    std::string contentHash;
    std::vector<CapabilityRequest> requests;
    bool update = false;        ///< an earlier version was reviewed
    bool authorChanged = false; ///< ... by another author (nothing is pre-ticked; its storage starts empty)
    bool needsTrust = false;    ///< a per-project extension the user does not trust for this project (yet)
    std::vector<std::string> notes; ///< things to point out ("comes with the project", manifest warnings)
};

class PermissionStore {
public:
    PermissionStore() = default;
    /// Reads `file`; a missing file is an empty store, an unreadable one too (it is renamed *.bad, never reused).
    static PermissionStore load(const std::filesystem::path& file);
    /// Writes the store (atomically: a temporary file renamed over the old one).
    std::expected<void, Error> save() const;
    const std::filesystem::path& file() const { return m_file; }

    const ExtensionGrant* find(std::string_view id) const;

    /// `project`: the project file, for a per-project extension's trust.
    ExtensionReview review(const ExtensionBundle& bundle, const std::filesystem::path& project = {}) const;
    /// Grants `chosen` (it must lie within what the manifest asks for and what extensions may have) to exactly this
    /// bundle - its files, author and folder - and enables the extension. InvalidArgument otherwise.
    std::expected<void, Error> grant(const ExtensionBundle& bundle, command::CapabilitySet chosen);
    /// Takes every grant back and disables the extension (its storage stays until it is uninstalled).
    void revoke(std::string_view id);
    void setEnabled(std::string_view id, bool enabled);
    /// Forgets the extension altogether (uninstall).
    void forget(std::string_view id);

    bool isEnabled(std::string_view id) const;
    /// The bundle is what the user last reviewed: the same files (content hash) in the same folder.
    bool isReviewed(const ExtensionBundle& bundle) const;
    /// Granted, requested and grantable; nothing unless isReviewed(bundle).
    command::CapabilitySet effective(const ExtensionBundle& bundle) const;
    /// Never reviewed, not the files reviewed (isReviewed), or the manifest asks for a capability that was not granted.
    bool needsReview(const ExtensionBundle& bundle) const;

    void trust(const std::filesystem::path& project, const ExtensionBundle& bundle);
    void distrust(const std::filesystem::path& project, std::string_view id);
    bool isTrusted(const std::filesystem::path& project, const ExtensionBundle& bundle) const;

    /// Counts a crash; disables the extension (with a reason) at kMaxCrashes. Returns the count.
    int recordCrash(std::string_view id, std::string_view why);
    /// A run that ended cleanly: the crash count starts again.
    void recordCleanRun(std::string_view id);

    Json toJson() const;
    static std::expected<PermissionStore, std::string> fromJson(const Json& json);

private:
    /// "<project folder>|<extension id>": trust is per project folder, so a copied project starts untrusted.
    static std::string trustKey(const std::filesystem::path& project, std::string_view id);

    std::filesystem::path m_file;
    std::map<std::string, ExtensionGrant, std::less<>> m_grants;
    std::map<std::string, std::string, std::less<>> m_trust; ///< trustKey -> content hash
};

/// "Read, EditSequence" (the names extensions and the CLI use).
std::string capabilityList(command::CapabilitySet set);
/// Parses "read,editSequence" or "Read, EditSequence" (case-insensitive); nullopt on an unknown name.
std::optional<command::CapabilitySet> parseCapabilityList(std::string_view text);

} // namespace wabcut::script
