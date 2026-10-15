#pragma once

// Strong identifier types for project objects.
//
// Every id is a uint64 handed out by the project's IdGenerator (see ProjectState). The generator is a
// single monotonic counter shared by all id kinds, so an id value is unique across the whole project and
// is never reused, even after the object it named was deleted. Value 0 means "no id".
//
// Undo does not rewind ids: EditSession keeps the highest id ever handed out outside the undo history
// and raises every edit's starting generator to it, so ids from an undone (or abandoned redo) branch
// are never given to new objects.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace wabcut {

/// A typed uint64 id. `Tag` only distinguishes the kinds at compile time.
template <class Tag>
struct Id {
    std::uint64_t value = 0;

    constexpr bool isValid() const noexcept { return value != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    constexpr auto operator<=>(const Id&) const = default;
};

struct MediaIdTag;
struct SequenceIdTag;
struct TrackIdTag;
struct ClipIdTag;
struct LinkGroupIdTag;
struct BinIdTag;
struct TransitionIdTag;
struct EffectInstanceIdTag;
struct CaptionTrackIdTag;
struct AssetIdTag;
struct RedactionIdTag;
struct SpeakerIdTag;
struct ReviewNoteIdTag;

using MediaId = Id<MediaIdTag>;
using SequenceId = Id<SequenceIdTag>;
using TrackId = Id<TrackIdTag>;
using ClipId = Id<ClipIdTag>;
/// Clips sharing a valid LinkGroupId are linked (Premiere's A/V link): they select and edit together.
using LinkGroupId = Id<LinkGroupIdTag>;
using BinId = Id<BinIdTag>;
using TransitionId = Id<TransitionIdTag>;
/// An effect instance in an effect stack. Unique within its stack; both halves of a razor cut share their
/// stacks (and so the ids) until one of them is edited.
using EffectInstanceId = Id<EffectInstanceIdTag>;
using CaptionTrackId = Id<CaptionTrackIdTag>;
/// A file the project uses besides media (ProjectAsset): a colour LUT, for now.
using AssetId = Id<AssetIdTag>;
/// A source-anchored redaction (Masks.h, I.6).
using RedactionId = Id<RedactionIdTag>;
/// A speaker of the project's transcripts (Transcripts.h, C.7): a display name and colour.
using SpeakerId = Id<SpeakerIdTag>;
/// A review note (ReviewNotes.h, C.9).
using ReviewNoteId = Id<ReviewNoteIdTag>;

/// Hands out project-unique ids. It is plain data inside the immutable ProjectState: an edit copies it,
/// allocates from the copy and stores the advanced generator in the new state, so edits stay pure.
struct IdGenerator {
    std::uint64_t next = 1;

    template <class IdT>
    constexpr IdT allocate() noexcept {
        return IdT{next++};
    }
    constexpr bool operator==(const IdGenerator&) const = default;
};

} // namespace wabcut

template <class Tag>
struct std::hash<wabcut::Id<Tag>> {
    std::size_t operator()(const wabcut::Id<Tag>& id) const noexcept { return std::hash<std::uint64_t>{}(id.value); }
};
