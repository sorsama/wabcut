#pragma once

// The immutable project snapshot: media items, bins and sequences.
//
// A ProjectState is never modified after it is published. Edits (EditOps.h, ProjectOps.h) build a new
// state that shares every unchanged node (media items, sequences, tracks) with the old one through
// shared_ptr<const T>, so a revision costs O(touched tracks) and old revisions stay valid for undo and
// for engine threads that still read them.

#include <wabcut/model/Error.h>
#include <wabcut/model/Graphics.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/MediaInfo.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ReviewNotes.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/SharedList.h>
#include <wabcut/model/Transcripts.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut {

/// A folder in the project panel. Bins form a tree through `parent` (invalid = child of the root bin,
/// which is implicit and has no entry).
struct Bin {
    BinId id;
    BinId parent;
    std::string name;

    bool operator==(const Bin&) const = default;
};

/// The generator of a synthetic media item: an item made by Wabcut rather than read from a file (a title).
/// The renderer draws it; the model only stores what to draw. A title's numeric styling (size, colours,
/// stroke, shadow, alignment, spacing) is the clip's "wabcut.title.style" effect (TitleOps.h), so it can be
/// keyframed; its placement is the clip's intrinsic Transform.
struct GeneratorSpec {
    std::string kind;       ///< what generates it, e.g. "wabcut.title"; unknown kinds are kept as they are
    std::string text;       ///< title text (UTF-8); lines are separated by '\n'
    std::string fontFamily; ///< e.g. "Arial"
    std::string fontStyle;  ///< e.g. "Regular", "Bold Italic"
    /// C.15: a synthetic item's parameters (Synthetic.h: colours, bars pattern, tone, leader options), constants sorted
    /// by key; a missing key has the kind's default. Saved only when not empty.
    std::vector<EffectParam> params;

    bool operator==(const GeneratorSpec&) const = default;
};

/// GeneratorSpec::kind of titles.
inline constexpr std::string_view kTitleGenerator = "wabcut.title";

/// Where an AI-generated media file came from (AI-10, docs/ai.md "Generative media"): the provider and model that
/// made it, a SHA-256 of the prompt (the prompt itself is not retained in either the project or sidecars), when, and how.
/// Its presence marks the item as AI-generated: the timeline tags its clips and exports declare it (C2PA / sidecar).
struct GeneratedProvenance {
    static constexpr std::size_t kMaxNameBytes = 128;     ///< provider, model
    static constexpr std::size_t kMaxOperationBytes = 32; ///< operation

    std::string provider;   ///< registry provider id, e.g. "gemini", "fal"
    std::string model;      ///< the provider's model id
    std::string promptHash; ///< 64 lowercase hex digits: SHA-256 of the UTF-8 prompt
    std::string createdAt;  ///< ISO-8601 UTC, e.g. "2026-10-11T09:30:00Z"
    std::string kind;       ///< "video", "image" or "audio" (AI-12: dub and narration)
    /// "text-to-video", "image-to-video", "extend-head", "extend-tail" or "text-to-image" (others are kept).
    std::string operation;
    /// For an extension: the media it extends. A weak reference: it may dangle once that media is removed.
    MediaId source;

    bool operator==(const GeneratedProvenance&) const = default;
};

/// True for a strict ISO-8601 UTC timestamp "YYYY-MM-DDTHH:MM:SS[.fraction]Z" with valid fields.
bool isIso8601Utc(std::string_view text);

/// A subclip (M5k, Premiere's Clip > Make Subclip, Ctrl/Cmd+U): a library item that plays part of another item's file.
/// It is a full MediaItem with a copy of its master's locator, info and streams (so playback, proxies, relinking and
/// export need nothing new), plus the part of the media it stands for.
struct SubclipSpec {
    /// The item it was made from. Weak: it may dangle once that item is removed (the subclip keeps its own locator).
    MediaId master;
    /// The subclip's In/Out in media time, [start, end): non-empty, start >= 0. A range past the end of a shorter
    /// relinked file is kept as it is (subclipRangeWarnings) and clamped where it is used (sourceBounds).
    TimeRange range;
    /// Premiere's "Restrict Trims To Subclip Boundaries" (on by default): edits may not reach media outside `range`
    /// (sourceBounds() returns it). Off: `range` is only the default In/Out and the whole file is usable.
    bool restricted = true;

