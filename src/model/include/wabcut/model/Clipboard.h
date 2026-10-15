#pragma once

// The clipboard (C.11): copy, cut and paste of timeline clips, and Premiere's Paste Attributes / Remove Attributes.
// Model-level, so the app, the CLI, MCP clients and the AI agent share one behaviour.
//
// A ClipboardDocument is a small project of its own: one sequence holding the copied clips exactly as they were - their
// speed (constant, reversed, freeze), time remap, effects and keyframes, names, labels, enabled state, links - at their
// times relative to the first copied clip and on tracks numbered from the lowest copied track of each kind, with the
// transitions between copied clips, the sequence markers inside the copied range and (optionally) the caption cues
// there, plus the media items (and nested sequences) the clips play. Its JSON (clipboardToJson) wraps that project's
// .wabcut JSON, so it is versioned, bounded and validated like a project file: a document pasted from another Wabcut is
// untrusted input.
//
// Paste follows Premiere. Overwrite (Ctrl+V) replaces what lies under each pasted clip; insert (Ctrl+Shift+V) first
// opens the clipboard's length at the playhead on the destination tracks and every unlocked sync-locked track (and the
// captions). Clips land on the lowest targeted track of their kind plus their copied offset (the copied tracks when no
// track of that kind is targeted, or the tracks given); missing tracks are added. Media is found in the destination
// project by id (same file), else by fingerprint, else by path; else the copied media item is added to the project.
// The report says which.

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

inline constexpr int kClipboardVersion = 1;
/// Bounds of a clipboard document read back (it may come from anywhere).
inline constexpr std::size_t kMaxClipboardBytes = std::size_t{64} * 1024 * 1024;
inline constexpr std::size_t kMaxClipboardClips = 10'000;

struct ClipboardDocument {
    /// The copied material: media, nested sequences and one sequence (`clips`) with the clips from time 0.
    ProjectState content;
    SequenceId clips;
    /// Index of the lowest copied video / audio track in the sequence it came from (pasting with nothing targeted
    /// puts clips back on the same tracks); -1 when none of that kind was copied.
    int videoBase = -1;
    int audioBase = -1;
    /// From the first copied clip's start to the last one's end.
    Ticks duration;
    bool hasCaptions = false;
    /// Copied from a project locked to no-cloud AI (ProjectState::aiNoCloud): kept from cloud-bound readers (the
    /// app's assistant, ClipboardServices::forCloudCaller) outside a no-cloud project, and a project it is pasted into
    /// becomes no-cloud (review wave 5, S5-P2-12).
    bool noCloud = false;

    const Sequence& sequence() const { return *content.findSequence(clips); }
    std::size_t clipCount() const;
};

struct CopyOptions {
    /// The clips' link partners are copied with them (Premiere copies what is selected, and selecting a linked clip
    /// selects its partners).
    edit::LinkMode links = edit::LinkMode::FollowLinks;
    bool markers = true;  ///< sequence markers starting inside the copied range
    bool captions = false; ///< caption cues inside the copied range
};

std::expected<ClipboardDocument, Error> copyClips(const ProjectState& state, SequenceId sequence,
                                                  std::span<const ClipId> clips, const CopyOptions& options = {});

/// The clipboard as text (format "wabcut-clipboard"); fails only on text that is not UTF-8.
std::expected<std::string, Error> clipboardToJson(const ClipboardDocument& document);
/// Reads a clipboard back: bounded (kMaxClipboardBytes, kMaxClipboardClips, JSON depth), versioned, and validated
/// like a project file.
std::expected<ClipboardDocument, Error> clipboardFromJson(std::string_view text);

enum class PasteMode : std::uint8_t { Overwrite, Insert };

struct PasteOptions {
    PasteMode mode = PasteMode::Overwrite;
    /// Destination of the lowest copied track of each kind; unset: the lowest targeted unlocked track, else the
    /// copied track index.
    std::optional<TrackId> videoTrack;
    std::optional<TrackId> audioTrack;
    bool markers = true;
    bool captions = true;
    /// Add the tracks the clipboard needs beyond the sequence's last one (else refuse).
    bool addTracks = true;
};

struct PasteReport {
    std::vector<ClipId> pasted;           ///< the new clips, in clipboard order
    std::vector<MediaId> mediaAdded;      ///< media items added to the project for the paste
    std::vector<TrackId> tracksAdded;
    std::vector<std::string> notes;       ///< how media was found ("'A.mov': matched by fingerprint"), what was skipped
};

struct PasteResult {
    edit::EditResult edit;
    PasteReport report;
};

/// Pastes at `at` (one undo step; the playhead moves to the end of the pasted material).
std::expected<PasteResult, Error> pasteClips(const ProjectState& state, SequenceId sequence,
                                             const ClipboardDocument& document, Ticks at,
                                             const PasteOptions& options = {});

// ---- Paste Attributes / Remove Attributes (Premiere's Ctrl+Alt+V) -----------------------------------------------------

/// Which attributes. Video clips take motion, opacity and video effects; audio clips volume, panner and audio
/// effects; time remapping applies to either. (Premiere's Channel Volume and Panner are the panner here.)
struct AttributeSet {
    bool motion = false;   ///< Transform: position, scale, rotation, anchor
    bool opacity = false;
    bool timeRemap = false;
    bool volume = false;
    bool panner = false;
    /// Effects of the source clip's stack by index (paste), or effect ids such as "wabcut.blur.gaussian" (remove).
    std::vector<std::size_t> effects;
    std::vector<std::string> effectIds;
    bool allEffects = false;

    bool empty() const {
        return !motion && !opacity && !timeRemap && !volume && !panner && effects.empty() && effectIds.empty() &&
               !allEffects;
    }
};

struct AttributeChange {
    ClipId clip;
    std::vector<std::string> applied; ///< "Motion", "Opacity", "Time Remapping", "Gaussian Blur", ...
    std::vector<std::string> skipped; ///< with the reason ("Volume: not an audio clip")
};

struct AttributesResult {
    edit::EditResult edit;
    std::vector<AttributeChange> changes;
};

/// Pastes attributes of `source` (a clip of the clipboard, or of any sequence) onto `targets`: parameter values and
/// keyframes replace the target's, effects are added after the target's own, a time remap replaces the target's
/// speed. Keyframes keep their time from the clip's start, or with `scaleTimes` are stretched to the target's length.
/// One undo step; refuses when nothing would change.
std::expected<AttributesResult, Error> pasteAttributes(const ProjectState& state, SequenceId sequence, const Clip& source,
                                                       std::span<const ClipId> targets, const AttributeSet& attributes,
                                                       bool scaleTimes = false);

/// Removes attributes: parameters back to their defaults (keyframes go), time remapping off (the clip plays at 100 %
/// from where it showed its first frame), effects by id (or all). One undo step; refuses when nothing would change.
std::expected<AttributesResult, Error> removeAttributes(const ProjectState& state, SequenceId sequence,
                                                        std::span<const ClipId> targets, const AttributeSet& attributes);

} // namespace wabcut
