#pragma once

// Saving a project for an older Wabcut (0.7, card 059): File > Save for an Older Version.
//
// An older build refuses a project whose schemaVersion is newer than its own, and opens one at or below it while
// ignoring (and, on its next save, silently losing) whatever it does not know. Saving for an older release therefore
// writes the project the way that release reads it: the release's schema number, no key or value the release does not
// have, every other setting exactly as it is. What cannot be held is left out and listed, so the person who hands the file
// over knows what the other editor will not see.
//
// The releases offered are the ones whose files this build knows how to write (each rule below says which release
// introduced a field):
//   * Wabcut 0.6   - schema 19; 0.7 added optional fields only, each written only when set.
//   * Wabcut 0.2 to 0.5 - schema 18 (0.2 through 0.5 shared one schema); also without the 0.6 additions.
// Wabcut 0.1 (schema 16) is not offered: 0.2 changed too much of the file (sequence settings, colour management, graphics,
// submixes...) to leave out by field, and 0.1 is three weeks old at the time of writing - open the project in 0.2 to 0.5.
// The source of truth for "which keys did a release know" is tests/data/project-file-keys-*.txt (the key lists of 0.2 and
// 0.6); the test "Every key a project file uses has a place in the older-version rules" fails when a field is added to the
// project file without a rule here, so the table cannot fall behind.

#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::io {

/// A release (or a run of releases that share a project schema) a project can be saved for.
struct OlderRelease {
    std::string_view id;       ///< "0.6", "0.5": the newest minor version of the run, what the commands and the menu pass
    std::string_view name;     ///< "Wabcut 0.6", "Wabcut 0.2 to 0.5"
    std::string_view versions; ///< "0.6.x", "0.2.x to 0.5.x"
    int schema = 0;            ///< the project schema those builds read and write
};

/// The releases offered, newest first. All are older than this build.
std::span<const OlderRelease> olderReleases();

/// The entry for a version text: "0.6", "0.6.2", "Wabcut 0.4" and "v0.3.1" all find their run. Null for a version this
/// build cannot write (0.1, an unknown one, this build's own or a newer one).
const OlderRelease* findOlderRelease(std::string_view version);

/// One thing the older release cannot hold.
struct DowngradeLoss {
    std::string id;      ///< stable key of the feature ("captionLooks")
    std::string what;    ///< "Kinetic caption looks"
    std::string effect;  ///< what the other editor sees instead ("shown without the animation")
    int count = 0;       ///< how many places lost it (captions styles, clips, tracks ...)
};

struct DowngradeResult {
    std::string json; ///< the project text for the release (pretty-printed)
    std::string release; ///< OlderRelease::id
    int schema = 0;
    std::vector<DowngradeLoss> losses; ///< in the order of the rules; empty = nothing was left out
};

/// The project as release `release` reads it. InvalidArgument for an unknown or not older release.
std::expected<DowngradeResult, Error> toJsonForRelease(const ProjectState& state, std::string_view release);

/// The same for a project already serialized by toJson (any current-schema text).
std::expected<DowngradeResult, Error> downgradeJson(std::string_view currentJson, std::string_view release);

/// Writes the project for `release` to `file` atomically, after checking that the text loads back. No ".bakN" rotation: a
/// copy for someone else is a one-off file. Returns what was left out.
std::expected<DowngradeResult, Error> saveProjectForRelease(const ProjectState& state, const std::filesystem::path& file,
                                                            std::string_view release);

/// The features the rules know, for tests and docs: (id, the release run it needs, a key it removes).
struct DowngradeRuleInfo {
    std::string_view id;
    std::string_view key;
    int since = 0; ///< 2 = Wabcut 0.6 introduced it, 3 = Wabcut 0.7
};
std::vector<DowngradeRuleInfo> downgradeRules();

} // namespace wabcut::io