    bool operator==(const SubclipSpec&) const = default;
};

/// The part of a stream's media that edits may use: [start, end); `end` unset when unbounded (stills) or unknown.
struct SourceBounds {
    Ticks start;
    std::optional<Ticks> end;

    bool operator==(const SourceBounds&) const = default;
};

/// AudioTake::kind of Enhance Speech's takes.
inline constexpr std::string_view kEnhanceSpeechTake = "enhanceSpeech";

/// An offline render of one of an item's audio streams that clips can play in its place (A.4, docs/ai.md "Enhance
/// Speech"): Enhance Speech's enhanced take. Clips of that stream with an enabled wabcut.audio.enhanceSpeech effect
/// play it, mixed with the source by the effect's Strength. A take whose file is missing, or whose source no longer
/// matches the item's file, is ignored (the source plays) until it is rendered again: it is a cache, losing it loses no
/// edit.
struct AudioTake {
    static constexpr std::size_t kMaxNameBytes = 128;

    int stream = 0;               ///< the audio stream ordinal (StreamRef::index) it renders
    std::string kind;             ///< what made it: kEnhanceSpeechTake
    std::string path;             ///< UTF-8, absolute: a 48 kHz float WAV of the stream from source time 0
    std::string model;            ///< model and processing version, e.g. "deepfilternet3@c94d91f70911/dsp1"
    SourceFingerprint source;     ///< the file it was rendered from
    std::uint64_t generation = 0; ///< bumped by every render, so rendering again is a change players notice

    bool operator==(const AudioTake&) const = default;
};

/// Where a media file made from another one came from (A.4: stem separation): the source (a weak reference: it may
/// dangle once that media is removed), the operation, the model and which part of the result the file holds.
struct DerivedProvenance {
    static constexpr std::size_t kMaxNameBytes = 128;

    MediaId source;
    std::string operation; ///< e.g. "separateStems"
    std::string model;     ///< e.g. "htdemucs@d05c269d0178/sep1"
    std::string part;      ///< e.g. "vocals"
    std::string createdAt; ///< ISO-8601 UTC

    bool operator==(const DerivedProvenance&) const = default;
};

/// Where and when a media file was recorded in Wabcut (A.8: Voice-over Record): the sequence, track and timeline
/// range of the pass (the slot its takes share), the take number, the input device and the latency compensation
/// applied. The sequence and track are weak references (they may dangle once removed: still valid state). The device
/// name is the operating system's, untrusted text: bounded and UTF-8, never interpreted.
struct RecordingProvenance {
    static constexpr std::size_t kMaxDeviceBytes = 256;

    SequenceId sequence;
    TrackId track;
    TimeRange range;             ///< timeline range of the pass (positive duration)
    std::int32_t take = 1;       ///< 1-based, per slot
    std::string device;          ///< input device name (UTF-8, at most kMaxDeviceBytes)
    Ticks latencyCompensation{}; ///< how much earlier the clip was placed than the input was read (round trip)
    std::string createdAt;       ///< ISO-8601 UTC

    bool operator==(const RecordingProvenance&) const = default;
};

/// An imported media file, or a synthetic item (`generator` set: no file, no locator path, nothing to relink).
struct MediaItem {
    MediaId id;
    /// Bin that holds the item; invalid = root bin.
    BinId bin;
    std::string name;
    /// Where the file is (absolute and project-relative), its fingerprint, offline state and proxy.
    /// Authoritative for relinking; `info.path` is only what the probe saw.
    MediaLocator locator;
    MediaInfo info;
    /// Set for synthetic items. Their locator has no path, fingerprint or proxy; `info` describes what the
    /// generator produces (a title: one still video stream at the canvas size, unbounded, no audio).
    std::optional<GeneratorSpec> generator;
    /// Content new clips of this media start with (its name, default parameters). Clips that still have
    /// it share this one block, which keeps copying them cheap. Derived data: set by addMedia and when
    /// loading, never serialized and ignored by ==; may be null for hand-built items.
    std::shared_ptr<const ClipContent> clipDefaults;
    /// Source Monitor In/Out marks (Premiere keeps them with the clip in the project), in media time: In is the
    /// first frame used, Out is exclusive. Unset = the media start / end. Saved only when set; a mark past the
    /// media's end (relinked to a shorter file) loads as it is and is clamped when it is used.
    std::optional<Ticks> markIn;
    std::optional<Ticks> markOut;
    /// Clip markers (M5i), in media time and sorted by start. Every clip of this media shows the ones inside the
    /// source range it uses (Premiere's master-clip markers). Saved only when there are any.
    std::vector<Marker> markers;
    /// Set for AI-generated media (AI-10). Saved only when set.
    std::optional<GeneratedProvenance> generated;
    // ---- Library metadata (M5k, Premiere's Project panel) ----
    /// Bound on comment and description (project files are untrusted input).
    static constexpr std::size_t kMaxTextBytes = 16 * 1024;
    /// Label colour: index into the 16 label colours (0..15, Theme.clipColors); new clips of the item take it. Saved
    /// only when not 0.
    std::int32_t colorIndex = 0;
    /// Premiere's Comment and Description columns (UTF-8, at most kMaxTextBytes). Saved only when not empty.
    std::string comment;
    std::string description;
    /// Poster frame (the library thumbnail), media time, >= 0; unset = the middle. A frame past the end of a shorter
    /// relinked file is clamped where it is used. Saved only when set.
    std::optional<Ticks> posterFrame;
    /// Set for subclips. Saved only when set.
    std::optional<SubclipSpec> subclip;
    /// Offline renders clips can play instead of the source (A.4: Enhance Speech), sorted by (stream, kind), at most
    /// one per pair. Saved only when there are any.
    std::vector<AudioTake> audioTakes;
    /// Set for media made from other media (A.4: separated stems). Saved only when set.
    std::optional<DerivedProvenance> derived;
    /// Set for media recorded in Wabcut (A.8: voice-over takes). Saved only when set.
    std::optional<RecordingProvenance> recording;
    /// A.9: Premiere's Modify > Audio Channels on the item: the channel map its audio clips use unless they have their
    /// own (Clip::channelMap). One map for every audio stream of the item. Saved only when set.
    std::optional<AudioChannelMap> channels;

    bool hasStream(StreamRef stream) const;
    /// Amount of source media available for `stream`, or nullopt when unbounded (still images) or
    /// unknown. Uses the stream's duration, falling back to the container duration.
    std::optional<Ticks> sourceExtent(StreamRef stream) const;
    /// What edits may use of `stream`: [0, sourceExtent()), or a restricted subclip's range clipped to it. Every edit
    /// operation that bounds a clip's source (trim, slip, slide, speed, remap, three-point edits, transitions,
    /// validation) goes through this one helper.
    SourceBounds sourceBounds(StreamRef stream) const;
    bool isSubclip() const { return subclip.has_value(); }
    /// clipDefaults, or a new block for this media's name when it has none.
    std::shared_ptr<const ClipContent> defaultClipContent() const;
    /// A synthetic item: no file to find, relink, fingerprint or proxy.
    bool isGenerator() const { return generator.has_value(); }
    bool isTitle() const { return generator && generator->kind == kTitleGenerator; }
    /// A graphic clip's canvas (Graphics.h, C.13).
    bool isGraphic() const { return generator && generator->kind == kGraphicGenerator; }
    /// The take of `stream` made by `kind`, or null.
    const AudioTake* audioTake(int stream, std::string_view kind) const;

    bool operator==(const MediaItem& o) const {
        return id == o.id && bin == o.bin && name == o.name && locator == o.locator && info == o.info &&
               generator == o.generator && markIn == o.markIn && markOut == o.markOut && markers == o.markers &&
               generated == o.generated && colorIndex == o.colorIndex && comment == o.comment &&
               description == o.description && posterFrame == o.posterFrame && subclip == o.subclip &&
               audioTakes == o.audioTakes && derived == o.derived && recording == o.recording && channels == o.channels;
    }
};

/// A file the project uses besides media. For now that is a colour look-up table (.cube) a grade applies. Effects
/// refer to it through EffectData text ("asset:<id>", assetReference()). It is located like media (absolute and
/// project-relative path, fingerprint, offline flag, never a proxy), so opening, relinking and Collect Files treat it
/// the same way.
struct ProjectAsset {
    AssetId id;
    std::string kind;     ///< kAssetKindLut; unknown kinds are kept as they are
    std::string name;     ///< shown in the UI, e.g. the file's base name
    MediaLocator locator;

    bool operator==(const ProjectAsset&) const = default;
};

/// ProjectAsset::kind of colour look-up tables.
inline constexpr std::string_view kAssetKindLut = "lut";

/// The EffectData text that refers to an asset ("asset:<id>"), and the id such a text refers to (invalid otherwise).
std::string assetReference(AssetId asset);
AssetId assetOfReference(std::string_view text);

using MediaPtr = std::shared_ptr<const MediaItem>;
using SequencePtr = std::shared_ptr<const Sequence>;

struct ProjectState {
    /// Next id to hand out; every id in the project is below `ids.next`.
    IdGenerator ids;
    std::vector<Bin> bins;
    /// Sorted by id.
    std::vector<MediaPtr> media;
    /// Sorted by id.
    std::vector<SequencePtr> sequences;
    /// The project is locked to no-cloud AI (docs/ai.md, "Security"): no provider call or upload may leave this
    /// machine while it is open, whatever consent says. Saved with the project, so it holds after a reopen, in
    /// recovery copies and on other machines.
    bool aiNoCloud = false;
    /// Files besides media (LUTs), sorted by id. Saved only when not empty.
    std::vector<ProjectAsset> assets;
    /// Source-anchored redactions (Masks.h, I.6), sorted by id. Saved only when not empty. Shared between snapshots
    /// until changed (SharedList.h, C.10): a tracked region holds ~420 KB of keys per minute, and twenty of them would
    /// otherwise be copied by every edit and undo step.
    SharedList<Redaction> redactions;
    /// Speakers of the project's transcripts (Transcripts.h, C.7), sorted by id. Saved only when not empty.
    std::vector<Speaker> speakers;
    /// Edits of transcripts per media audio stream (Transcripts.h, C.7), sorted by (media, audioStream), none empty.
    /// Saved only when not empty. Shared between snapshots until changed (SharedList.h): they can be large.
    SharedList<TranscriptEdits> transcriptEdits;
    /// The project's named caption styles (CaptionStyle.h, C.7). Saved only when not empty. Shared until changed.
    SharedList<CaptionPreset> captionPresets;
    /// Review notes on the project's sequences (ReviewNotes.h, C.9), sorted by id. Saved only when not empty. Shared
    /// between snapshots until changed: a thousand notes would otherwise be copied by every edit and undo step.
    SharedList<ReviewNote> reviewNotes;
    /// The project's brand kit (Graphics.h, C.13): fonts, colours and logos graphics and caption styles refer to by
    /// name. Saved only when set.
    std::optional<BrandKit> brandKit;
    /// The project's graphic templates (Graphics.h, C.13), sorted by id. Saved only when not empty. Shared until changed.
    SharedList<GraphicTemplate> templates;

    const MediaItem* findMedia(MediaId mediaId) const;
    const Sequence* findSequence(SequenceId sequenceId) const;
    SequencePtr sequencePtr(SequenceId sequenceId) const;
    const Bin* findBin(BinId binId) const;
    const ProjectAsset* findAsset(AssetId assetId) const;
    const Redaction* findRedaction(RedactionId redactionId) const;
    const Speaker* findSpeaker(SpeakerId speakerId) const;
    const TranscriptEdits* findTranscriptEdits(MediaId media, std::int32_t audioStream) const;
    /// A project caption preset by name (case-insensitive), or null.
    const CaptionPreset* findCaptionPreset(std::string_view name) const;
    const ReviewNote* findReviewNote(ReviewNoteId noteId) const;
    /// A project template by id, or null.
    const GraphicTemplate* findTemplate(std::string_view id) const;

    /// A copy with `sequence` replacing the sequence that has the same id (which must exist).
    ProjectState withSequence(SequencePtr sequence) const;

    /// Deep value equality.
    bool operator==(const ProjectState& other) const;
};

/// True when `a` and `b` are the same revision: equal generators and every bin, media item, sequence
/// and track node is shared (pointer-identical). O(nodes), never compares clip contents. EditSession
/// uses it to skip no-op edits.
bool isSameRevision(const ProjectState& a, const ProjectState& b);

enum class Validation {
    /// Ids, ordering, overlaps, references, links, marks, values. ProjectIO::load uses this level.
    Structure,
    /// Structure plus: every clip's source range lies inside its media's known extent. A relinked,
    /// shorter file can break this without the project being corrupt, so loading does not require it;
    /// edit operations always preserve it.
    Strict,
};

/// Checks one transition against its track: its clips exist on the track and meet at the edit point,
/// its lengths fit inside them, and (Strict) both clips have the source handles it needs. Returns the
/// problem in words, or nullopt when it fits.
std::optional<std::string> transitionProblem(const ProjectState& state, const Track& track, const Transition& t,
                                             Validation level);

/// The Strict check of one clip: its source range lies inside MediaItem::sourceBounds() (the media, or a restricted
/// subclip's range), time remaps and frame holds included. The problem in words ("plays past the end of its source
/// media"), or nullopt; nullopt too for a clip that plays no media (nested, generators of a newer build) or whose media
/// is missing (Structure reports that). An edit that brings a few clips checks those instead of the whole project
/// (paste, E5-P2-2): a clip elsewhere of a file relinked to a shorter one does not block it.
std::optional<std::string> clipBoundsProblem(const ProjectState& state, const Clip& clip);

/// The edit point of a transition (A's end or, for a one-sided transition at B's head, B's start).
std::optional<Ticks> transitionEditPoint(const Track& track, const Transition& t);

/// Effect data that refers to an asset the project does not have ("asset:<id>" with no such id): a hand-edited or
/// merged project. Not invalid state - the grade bypasses that LUT and the UI shows it missing - so validate() does
/// not report it; this lists them ("clip 12, effect 3: creative.look -> asset:40") for warnings.
std::vector<std::string> assetReferenceWarnings(const ProjectState& state);

/// Subclips whose range lies (partly) past the end of their file (relinked to a shorter one): not invalid state - the
/// range is clamped where it is used (MediaItem::sourceBounds) - so validate() does not report it; this lists them
/// ("subclip 'Take 2' runs 00:00:03:12 past the end of its media") for warnings.
std::vector<std::string> subclipRangeWarnings(const ProjectState& state);

/// Checks every model invariant. Returns an error that lists the violations found. A link group needs at
/// least two clips, except that a single remaining clip on a locked track is allowed (edits never touch
/// locked tracks, so they leave such a clip in its group until the track is unlocked).
/// EditSession runs it after every edit in debug builds; tests run it after every operation.
std::expected<void, Error> validate(const ProjectState& state, Validation level = Validation::Strict);

} // namespace wabcut
