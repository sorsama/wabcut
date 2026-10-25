#include <wabcut/model/ProjectState.h>

#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/CaptionText.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/SubmixOps.h>
#include <wabcut/model/Synthetic.h>
#include <wabcut/model/TitleOps.h>

#include <algorithm>
#include <cctype>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace wabcut {

// --- Generated media ----------------------------------------------------------------------------------

bool isIso8601Utc(std::string_view text) {
    // YYYY-MM-DDTHH:MM:SS, an optional fraction, then Z.
    if (text.size() < 20 || text.size() > 40 || text.back() != 'Z') {
        return false;
    }
    auto digits = [&](std::size_t at, std::size_t count, int& out) {
        out = 0;
        for (std::size_t i = at; i < at + count; ++i) {
            if (text[i] < '0' || text[i] > '9') {
                return false;
            }
            out = out * 10 + (text[i] - '0');
        }
        return true;
    };
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!digits(0, 4, year) || text[4] != '-' || !digits(5, 2, month) || text[7] != '-' || !digits(8, 2, day) ||
        text[10] != 'T' || !digits(11, 2, hour) || text[13] != ':' || !digits(14, 2, minute) || text[16] != ':' ||
        !digits(17, 2, second)) {
        return false;
    }
    const std::string_view rest = text.substr(19, text.size() - 20); // between the seconds and the Z
    if (!rest.empty()) {
        if (rest.size() < 2 || rest[0] != '.') {
            return false;
        }
        for (const char c : rest.substr(1)) {
            if (c < '0' || c > '9') {
                return false;
            }
        }
    }
    static constexpr int kDays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12 || day < 1 || day > kDays[month - 1] || hour > 23 || minute > 59 || second > 60) {
        return false;
    }
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return !(month == 2 && day == 29 && !leap);
}

// --- Sequence -----------------------------------------------------------------------------------------

const Track* Sequence::findTrack(TrackId trackId) const {
    for (const auto* list : {&videoTracks, &audioTracks}) {
        for (const auto& t : *list) {
            if (t->id == trackId) {
                return t.get();
            }
        }
    }
    return nullptr;
}

std::optional<Sequence::TrackPosition> Sequence::trackPosition(TrackId trackId) const {
    for (std::size_t i = 0; i < videoTracks.size(); ++i) {
        if (videoTracks[i]->id == trackId) {
            return TrackPosition{TrackKind::Video, i};
        }
    }
    for (std::size_t i = 0; i < audioTracks.size(); ++i) {
        if (audioTracks[i]->id == trackId) {
            return TrackPosition{TrackKind::Audio, i};
        }
    }
    return std::nullopt;
}

const Track* Sequence::findSubmix(TrackId trackId) const {
    for (const auto& t : submixTracks) {
        if (t->id == trackId) {
            return t.get();
        }
    }
    return nullptr;
}

const Track* Sequence::findAudioBus(TrackId trackId) const {
    for (const auto* list : {&audioTracks, &submixTracks}) {
        for (const auto& t : *list) {
            if (t->id == trackId) {
                return t.get();
            }
        }
    }
    return nullptr;
}

std::optional<std::size_t> Sequence::submixIndex(TrackId trackId) const {
    for (std::size_t i = 0; i < submixTracks.size(); ++i) {
        if (submixTracks[i]->id == trackId) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<Sequence::ClipLocation> Sequence::findClip(ClipId clipId) const {
    for (const auto* list : {&videoTracks, &audioTracks}) {
        for (const auto& t : *list) {
            if (const auto index = t->indexOf(clipId)) {
                return ClipLocation{t.get(), *index};
            }
        }
    }
    return std::nullopt;
}

Ticks Sequence::contentEnd() const {
    Ticks end{};
    forEachTrack([&](const Track& t) { end = std::max(end, t.end()); });
    return end;
}

std::optional<TimeRange> Sequence::markedRange() const {
    if (markIn && markOut && *markIn < *markOut) {
        return TimeRange::fromStartEnd(*markIn, *markOut);
    }
    return std::nullopt;
}

namespace {

bool sameTracks(const std::vector<TrackPtr>& a, const std::vector<TrackPtr>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i] && !(a[i] && b[i] && *a[i] == *b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

bool Sequence::operator==(const Sequence& o) const {
    return id == o.id && bin == o.bin && name == o.name && frameRate == o.frameRate && width == o.width &&
           height == o.height && audioRate == o.audioRate && dropFrame == o.dropFrame &&
           startTimecode == o.startTimecode && pixelAspect == o.pixelAspect && fields == o.fields &&
           previewFormat == o.previewFormat && videoDisplay == o.videoDisplay && audioDisplay == o.audioDisplay &&
           colorIndex == o.colorIndex && rippleMarkers == o.rippleMarkers && markIn == o.markIn &&
           markOut == o.markOut && markers == o.markers && duration == o.duration && workArea == o.workArea &&
           effects == o.effects && audioEffects == o.audioEffects && captionTracks == o.captionTracks &&
           beatGrid == o.beatGrid && derivedFrom == o.derivedFrom && masterVolume == o.masterVolume &&
           masterFormat == o.masterFormat && multicam == o.multicam && importSource == o.importSource &&
           sameTracks(videoTracks, o.videoTracks) &&
           sameTracks(audioTracks, o.audioTracks) && sameTracks(submixTracks, o.submixTracks);
}

// --- MediaItem ----------------------------------------------------------------------------------------

const AudioTake* MediaItem::audioTake(int stream, std::string_view kind) const {
    for (const AudioTake& t : audioTakes) {
        if (t.stream == stream && t.kind == kind) {
            return &t;
        }
    }
    return nullptr;
}

bool MediaItem::hasStream(StreamRef stream) const {
    if (stream.index < 0) {
        return false;
    }
    const auto index = static_cast<std::size_t>(stream.index);
    return stream.kind == StreamKind::Video ? index < info.video.size() : index < info.audio.size();
}

std::optional<Ticks> MediaItem::sourceExtent(StreamRef stream) const {
    if (info.isStillImage || !hasStream(stream)) {
        return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(stream.index);
    Ticks extent = stream.kind == StreamKind::Video ? info.video[index].duration : info.audio[index].duration;
    if (extent.value <= 0) {
        extent = info.duration;
    }
    if (extent.value <= 0) {
        return std::nullopt;
    }
    return extent;
}

SourceBounds MediaItem::sourceBounds(StreamRef stream) const {
    SourceBounds bounds{Ticks{}, sourceExtent(stream)};
    if (subclip && subclip->restricted) {
        Ticks start = subclip->range.start;
        Ticks end = subclip->range.end();
        if (bounds.end) {
            // A range past the end of a shorter relinked file: what is left of it (subclipRangeWarnings says so).
            end = std::min(end, *bounds.end);
            start = std::min(start, end);
        }
        bounds = {start, end};
    }
    return bounds;
}

std::shared_ptr<const ClipContent> MediaItem::defaultClipContent() const {
    if (clipDefaults && clipDefaults->name == name && clipDefaults->properties == ClipProperties{}) {
        return clipDefaults;
    }
    return std::make_shared<const ClipContent>(ClipContent{name, {}});
}

// --- ProjectState -------------------------------------------------------------------------------------

namespace {

template <class Ptr, class IdT>
auto findById(const std::vector<Ptr>& items, IdT id) -> decltype(items.begin()) {
    const auto it = std::lower_bound(items.begin(), items.end(), id,
                                     [](const Ptr& item, IdT value) { return item->id < value; });
    if (it != items.end() && (*it)->id == id) {
        return it;
    }
    return items.end();
}

template <class Ptr>
bool sameNodes(const std::vector<Ptr>& a, const std::vector<Ptr>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

template <class Ptr>
bool equalValues(const std::vector<Ptr>& a, const std::vector<Ptr>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const Ptr& x, const Ptr& y) { return x == y || (x && y && *x == *y); });
}

} // namespace

const MediaItem* ProjectState::findMedia(MediaId mediaId) const {
    const auto it = findById(media, mediaId);
    return it == media.end() ? nullptr : it->get();
}

const Sequence* ProjectState::findSequence(SequenceId sequenceId) const {
    const auto it = findById(sequences, sequenceId);
    return it == sequences.end() ? nullptr : it->get();
}

SequencePtr ProjectState::sequencePtr(SequenceId sequenceId) const {
    const auto it = findById(sequences, sequenceId);
    return it == sequences.end() ? nullptr : *it;
}

const Bin* ProjectState::findBin(BinId binId) const {
    for (const auto& bin : bins) {
        if (bin.id == binId) {
            return &bin;
        }
    }
    return nullptr;
}

ProjectState ProjectState::withSequence(SequencePtr sequence) const {
    ProjectState next = *this;
    const auto it = std::lower_bound(next.sequences.begin(), next.sequences.end(), sequence->id,
                                     [](const SequencePtr& s, SequenceId v) { return s->id < v; });
    assert(it != next.sequences.end() && (*it)->id == sequence->id);
    *it = std::move(sequence);
    return next;
}

bool ProjectState::operator==(const ProjectState& o) const {
    return ids == o.ids && bins == o.bins && equalValues(media, o.media) && equalValues(sequences, o.sequences) &&
           aiNoCloud == o.aiNoCloud && assets == o.assets && redactions == o.redactions && speakers == o.speakers &&
           transcriptEdits == o.transcriptEdits && captionPresets == o.captionPresets && reviewNotes == o.reviewNotes &&
           brandKit == o.brandKit && templates == o.templates && colorManagement == o.colorManagement &&
           textStyles == o.textStyles && freeform == o.freeform;
}

bool isSameRevision(const ProjectState& a, const ProjectState& b) {
    // Assets are few and small: compared by value, so an asset-only edit is never skipped as a no-op.
    return a.ids == b.ids && a.bins == b.bins && sameNodes(a.media, b.media) && sameNodes(a.sequences, b.sequences) &&
           a.aiNoCloud == b.aiNoCloud && a.assets == b.assets && a.redactions == b.redactions &&
           a.speakers == b.speakers && a.transcriptEdits == b.transcriptEdits && a.captionPresets == b.captionPresets &&
           a.reviewNotes == b.reviewNotes && a.brandKit == b.brandKit && a.templates == b.templates && a.colorManagement == b.colorManagement &&
           a.textStyles == b.textStyles && a.freeform == b.freeform;
}

const FreeformView* ProjectState::findFreeform(BinId bin) const {
    const auto it = std::lower_bound(freeform.begin(), freeform.end(), bin,
                                     [](const FreeformView& v, BinId b) { return v.bin < b; });
    return it != freeform.end() && it->bin == bin ? &*it : nullptr;
}

// --- Search Bins (0.2, P.3) ---------------------------------------------------------------------------

bool isBinSearchField(std::string_view field) {
    return std::find(kBinSearchFields.begin(), kBinSearchFields.end(), field) != kBinSearchFields.end();
}

bool isBinSearchType(std::string_view type) {
    return std::find(kBinSearchTypes.begin(), kBinSearchTypes.end(), type) != kBinSearchTypes.end();
}

namespace {

/// "YYYY-MM-DD" with a month 01..12 and a day 01..31.
bool isIsoDate(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return false;
    }
    for (const std::size_t i : {0, 1, 2, 3, 5, 6, 8, 9}) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
    }
    const int month = (text[5] - '0') * 10 + (text[6] - '0');
    const int day = (text[8] - '0') * 10 + (text[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

} // namespace

std::optional<std::string> binSearchProblem(const BinSearch& search) {
    for (const std::string* text : {&search.text, &search.field, &search.type, &search.from, &search.to}) {
        if (text->size() > BinSearch::kMaxTextBytes || !edit::isValidUtf8(*text)) {
            return "a search bin's query text must be UTF-8 of at most " + std::to_string(BinSearch::kMaxTextBytes) + " bytes";
        }
    }
    if (!search.field.empty() && !isBinSearchField(search.field)) {
        return "a search bin cannot search the field '" + search.field + "'";
    }
    if (!search.type.empty() && !isBinSearchType(search.type)) {
        return "a search bin cannot find the type '" + search.type + "'";
    }
    if (search.label < -1 || search.label > 15) {
        return "a search bin's label must be -1 (any) or 0..15";
    }
    if ((!search.from.empty() && !isIsoDate(search.from)) || (!search.to.empty() && !isIsoDate(search.to))) {
        return "a search bin's dates must be YYYY-MM-DD";
    }
    return std::nullopt;
}

const GraphicTemplate* ProjectState::findTemplate(std::string_view id) const {
    const auto it = std::lower_bound(templates.begin(), templates.end(), id,
                                     [](const GraphicTemplate& t, std::string_view v) { return t.id < v; });
    return it != templates.end() && it->id == id ? &*it : nullptr;
}

const ProjectTextStyle* ProjectState::findTextStyle(std::string_view name) const {
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    const auto it = std::find_if(textStyles.begin(), textStyles.end(), [&](const ProjectTextStyle& s) {
        return s.name.size() == name.size() &&
               std::equal(s.name.begin(), s.name.end(), name.begin(), [&](char a, char b) { return lower(a) == lower(b); });
    });
    return it != textStyles.end() ? &*it : nullptr;
}

const ReviewNote* ProjectState::findReviewNote(ReviewNoteId noteId) const {
    const auto it = std::lower_bound(reviewNotes.begin(), reviewNotes.end(), noteId,
                                     [](const ReviewNote& n, ReviewNoteId v) { return n.id < v; });
    return it != reviewNotes.end() && it->id == noteId ? &*it : nullptr;
}

// --- assets -------------------------------------------------------------------------------------------

const ProjectAsset* ProjectState::findAsset(AssetId assetId) const {
    const auto it = std::lower_bound(assets.begin(), assets.end(), assetId,
                                     [](const ProjectAsset& a, AssetId v) { return a.id < v; });
    return it != assets.end() && it->id == assetId ? &*it : nullptr;
}

const Redaction* ProjectState::findRedaction(RedactionId redactionId) const {
    const auto it = std::lower_bound(redactions.begin(), redactions.end(), redactionId,
                                     [](const Redaction& r, RedactionId v) { return r.id < v; });
    return it != redactions.end() && it->id == redactionId ? &*it : nullptr;
}

const Speaker* ProjectState::findSpeaker(SpeakerId speakerId) const {
    const auto it = std::lower_bound(speakers.begin(), speakers.end(), speakerId,
                                     [](const Speaker& s, SpeakerId v) { return s.id < v; });
    return it != speakers.end() && it->id == speakerId ? &*it : nullptr;
}

const TranscriptEdits* ProjectState::findTranscriptEdits(MediaId mediaId, std::int32_t audioStream) const {
    const auto key = std::pair{mediaId, audioStream};
    const auto it = std::lower_bound(transcriptEdits.begin(), transcriptEdits.end(), key,
                                     [](const TranscriptEdits& e, const std::pair<MediaId, std::int32_t>& k) {
                                         return std::pair{e.media, e.audioStream} < k;
                                     });
    return it != transcriptEdits.end() && it->media == mediaId && it->audioStream == audioStream ? &*it : nullptr;
}

namespace {

bool sameNameIgnoringCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
               return lower(x) == lower(y);
           });
}

} // namespace

const CaptionPreset* ProjectState::findCaptionPreset(std::string_view name) const {
    for (const CaptionPreset& p : captionPresets) {
        if (sameNameIgnoringCase(p.name, name)) {
            return &p;
        }
    }
    return nullptr;
}

std::string assetReference(AssetId asset) { return "asset:" + std::to_string(asset.value); }

AssetId assetOfReference(std::string_view text) {
    constexpr std::string_view prefix = "asset:";
    if (!text.starts_with(prefix) || text.size() == prefix.size() || text.size() > prefix.size() + 20) {
        return {};
    }
    std::uint64_t value = 0;
    for (const char c : text.substr(prefix.size())) {
        if (c < '0' || c > '9' || value > (UINT64_MAX - 9) / 10) {
            return {};
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return AssetId{value};
}

std::vector<std::string> assetReferenceWarnings(const ProjectState& state) {
    std::vector<std::string> out;
    const auto check = [&](const EffectStack& stack, const std::string& owner) {
        for (const EffectInstance& e : stack) {
            for (const EffectData& d : e.data) {
                const std::string* text = d.text();
                const AssetId asset = text ? assetOfReference(*text) : AssetId{};
                if (asset.isValid() && !state.findAsset(asset)) {
                    out.push_back(owner + ", effect " + std::to_string(e.id.value) + ": " + d.key + " -> " + *text);
                }
            }
        }
    };
    for (const SequencePtr& seq : state.sequences) {
        check(seq->effects, "sequence " + std::to_string(seq->id.value));
        seq->forEachTrack([&](const Track& t) {
            check(t.effects, "track " + std::to_string(t.id.value));
            for (const Clip& c : t.clips) {
                check(c.effects(), "clip " + std::to_string(c.id.value));
            }
        });
    }
    return out;
}

std::vector<std::string> subclipRangeWarnings(const ProjectState& state) {
    std::vector<std::string> out;
    for (const MediaPtr& m : state.media) {
        if (!m->subclip) {
            continue;
        }
        std::optional<Ticks> extent;
        for (std::size_t i = 0; i < m->info.video.size() + m->info.audio.size(); ++i) {
            const bool video = i < m->info.video.size();
            const StreamRef stream{video ? StreamKind::Video : StreamKind::Audio,
                                   static_cast<std::int32_t>(video ? i : i - m->info.video.size())};
            if (const auto e = m->sourceExtent(stream)) {
                extent = extent ? std::max(*extent, *e) : *e;
            }
        }
        if (extent && m->subclip->range.end() > *extent) {
            const double seconds = (m->subclip->range.end() - *extent).toSeconds();
            out.push_back("subclip '" + m->name + "' runs " + std::to_string(seconds) +
                          " s past the end of its media; it is clamped to the media");
        }
    }
    return out;
}

// --- transitions --------------------------------------------------------------------------------------

std::optional<std::string> transitionProblem(const ProjectState& state, const Track& track, const Transition& t,
                                             Validation level) {
    if (t.before.value < 0 || t.after.value < 0 || t.duration().value <= 0) {
        return "has a negative or zero length";
    }
    if (!t.outgoing.isValid() && !t.incoming.isValid()) {
        return "has no clip";
    }
    const auto outIndex = t.outgoing.isValid() ? track.indexOf(t.outgoing) : std::nullopt;
    const auto inIndex = t.incoming.isValid() ? track.indexOf(t.incoming) : std::nullopt;
    if ((t.outgoing.isValid() && !outIndex) || (t.incoming.isValid() && !inIndex)) {
        return "refers to a clip that is not on its track";
    }
    const Clip* a = outIndex ? &track.clips[*outIndex] : nullptr;
    const Clip* b = inIndex ? &track.clips[*inIndex] : nullptr;
    if (a && b && a->end() != b->start()) {
        return "joins clips that do not meet at an edit point";
    }
    if (!b && t.after.value != 0) {
        return "extends past the end of its only clip";
    }
    if (!a && t.before.value != 0) {
        return "starts before its only clip";
    }
    if ((t.alignment == TransitionAlignment::StartAtCut && t.before.value != 0) ||
        (t.alignment == TransitionAlignment::EndAtCut && t.after.value != 0)) {
        return "has lengths that do not match its alignment";
    }
    // A one-sided transition stays inside its clip; a two-sided one may not be longer than either side.
    if (a && t.before > a->duration()) {
        return "is longer than its outgoing clip";
    }
    if (b && t.after > b->duration()) {
        return "is longer than its incoming clip";
    }
    if (level == Validation::Strict && a && b) {
        // A plays `after` past its Out point and B starts `before` ahead of its In point (direction-aware:
        // a reversed clip's handles lie on the other side of its source range; a freeze frame has any).
        const Transition::Handles handles = t.handles();
        const auto boundsOf = [&](const Clip& c) -> SourceBounds {
            const MediaItem* media = state.findMedia(c.media);
            return media ? media->sourceBounds(c.stream) : SourceBounds{};
        };
        const SourceBounds aBounds = boundsOf(*a);
        const SourceBounds bBounds = boundsOf(*b);
        if (const auto room = a->tailRoom(aBounds.start, aBounds.end); room && handles.outgoingTail > *room) {
            return "needs more source media after its outgoing clip than there is";
        }
        if (const auto room = b->headRoom(bBounds.start, bBounds.end); room && handles.incomingHead > *room) {
            return "needs more source media before its incoming clip than there is";
        }
    }
    const EffectKind kind = track.kind == TrackKind::Video ? EffectKind::VideoTransition : EffectKind::AudioTransition;
    if (const EffectDescriptor* d = EffectRegistry::builtin().find(t.effect)) {
        if (d->kind != kind) {
            return "uses " + t.effect + ", which is not a " + std::string(toString(kind));
        }
        for (const EffectParam& p : t.params) {
            if (const EffectParamInfo* info = d->param(p.key);
                info && (!p.value.isConstant() || !p.value.isValid(info->minValue, info->maxValue))) {
                return "has parameter " + p.key + " outside its range or animated";
            } else if (info && info->isChoice() && info->optionLabel(p.value.constant()).empty()) {
                return "has parameter " + p.key + " that is not one of its options";
            }
        }
    }
    for (std::size_t i = 0; i < t.params.size(); ++i) {
        if (i > 0 && !(t.params[i - 1].key < t.params[i].key)) {
            return "has unsorted or duplicate parameter keys";
        }
        if (!t.params[i].value.isConstant() || !t.params[i].value.isValid(-1e300, 1e300)) {
            return "has parameter " + t.params[i].key + " that is animated or not finite";
        }
    }
    return std::nullopt;
}

std::string_view toString(TransitionAlignment alignment) {
    switch (alignment) {
    case TransitionAlignment::Center:
        return "center";
    case TransitionAlignment::StartAtCut:
        return "startAtCut";
    case TransitionAlignment::EndAtCut:
        return "endAtCut";
    case TransitionAlignment::Custom:
        return "custom";
    }
    return "center";
}

std::optional<TransitionAlignment> transitionAlignmentFromString(std::string_view text) {
    for (const auto a : {TransitionAlignment::Center, TransitionAlignment::StartAtCut, TransitionAlignment::EndAtCut,
                         TransitionAlignment::Custom}) {
        if (toString(a) == text) {
            return a;
        }
    }
    return std::nullopt;
}

bool alignmentFits(TransitionAlignment alignment, Ticks before, Ticks after, Ticks frame) {
    switch (alignment) {
    case TransitionAlignment::Center:
        return before - after <= frame && after - before <= frame;
    case TransitionAlignment::StartAtCut:
        return before.value == 0;
    case TransitionAlignment::EndAtCut:
        return after.value == 0;
    case TransitionAlignment::Custom:
        return true;
    }
    return false;
}

TransitionAlignment alignmentOf(Ticks before, Ticks after, Ticks frame) {
    if (before.value == 0) {
        return TransitionAlignment::StartAtCut;
    }
    if (after.value == 0) {
        return TransitionAlignment::EndAtCut;
    }
    return alignmentFits(TransitionAlignment::Center, before, after, frame) ? TransitionAlignment::Center
                                                                            : TransitionAlignment::Custom;
}

std::pair<Ticks, Ticks> splitTransition(Ticks duration, TransitionAlignment alignment, Ticks offset, Rate rate) {
    switch (alignment) {
    case TransitionAlignment::Center: {
        const Ticks before = snapToFrame(Ticks{duration.value / 2}, rate);
        return {before, duration - before};
    }
    case TransitionAlignment::StartAtCut:
        return {Ticks{}, duration};
    case TransitionAlignment::EndAtCut:
        return {duration, Ticks{}};
    case TransitionAlignment::Custom: {
        const Ticks before = std::clamp(offset, Ticks{}, duration);
        return {before, duration - before};
    }
    }
    return {Ticks{}, duration};
}

std::optional<std::string> clipBoundsProblem(const ProjectState& state, const Clip& clip) {
    if (clip.isNested() || clip.hasUnknownSource() || !clip.media.isValid()) {
        return std::nullopt;
    }
    const MediaItem* media = state.findMedia(clip.media);
    if (!media || !media->hasStream(clip.stream)) {
        return std::nullopt; // Structure reports these
    }
    const SourceBounds bounds = media->sourceBounds(clip.stream);
    if (const Curve* remap = clip.timeRemap()) {
        if (remapProblem(*remap) || !checkedAdd(clip.sourceIn, clip.duration())) {
            return std::nullopt; // malformed: Structure reports it
        }
        // Keys lie inside the media, and so does every frame the clip shows.
        if (const auto extent = bounds.end) {
            const bool keysInside = std::all_of(remap->keys.begin(), remap->keys.end(), [&](const Keyframe& k) {
                return k.value <= static_cast<double>(extent->value);
            });
            if (!keysInside || remapShownRange(*remap, clip.sourceIn, clip.sourceOut()).second >= *extent) {
                return "is time-remapped past the end of its source media";
            }
        }
        if (bounds.start.value > 0) {
            const bool keysAfter = std::all_of(remap->keys.begin(), remap->keys.end(), [&](const Keyframe& k) {
                return k.value >= static_cast<double>(bounds.start.value);
            });
            if (!keysAfter || remapShownRange(*remap, clip.sourceIn, clip.sourceOut()).first < bounds.start) {
                return "is time-remapped before the start of its subclip";
            }
        }
        return std::nullopt;
    }
    if (!clip.speed.isValid()) {
        return std::nullopt;
    }
    if (const auto extent = bounds.end; extent && clip.sourceOut() > *extent) {
        return "plays past the end of its source media";
    } else if (extent && clip.isFreeze() && clip.sourceIn >= *extent) {
        return "holds a frame past the end of its source media";
    } else if (bounds.start.value > 0 && clip.sourceIn < bounds.start) {
        return "plays media before the start of its subclip";
    }
    return std::nullopt;
}

std::optional<Ticks> transitionEditPoint(const Track& track, const Transition& t) {
    if (t.outgoing.isValid()) {
        if (const Clip* a = track.findClip(t.outgoing)) {
            return a->end();
        }
        return std::nullopt;
    }
    if (const Clip* b = track.findClip(t.incoming)) {
        return b->start();
    }
    return std::nullopt;
}

// --- validate -----------------------------------------------------------------------------------------

namespace {

constexpr std::size_t kMaxReported = 32;

class Validator {
public:
    Validator(const ProjectState& state, Validation level) : m_state(state), m_level(level) {}

    std::expected<void, Error> run() {
        if (m_state.ids.next == 0) {
            fail("id generator is at 0");
        }
        // A file may claim any next id; near 2^64 the generator would wrap to 0 and hand out duplicates (edits are not
        // validated in release builds). 2^53 (exact in every JSON reader) is beyond any real project's ids.
        if (m_state.ids.next > (std::uint64_t{1} << 53)) {
            fail("id generator is out of range");
        }
        checkBins();
        checkMedia();
        checkAssets();
        checkRedactions();
        checkSpeakers();
        checkCaptionPresets();
        checkGraphicsLibrary();
        checkSequences();
        checkColorManagement();
        checkReviewNotes();
        checkFreeform();
        if (m_problems.empty()) {
            return {};
        }
        std::string message = "invalid project state:";
        for (const auto& p : m_problems) {
            message += "\n - ";
            message += p;
        }
        if (m_dropped > 0) {
            message += "\n - ... and " + std::to_string(m_dropped) + " more";
        }
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::move(message)});
    }

private:
    void fail(std::string problem) {
        if (m_problems.size() < kMaxReported) {
            m_problems.push_back(std::move(problem));
        } else {
            ++m_dropped;
        }
    }

    /// Registers an object id: valid, below the generator and unique among all objects.
    void claim(std::uint64_t id, const std::string& what) {
        if (id == 0) {
            fail(what + " has no id");
            return;
        }
        if (id >= m_state.ids.next) {
            fail(what + " id " + std::to_string(id) + " is not below the id generator");
        }
        if (!m_ids.insert(id).second) {
            fail(what + " id " + std::to_string(id) + " is used twice");
        }
    }

    bool binExists(BinId bin) const { return !bin.isValid() || m_state.findBin(bin) != nullptr; }
    /// 0.2 (P.3): a search bin holds nothing (it lists what its query finds).
    bool isSearchBin(BinId bin) const {
        const Bin* b = bin.isValid() ? m_state.findBin(bin) : nullptr;
        return b && b->isSearch();
    }

    void checkBins() {
        for (const auto& bin : m_state.bins) {
            claim(bin.id.value, "bin '" + bin.name + "'");
            if (!binExists(bin.parent)) {
                fail("bin '" + bin.name + "' has a missing parent");
            }
            if (isSearchBin(bin.parent)) {
                fail("bin '" + bin.name + "' is inside a search bin");
            }
            if (bin.search) {
                if (const auto problem = binSearchProblem(*bin.search)) {
                    fail("search bin '" + bin.name + "': " + *problem);
                }
            }
        }
        // No cycles: walking up from any bin reaches the root within bins.size() steps.
        for (const auto& bin : m_state.bins) {
            BinId cursor = bin.parent;
            std::size_t steps = 0;
            while (cursor.isValid() && steps <= m_state.bins.size()) {
                const Bin* parent = m_state.findBin(cursor);
                cursor = parent ? parent->parent : BinId{};
                ++steps;
            }
            if (steps > m_state.bins.size()) {
                fail("bin '" + bin.name + "' is part of a parent cycle");
            }
        }
    }

    /// Markers of a sequence or a media item: each within the bounds (markerProblem), sorted, and not too many.
    void checkMarkers(const std::vector<Marker>& markers, const std::string& what) {
        if (markers.size() > kMaxMarkers) {
            fail(what + " has more than " + std::to_string(kMaxMarkers) + " markers");
        }
        for (std::size_t i = 0; i < markers.size(); ++i) {
            if (const auto problem = markerProblem(markers[i])) {
                fail(what + " has a marker that is not allowed: " + *problem);
            }
            if (i > 0 && markers[i - 1].start > markers[i].start) {
                fail(what + " has markers out of order");
            }
        }
    }

    void checkMedia() {
        MediaId previous{};
        for (const auto& item : m_state.media) {
            if (!item) {
                fail("null media item");
                continue;
            }
            const std::string what = "media '" + item->name + "'";
            claim(item->id.value, what);
            if (item->id <= previous) {
                fail("media items are not sorted by id");
            }
            previous = item->id;
            if (isSearchBin(item->bin)) {
                fail("media '" + item->name + "' is inside a search bin");
            }
            if (!binExists(item->bin)) {
                fail(what + " is in a missing bin");
            }
            if (item->info.duration.value < 0) {
                fail(what + " has a negative duration");
            }
            const MediaLocator& loc = item->locator;
            if (loc.fingerprint.size < -1) {
                fail(what + " has an invalid fingerprint size");
            }
            if (loc.proxy) {
                if (loc.proxy->path.empty()) {
                    fail(what + " has a proxy without a path");
                }
                if (loc.proxy->provenance.source.size < -1) {
                    fail(what + " has a proxy with an invalid source fingerprint");
                }
            }
            if (item->generator) {
                checkGenerator(*item, what);
            }
            if (item->generated) {
                checkGenerated(*item->generated, what);
            }
            checkAudioTakes(*item, what);
            if (item->derived) {
                checkDerived(*item->derived, what);
            }
            if (item->recording) {
                checkRecording(*item->recording, what);
            }
            if (item->channels) { // A.9
                if (const auto problem = channelMapProblem(*item->channels)) {
                    fail(what + "'s channel map: " + *problem);
                }
            }
            // Source marks: a mark past the media's end is fine here (relinked to a shorter file); it is clamped
            // when used.
            if (item->markIn && item->markIn->value < 0) {
                fail(what + " has a negative source In");
            }
            if (item->markOut && item->markOut->value <= 0) {
                fail(what + " has a source Out at or before the start");
            }
            if (item->markIn && item->markOut && *item->markOut <= *item->markIn) {
                fail(what + " has a source Out at or before its In");
            }
            checkMarkers(item->markers, what);
            checkLibraryMetadata(*item, what);
        }
    }

    /// M5k: the label, bounded comment and description, the poster frame and a subclip's range. A subclip's master is
    /// a weak reference (it may be gone), and a range past the end of a shorter relinked file is a warning
    /// (subclipRangeWarnings), not invalid state.
    void checkLibraryMetadata(const MediaItem& item, const std::string& what) {
        if (item.colorIndex < 0 || item.colorIndex > 15) {
            fail(what + " has a label colour outside 0..15");
        }
        if (item.comment.size() > MediaItem::kMaxTextBytes || item.description.size() > MediaItem::kMaxTextBytes) {
            fail(what + " has a comment or description longer than " + std::to_string(MediaItem::kMaxTextBytes) +
                 " bytes");
        }
        if (item.posterFrame && item.posterFrame->value < 0) {
            fail(what + " has a negative poster frame");
        }
        // 0.2 (P.3): clip metadata, bounded UTF-8.
        const ClipMetadata& md = item.metadata;
        for (const std::string* field : {&md.scene, &md.shot, &md.take, &md.client, &md.status, &md.cameraAngle,
                                         &md.cameraLabel, &md.cameraModel, &md.shotDate, &md.shotLocation}) {
            if (field->size() > ClipMetadata::kMaxFieldBytes || !edit::isValidUtf8(*field)) {
                fail(what + " has a clip metadata field that is not UTF-8 of at most " +
                     std::to_string(ClipMetadata::kMaxFieldBytes) + " bytes");
                break;
            }
        }
        if (md.logNote.size() > MediaItem::kMaxTextBytes || !edit::isValidUtf8(md.logNote)) {
            fail(what + " has a log note that is not UTF-8 of at most " + std::to_string(MediaItem::kMaxTextBytes) + " bytes");
        }
        if (item.importSource && (item.importSource->project.size() > ImportSource::kMaxPathBytes ||
                                  !edit::isValidUtf8(item.importSource->project))) {
            fail(what + " has an import source path that is not UTF-8 of at most " +
                 std::to_string(ImportSource::kMaxPathBytes) + " bytes");
        }
        if (item.subclip) {
            const SubclipSpec& s = *item.subclip;
            if (s.range.start.value < 0 || s.range.duration.value <= 0) {
                fail(what + " has an empty or negative subclip range");
            }
            if (s.master == item.id) {
                fail(what + " is a subclip of itself");
            }
            if (item.generator) {
                fail(what + " is a generated item and a subclip");
            }
        }
        // P.2 (E5): Interpret Footage, Modify > Timecode, the still's default duration.
        const auto ratio = [](Rate r) { return static_cast<double>(r.num) / static_cast<double>(r.den); };
        if (const auto& in = item.interpretation) {
            if (in->frameRate && (!in->frameRate->isValid() || ratio(*in->frameRate) < 1.0 / 1000.0 ||
                                  ratio(*in->frameRate) > 1000.0)) {
                fail(what + " is interpreted at a frame rate outside 0.001 to 1000 fps");
            }
            if (in->pixelAspect && (!in->pixelAspect->isValid() || ratio(*in->pixelAspect) < 0.1 ||
                                    ratio(*in->pixelAspect) > 10.0)) {
                fail(what + " is interpreted with a pixel aspect outside 0.1 to 10");
            }
        }
        if (item.timecode) {
            if (item.timecode->start && (item.timecode->start->value < 0 || *item.timecode->start > kMaxStartTimecode)) {
                fail(what + " has a start timecode outside 00:00:00:00 to 24 hours");
            }
            if (item.timecode->reelName.size() > MediaTimecode::kMaxReelNameBytes) {
                fail(what + " has a reel name longer than " + std::to_string(MediaTimecode::kMaxReelNameBytes) + " bytes");
            }
        }
        if (item.stillDuration && (item.stillDuration->value <= 0 || *item.stillDuration > kMaxStartTimecode)) {
            fail(what + " has a still duration outside 1 tick to 24 hours");
        }
        if (item.madeOffline && item.generator) {
            fail(what + " is a generated item and cannot be made offline");
        }
    }

    /// 0.2 (P.3): Freeform views, one per bin (sorted by bin id, plain bins that exist), every value in range. Placements
    /// are weak references (an item since removed or moved is not shown), so they are not checked against the items.
    void checkFreeform() {
        const auto checkArrangement = [&](const FreeformArrangement& a, const std::string& what) {
            if (!(a.scale >= kFreeformMinSize && a.scale <= kFreeformMaxSize)) {
                fail(what + " has a clip size outside " + std::to_string(kFreeformMinSize) + ".." +
                     std::to_string(kFreeformMaxSize));
            }
            if (a.items.size() > kMaxFreeformItems) {
                fail(what + " places more than " + std::to_string(kMaxFreeformItems) + " items");
                return;
            }
            for (const FreeformPlacement& p : a.items) {
                if (p.id == 0 || !std::isfinite(p.x) || !std::isfinite(p.y) || std::abs(p.x) > kFreeformMaxCoordinate ||
                    std::abs(p.y) > kFreeformMaxCoordinate || !(p.size >= kFreeformMinSize && p.size <= kFreeformMaxSize)) {
                    fail(what + " has a placement out of range");
                    return;
                }
            }
        };
        for (std::size_t i = 0; i < m_state.freeform.size(); ++i) {
            const FreeformView& v = m_state.freeform[i];
            const std::string what = v.bin.isValid() ? "the Freeform view of bin " + std::to_string(v.bin.value)
                                                     : std::string("the Freeform view of the root bin");
            if (i > 0 && !(m_state.freeform[i - 1].bin < v.bin)) {
                fail("Freeform views are not sorted by unique bin");
            }
            if (!binExists(v.bin)) {
                fail(what + " is of a missing bin");
            } else if (isSearchBin(v.bin)) {
                fail(what + " is of a search bin");
            }
            checkArrangement(v.current, what);
            if (v.layouts.size() > FreeformView::kMaxLayouts) {
                fail(what + " has more than " + std::to_string(FreeformView::kMaxLayouts) + " saved layouts");
            }
            for (std::size_t k = 0; k < v.layouts.size(); ++k) {
                const FreeformLayout& l = v.layouts[k];
                if (l.name.empty() || l.name.size() > FreeformLayout::kMaxNameBytes || !edit::isValidUtf8(l.name)) {
                    fail(what + " has a saved layout without a valid name");
                }
                for (std::size_t j = 0; j < k; ++j) {
                    if (sameNameIgnoringCase(v.layouts[j].name, l.name)) {
                        fail(what + " has two saved layouts named '" + l.name + "'");
                    }
                }
                checkArrangement(l.arrangement, what + ", layout '" + l.name + "'");
            }
        }
    }

    /// Assets: sorted by unique ids below the generator, with a kind and a file path. References to them are not
    /// checked (assetReferenceWarnings): a missing LUT is a warning, never invalid state.
    void checkAssets() {
        AssetId previous{};
        for (const ProjectAsset& asset : m_state.assets) {
            const std::string what = "asset '" + asset.name + "'";
            claim(asset.id.value, what);
            if (asset.id <= previous) {
                fail("assets are not sorted by id");
            }
            previous = asset.id;
            if (asset.kind.empty()) {
                fail(what + " has no kind");
            }
            if (asset.locator.proxy) {
                fail(what + " has a proxy");
            }
            if (asset.locator.fingerprint.size < -1) {
                fail(what + " has an invalid fingerprint size");
            }
        }
    }

    /// Redactions (I.6): sorted by unique ids below the generator, each well formed (redactionProblem). Their media
    /// need not exist: a redaction of a removed item applies to nothing.
    void checkRedactions() {
        if (m_state.redactions.size() > kMaxRedactions) {
            fail("more than " + std::to_string(kMaxRedactions) + " redactions");
        }
        RedactionId previous{};
        for (const Redaction& r : m_state.redactions) {
            const std::string what = "redaction '" + r.name + "'";
            claim(r.id.value, what);
            if (r.id <= previous) {
                fail("redactions are not sorted by id");
            }
            previous = r.id;
            if (const auto problem = redactionProblem(r)) {
                fail(what + ": " + *problem);
            }
        }
    }

    /// Speakers and transcript edits (C.7): speakers sorted by unique ids below the generator with a name; edits
    /// sorted by (media, stream), well formed, referring to existing speakers. Their media need not exist.
    void checkSpeakers() {
        if (m_state.speakers.size() > kMaxSpeakers) {
            fail("more than " + std::to_string(kMaxSpeakers) + " speakers");
        }
        SpeakerId previous{};
        for (const Speaker& s : m_state.speakers) {
            const std::string what = "speaker " + std::to_string(s.id.value);
            claim(s.id.value, what);
            if (s.id <= previous) {
                fail("speakers are not sorted by id");
            }
            previous = s.id;
            if (s.name.empty() || s.name.size() > kMaxSpeakerNameBytes || !isValidCaptionText(s.name) ||
                s.name.find('\n') != std::string::npos) {
                fail(what + " needs a one-line UTF-8 name of 1.." + std::to_string(kMaxSpeakerNameBytes) + " bytes");
            }
            if (s.color > 0xFFFFFFu) {
                fail(what + " has a colour outside 0xRRGGBB");
            }
        }
        if (m_state.transcriptEdits.size() > kMaxTranscriptEdits) {
            fail("more than " + std::to_string(kMaxTranscriptEdits) + " transcript edit records");
        }
        for (std::size_t i = 0; i < m_state.transcriptEdits.size(); ++i) {
            const TranscriptEdits& e = m_state.transcriptEdits[i];
            const std::string what = "transcript edits of media " + std::to_string(e.media.value) + " stream " +
                                     std::to_string(e.audioStream);
            if (i > 0 && !(std::pair{m_state.transcriptEdits[i - 1].media, m_state.transcriptEdits[i - 1].audioStream} <
                           std::pair{e.media, e.audioStream})) {
                fail("transcript edits are not sorted by media and stream");
            }
            if (e.empty()) {
                fail(what + " are empty");
            }
            if (const auto problem = transcriptEditsProblem(e)) {
                fail(what + ": " + *problem);
            }
            for (const SpeakerLabel& l : e.labels) {
                if (!m_state.findSpeaker(l.speaker)) {
                    fail(what + " map a label to missing speaker " + std::to_string(l.speaker.value));
                }
            }
            for (const SpeakerSpan& span : e.speakerSpans) {
                if (!m_state.findSpeaker(span.speaker)) {
                    fail(what + " assign missing speaker " + std::to_string(span.speaker.value));
                }
            }
        }
    }

    /// Review notes (C.9): sorted, bounded, on an existing sequence.
    void checkReviewNotes() {
        if (m_state.reviewNotes.size() > kMaxReviewNotes) {
            fail("more than " + std::to_string(kMaxReviewNotes) + " review notes");
        }
        ReviewNoteId previous{};
        for (const ReviewNote& n : m_state.reviewNotes) {
            const std::string what = "review note " + std::to_string(n.id.value);
            claim(n.id.value, what);
            if (n.id <= previous) {
                fail("review notes are not sorted by id");
            }
            previous = n.id;
            if (!m_state.findSequence(n.sequence)) {
                fail(what + " is on a missing sequence");
            }
            if (const auto problem = reviewNoteProblem(n)) {
                fail(what + ": " + *problem);
            }
        }
    }

    /// The brand kit and the template library (C.13): valid, bounded, templates sorted by unique id.
    void checkGraphicsLibrary() {
        if (m_state.brandKit) {
            if (const auto problem = brandKitProblem(*m_state.brandKit)) {
                fail("the brand kit: " + *problem);
            }
        }
        if (m_state.templates.size() > kMaxProjectTemplates) {
            fail("more than " + std::to_string(kMaxProjectTemplates) + " graphic templates");
        }
        for (std::size_t i = 0; i < m_state.templates.size(); ++i) {
            const GraphicTemplate& t = m_state.templates[i];
            if (i > 0 && !(m_state.templates[i - 1].id < t.id)) {
                fail("graphic templates are not sorted by unique id");
            }
            if (const auto problem = templateProblem(t)) {
                fail(*problem);
            }
        }
        // 0.2: text styles: bounded, valid, unique names (case-insensitive).
        if (m_state.textStyles.size() > kMaxProjectTextStyles) {
            fail("more than " + std::to_string(kMaxProjectTextStyles) + " text styles");
        }
        for (std::size_t i = 0; i < m_state.textStyles.size(); ++i) {
            const ProjectTextStyle& s = m_state.textStyles[i];
            if (const auto problem = textStyleProblem(s)) {
                fail(*problem);
            }
            for (std::size_t k = 0; k < i; ++k) {
                if (sameNameIgnoringCase(m_state.textStyles[k].name, s.name)) {
                    fail("text style '" + s.name + "' appears twice");
                }
            }
        }
    }

    /// Caption presets (C.7): unique names (case-insensitive) that are not built-in, valid styles.
    void checkCaptionPresets() {
        if (m_state.captionPresets.size() > kMaxCaptionPresets) {
            fail("more than " + std::to_string(kMaxCaptionPresets) + " caption presets");
        }
        for (std::size_t i = 0; i < m_state.captionPresets.size(); ++i) {
            const CaptionPreset& p = m_state.captionPresets[i];
            const std::string what = "caption preset '" + p.name + "'";
            if (p.name.empty() || p.name.size() > kMaxCaptionNameBytes || !isValidCaptionText(p.name) ||
                p.name.find('\n') != std::string::npos) {
                fail("a caption preset needs a one-line UTF-8 name of 1.." + std::to_string(kMaxCaptionNameBytes) +
                     " bytes");
            }
            if (findBuiltinCaptionPreset(p.name)) {
                fail(what + " has the name of a built-in preset");
            }
            for (std::size_t k = 0; k < i; ++k) {
                if (sameNameIgnoringCase(m_state.captionPresets[k].name, p.name)) {
                    fail(what + " appears twice");
                }
            }
            if (const auto problem = captionStyleProblem(p.style)) {
                fail(what + ": " + *problem);
            }
        }
    }

    /// A synthetic item has no file: nothing to find, fingerprint, relink or proxy. A title is one unbounded still
    /// video stream with text in UTF-8 and a font family. Unknown generator kinds are kept as they are.
    void checkGenerator(const MediaItem& item, const std::string& what) {
        const MediaLocator& loc = item.locator;
        if (!loc.path.empty() || !loc.relativePath.empty() || loc.proxy || loc.offline ||
            !(loc.fingerprint == SourceFingerprint{})) {
            fail(what + " is generated but has a file locator (path, fingerprint, proxy or offline state)");
        }
        const GeneratorSpec& g = *item.generator;
        if (g.kind.empty()) {
            fail(what + " has a generator without a kind");
        }
        if (const auto problem = syntheticProblem(item)) { // C.15: synthetic items and adjustment layers
            fail(what + " " + *problem);
        }
        if (g.kind == kGraphicGenerator) {
            if (item.info.video.size() != 1 || !item.info.audio.empty() || !item.info.isStillImage) {
                fail(what + " is a graphic canvas but not exactly one still video stream");
            }
            return;
        }
        if (g.kind != kTitleGenerator) {
            return;
        }
        if (item.info.video.size() != 1 || !item.info.audio.empty() || !item.info.isStillImage) {
            fail(what + " is a title but not exactly one still video stream");
        }
        if (g.fontFamily.empty()) {
            fail(what + " is a title without a font family");
        }
        if (!edit::isValidUtf8(g.text) || !edit::isValidUtf8(g.fontFamily) || !edit::isValidUtf8(g.fontStyle)) {
            fail(what + " has title text or font names that are not UTF-8");
        }
    }

    /// AI provenance: bounded names, a SHA-256 hex prompt hash, a strict ISO-8601 UTC time and a known kind. The
    /// source media is a weak reference (an extension outlives the media it extended), so it is not checked.
    void checkGenerated(const GeneratedProvenance& g, const std::string& what) {
        if (g.provider.empty() || g.provider.size() > GeneratedProvenance::kMaxNameBytes || g.model.empty() ||
            g.model.size() > GeneratedProvenance::kMaxNameBytes) {
            fail(what + " has generated provenance without a provider or model (or one longer than 128 bytes)");
        }
        if (!edit::isValidUtf8(g.provider) || !edit::isValidUtf8(g.model) || !edit::isValidUtf8(g.operation)) {
            fail(what + " has generated provenance that is not UTF-8");
        }
        const bool hex = g.promptHash.size() == 64 && std::all_of(g.promptHash.begin(), g.promptHash.end(), [](char c) {
                             return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                         });
        if (!hex) {
            fail(what + " has a prompt hash that is not 64 lowercase hex digits");
        }
        if (!isIso8601Utc(g.createdAt)) {
            fail(what + " has a generation time that is not ISO-8601 UTC");
        }
        if (g.kind != "video" && g.kind != "image" && g.kind != "audio") {
            fail(what + " has a generated kind other than video, image or audio");
        }
        if (g.operation.empty() || g.operation.size() > GeneratedProvenance::kMaxOperationBytes) {
            fail(what + " has a generation operation that is empty or longer than 32 bytes");
        }
    }

    /// A.4 takes: a file of the item's own audio, so not on generated items; sorted by (stream, kind) and unique; a
    /// non-negative stream, a bounded kind and model in UTF-8, a path. The file need not exist (a missing take plays
    /// the source).
    void checkAudioTakes(const MediaItem& item, const std::string& what) {
        if (item.audioTakes.empty()) {
            return;
        }
        if (item.generator) {
            fail(what + " is generated but has audio takes");
        }
        const AudioTake* previous = nullptr;
        for (const AudioTake& t : item.audioTakes) {
            if (t.stream < 0) {
                fail(what + " has an audio take of a negative stream");
            }
            if (t.kind.empty() || t.kind.size() > AudioTake::kMaxNameBytes || t.model.size() > AudioTake::kMaxNameBytes) {
                fail(what + " has an audio take without a kind (or a kind or model longer than 128 bytes)");
            }
            if (t.path.empty()) {
                fail(what + " has an audio take without a path");
            }
            if (!edit::isValidUtf8(t.kind) || !edit::isValidUtf8(t.model) || !edit::isValidUtf8(t.path)) {
                fail(what + " has an audio take that is not UTF-8");
            }
            if (t.source.size < -1) {
                fail(what + " has an audio take with an invalid source fingerprint");
            }
            if (previous && std::tie(previous->stream, previous->kind) >= std::tie(t.stream, t.kind)) {
                fail(what + " has audio takes that are not sorted by stream and kind, or two of one kind");
            }
            previous = &t;
        }
    }

    /// A.4 derived media: a bounded operation (required), model and part in UTF-8 and an ISO-8601 UTC time. The source
    /// is a weak reference, not checked.
    void checkDerived(const DerivedProvenance& d, const std::string& what) {
        if (d.operation.empty() || d.operation.size() > DerivedProvenance::kMaxNameBytes ||
            d.model.size() > DerivedProvenance::kMaxNameBytes || d.part.size() > DerivedProvenance::kMaxNameBytes) {
            fail(what + " has derived provenance without an operation (or a name longer than 128 bytes)");
        }
        if (!edit::isValidUtf8(d.operation) || !edit::isValidUtf8(d.model) || !edit::isValidUtf8(d.part)) {
            fail(what + " has derived provenance that is not UTF-8");
        }
        if (!isIso8601Utc(d.createdAt)) {
            fail(what + " has a derivation time that is not ISO-8601 UTC");
        }
    }

    /// A.8 recorded media: a positive range, a take from 1, a bounded UTF-8 device name and an ISO-8601 UTC time. The
    /// sequence and track are weak references, not checked (a removed track leaves its takes valid).
    void checkRecording(const RecordingProvenance& r, const std::string& what) {
        if (r.range.duration.value <= 0 || r.range.start.value < 0 || r.take < 1) {
            fail(what + " has a recording without a positive range or take number");
        }
        if (r.device.size() > RecordingProvenance::kMaxDeviceBytes || !edit::isValidUtf8(r.device)) {
            fail(what + " has a recording device name that is too long or not UTF-8");
        }
        if (!isIso8601Utc(r.createdAt)) {
            fail(what + " has a recording time that is not ISO-8601 UTC");
        }
    }

    void checkProperties(const ClipProperties& p, const std::string& what) {
        if (const auto bad = p.firstInvalid()) {
            const ParameterInfo& info = parameterInfo(*bad);
            fail(what + " has parameter " + std::string(info.key) + " outside its range or not finite");
        }
    }

    void checkClip(const Track& track, const Clip& clip, const std::string& trackName) {
        const std::string what = "clip " + std::to_string(clip.id.value) + " ('" + clip.name() + "') on " + trackName;
        claim(clip.id.value, what);
        if (clip.duration().value <= 0) {
            fail(what + " has no duration");
        }
        if (clip.start().value < 0) {
            fail(what + " starts before 0");
        }
        if (clip.sourceIn.value < 0) {
            fail(what + " starts before its source media");
        }
        // A file may hold any int64; times past 2^60 ticks (some 52 days) would wrap when an edit adds two of them
        // (C.11: the clipboard fuzzer found a clip of duration 2^63 - 1 that a project file could carry).
        constexpr std::int64_t kMaxClipTime = std::int64_t{1} << 60;
        if (clip.start().value > kMaxClipTime || clip.duration().value > kMaxClipTime || clip.sourceIn.value > kMaxClipTime) {
            fail(what + " lies beyond the longest timeline");
            return;
        }
        if (!clip.speed.isValid()) {
            fail(what + " has an invalid speed");
        }
        if (clip.isFreeze() && track.kind == TrackKind::Audio) {
            fail(what + " is an audio clip with a frame hold");
        }
        const Curve* remap = clip.timeRemap();
        if (remap) {
            if (const auto problem = remapProblem(*remap)) {
                fail(what + " has a time remap that " + *problem);
                return; // Do not evaluate malformed source values or overflowing segments below.
            }
            if (!checkedAdd(clip.sourceIn, clip.duration())) {
                fail(what + " has a time remap curve range that exceeds the tick range");
                return;
            }
            if (!clip.speed.isUnity() || clip.reversed) {
                fail(what + " is time-remapped but not at speed 100 % forwards (the remap holds its timing)");
            }
        }
        if (clip.stream.kind != streamKindOf(track.kind)) {
            fail(what + " plays a stream of the wrong kind for its track");
        }
        if (clip.linkGroup.isValid()) {
            if (clip.linkGroup.value >= m_state.ids.next) {
                fail(what + " has a link group id that is not below the id generator");
            }
            GroupUse& use = m_groupCounts[clip.linkGroup];
            ++use.count;
            use.onLockedTrack = track.locked;
        }
        if (clip.group.isValid() && clip.group.value >= m_state.ids.next) {
            fail(what + " has a group id that is not below the id generator");
        }
        checkProperties(clip.properties(), what);
        if (const AudioChannelMap* map = clip.channelMap()) { // A.9: audio clips of media only
            if (track.kind != TrackKind::Audio || clip.isNested() || clip.hasUnknownSource()) {
                fail(what + " has a channel map but is not an audio clip of a media item");
            }
            if (const auto problem = channelMapProblem(*map)) {
                fail(what + "'s channel map: " + *problem);
            }
        }
        if (const Graphic* graphic = clip.graphic()) {
            const MediaItem* media = m_state.findMedia(clip.media);
            if (track.kind != TrackKind::Video || !media || !media->isGraphic()) {
                fail(what + " holds a graphic but does not play a graphic canvas on a video track");
            }
            if (const auto problem = graphicProblem(*graphic)) {
                fail(what + ": graphic: " + *problem);
            }
        }
        if (const auto problem = effectStackProblem(
                clip.effects(), track.kind == TrackKind::Video ? EffectKind::Video : EffectKind::Audio, m_state.ids.next)) {
            fail(what + ": " + *problem);
        }
        if (clip.hasUnknownSource()) {
            // A newer build's source kind (C.8): kept as it is, nothing to check it against.
            if (clip.media.isValid() || clip.isNested() || !clip.angles.empty()) {
                fail(what + " has an unknown source besides media or a sequence");
            }
            return;
        }
        if (clip.isNested()) {
            checkNestedClip(clip, what);
            return;
        }
        if (!clip.angles.empty()) {
            fail(what + " has multicam angles but does not play a multicam source");
        }
        const MediaItem* media = m_state.findMedia(clip.media);
        if (!media) {
            fail(what + " refers to missing media " + std::to_string(clip.media.value));
            return;
        }
        if (!media->hasStream(clip.stream)) {
            fail(what + " refers to a stream its media does not have");
            return;
        }
        // Strict: inside MediaItem::sourceBounds() (the media, or a restricted subclip's range).
        if (m_level == Validation::Strict) {
            if (const auto problem = clipBoundsProblem(m_state, clip)) {
                fail(what + " " + *problem);
            }
        }
    }

    /// A clip of a sequence (C.8, Nesting.h). Its extent is not checked even when Strict: the nested sequence may be
    /// shortened after its clips were cut (past its end they show nothing); edits keep trims inside it.
    void checkNestedClip(const Clip& clip, const std::string& what) {
        if (clip.media.isValid()) {
            fail(what + " plays both media and a sequence");
        }
        const Sequence* nested = m_state.findSequence(clip.sequence);
        if (!nested) {
            fail(what + " refers to missing sequence " + std::to_string(clip.sequence.value));
            return;
        }
        if (clip.stream.index != 0 || !sequenceHasStream(*nested, clip.stream)) {
            fail(what + " refers to a stream its sequence does not have");
        }
        if (const auto problem = clipAnglesProblem(m_state, clip)) {
            fail(what + " " + *problem);
        }
    }

    void checkTrack(const TrackPtr& track, TrackKind expected, std::size_t index, bool submix = false) {
        const std::string trackName =
            (submix ? "S" : expected == TrackKind::Video ? "V" : "A") + std::to_string(index + 1);
        if (!track) {
            fail("null track " + trackName);
            return;
        }
        claim(track->id.value, "track " + trackName);
        if (track->kind != expected) {
            fail("track " + trackName + " has the wrong kind");
        }
        if (submix && (!track->clips.empty() || !track->transitions.empty() || track->role != TrackRole::None ||
                       track->recordArmed || !track->sends.empty() || !track->surround.isDefault())) {
            // p02 audio: a submix is a bus: no clips, role, record arm, sends (a send goes from an audio track) or 5.1 panner.
            fail("submix track " + trackName + " holds clips, a role, a record arm, sends or a 5.1 panner");
        }
        if (track->kind == TrackKind::Video && (track->output.isValid() || !track->sends.empty())) {
            fail("track " + trackName + " is a video track with an audio output assignment or sends");
        }
        if (track->kind == TrackKind::Video && track->role != TrackRole::None) {
            fail("track " + trackName + " is a video track with an audio role");
        }
        if (track->kind == TrackKind::Video && track->recordArmed) {
            fail("track " + trackName + " is a video track armed for recording");
        }
        if (track->kind == TrackKind::Video && (track->format != ChannelFormat::Stereo || !track->surround.isDefault())) {
            fail("track " + trackName + " is a video track with an audio channel format or 5.1 panner");
        }
        if (!track->surround.x.isValid(-1.0, 1.0) || !track->surround.y.isValid(-1.0, 1.0) ||
            !track->surround.lfeDb.isValid(-144.0, 0.0)) {
            fail("track " + trackName + "'s 5.1 panner is outside x, y -1..1 or LFE -144..0 dB");
        }
        { // A.3: the track fader and panner
            const ParameterInfo& level = parameterInfo(ParamId::VolumeDb);
            const ParameterInfo& balance = parameterInfo(ParamId::Pan);
            if (!track->volume.isValid(level.minValue, level.maxValue) || !track->pan.isValid(balance.minValue, balance.maxValue)) {
                fail("track " + trackName + " has a volume or pan outside its range, or a malformed curve");
            }
            if (track->kind == TrackKind::Video && !(track->volume == ParamValue() && track->pan == ParamValue())) {
                fail("track " + trackName + " is a video track with an audio fader");
            }
        }
        if (const auto problem = effectStackProblem(
                track->effects, expected == TrackKind::Video ? EffectKind::Video : EffectKind::Audio, m_state.ids.next)) {
            fail("track " + trackName + ": " + *problem);
        }
        const Clip* previous = nullptr;
        for (const Clip& clip : track->clips) {
            checkClip(*track, clip, trackName);
            if (previous && previous->end() > clip.start()) {
                fail("clips " + std::to_string(previous->id.value) + " and " + std::to_string(clip.id.value) +
                     " on " + trackName + " overlap or are out of order");
            }
            previous = &clip;
        }
        std::optional<TimeRange> previousRegion;
        for (const Transition& t : track->transitions) {
            const std::string what = "transition " + std::to_string(t.id.value) + " on " + trackName;
            claim(t.id.value, what);
            if (const auto problem = transitionProblem(m_state, *track, t, m_level)) {
                fail(what + " " + *problem);
                continue;
            }
            const Ticks cut = *transitionEditPoint(*track, t);
            const TimeRange region = TimeRange::fromStartEnd(cut - t.before, cut + t.after);
            if (previousRegion && region.start < previousRegion->end()) {
                fail(what + " overlaps the previous transition or is out of order");
            }
            previousRegion = region;
        }
        m_contentEnd = std::max(m_contentEnd, track->end());
    }

    void checkSequence(const Sequence& seq) {
        const std::string what = "sequence '" + seq.name + "'";
        claim(seq.id.value, what);
        if (isSearchBin(seq.bin)) {
            fail("sequence '" + seq.name + "' is inside a search bin");
        }
        if (!binExists(seq.bin)) {
            fail(what + " is in a missing bin");
        }
        if (!seq.frameRate.isExact()) {
            fail(what + " has an unsupported frame rate");
        }
        if (!seq.audioRate.isExact()) {
            fail(what + " has an unsupported audio rate");
        }
        if (seq.width <= 0 || seq.height <= 0) {
            fail(what + " has an empty canvas");
        }
        if (const auto problem = effectStackProblem(seq.effects, EffectKind::Video, m_state.ids.next)) {
            fail(what + " master effects: " + *problem);
        }
        if (const auto problem = effectStackProblem(seq.audioEffects, EffectKind::Audio, m_state.ids.next)) {
            fail(what + " master audio effects: " + *problem);
        }
        if (seq.startTimecode.value < 0 || seq.startTimecode > kMaxStartTimecode) { // X.1
            fail(what + " has a start timecode outside 00:00:00:00 to 24 hours");
        }
        // P.2 (E5): sequence settings.
        if (!seq.pixelAspect.isValid() || seq.pixelAspect.toDouble() < 0.1 || seq.pixelAspect.toDouble() > 10.0) {
            fail(what + " has a pixel aspect outside 0.1 to 10");
        }
        if (seq.previewFormat.size() > 32) {
            fail(what + " has a preview format name longer than 32 bytes");
        }
        if (seq.colorIndex < 0 || seq.colorIndex > 15) { // 0.2
            fail(what + " has a label colour outside 0 to 15");
        }
        if (seq.masterFormat != ChannelFormat::Stereo && seq.masterFormat != ChannelFormat::Surround51) { // A.9
            fail(what + " has a " + std::string(toString(seq.masterFormat)) + " master (stereo or 5.1)");
        }
        if (const ParameterInfo& level = parameterInfo(ParamId::VolumeDb); !seq.masterVolume.isValid(level.minValue, level.maxValue)) {
            fail(what + " has a master volume outside its range, or a malformed curve"); // A.3
        }
        if (seq.beatGrid) {
            if (const auto problem = edit::beatGridProblem(*seq.beatGrid)) {
                fail(what + " beat grid: " + *problem);
            }
        }
        // A derivation's source may be gone (deleted since): only its size is bounded.
        if (seq.derivedFrom && (seq.derivedFrom->generator.size() > SequenceDerivation::kMaxGeneratorBytes ||
                                seq.derivedFrom->params.size() > SequenceDerivation::kMaxParamsBytes)) {
            fail(what + " derivation record is too large");
        }
        if (seq.importSource && (seq.importSource->project.size() > ImportSource::kMaxPathBytes ||
                                 !edit::isValidUtf8(seq.importSource->project))) {
            fail(what + " has an import source path that is not UTF-8 of at most " +
                 std::to_string(ImportSource::kMaxPathBytes) + " bytes");
        }
        if (seq.multicam) {
            if (const auto problem = multicamProblem(seq)) {
                fail(what + " multicam: " + *problem);
            }
        }
        if (seq.captionTracks.size() > kMaxCaptionTracks) {
            fail(what + " has more than " + std::to_string(kMaxCaptionTracks) + " caption tracks");
        }
        for (const CaptionTrack& captions : seq.captionTracks) {
            const std::string where = what + " caption track " + std::to_string(captions.id.value);
            claim(captions.id.value, where);
            if (const auto problem = edit::captionTrackProblem(captions)) {
                fail(where + " " + *problem);
            }
            for (const CaptionCue& cue : captions.cues) {
                if (cue.speaker.isValid() && !m_state.findSpeaker(cue.speaker)) {
                    fail(where + " has a cue of missing speaker " + std::to_string(cue.speaker.value));
                }
            }
        }
        if (seq.markIn && seq.markIn->value < 0) {
            fail(what + " has an In mark before 0");
        }
        if (seq.markOut && seq.markOut->value <= 0) {
            fail(what + " has an Out mark at or before 0");
        }
        if (seq.markIn && seq.markOut && *seq.markIn >= *seq.markOut) {
            fail(what + " has its In mark at or after its Out mark");
        }
        checkMarkers(seq.markers, what);
        if (seq.workArea && (seq.workArea->start.value < 0 || seq.workArea->isEmpty() ||
                             seq.workArea->end() > seq.duration)) {
            fail(what + " has a work area outside the sequence");
        }
        m_groupCounts.clear();
        m_contentEnd = Ticks{};
        for (std::size_t i = 0; i < seq.videoTracks.size(); ++i) {
            checkTrack(seq.videoTracks[i], TrackKind::Video, i);
        }
        for (std::size_t i = 0; i < seq.audioTracks.size(); ++i) {
            checkTrack(seq.audioTracks[i], TrackKind::Audio, i);
        }
        if (seq.submixTracks.size() > kMaxSubmixTracks) {
            fail(what + " has more than " + std::to_string(kMaxSubmixTracks) + " submix tracks");
        }
        for (std::size_t i = 0; i < seq.submixTracks.size(); ++i) {
            checkTrack(seq.submixTracks[i], TrackKind::Audio, i, true);
        }
        if (const auto problem = audioRoutingProblem(seq)) { // p02 audio: outputs and sends
            fail(what + " audio routing: " + *problem);
        }
        if (seq.duration < m_contentEnd) {
            fail(what + " is shorter than its content");
        }
        for (const auto& [group, use] : m_groupCounts) {
            // A group may shrink to one clip only on a locked track (an edit could not touch it there).
            if (use.count < 2 && !use.onLockedTrack) {
                fail(what + " has link group " + std::to_string(group.value) + " with a single clip on an unlocked track");
            }
            if (!m_groups.insert(group.value).second) {
                fail("link group " + std::to_string(group.value) + " is used by more than one sequence");
            }
        }
    }

    void checkSequences() {
        SequenceId previous{};
        for (const auto& seq : m_state.sequences) {
            if (!seq) {
                fail("null sequence");
                continue;
            }
            if (seq->id <= previous) {
                fail("sequences are not sorted by id");
            }
            previous = seq->id;
            checkSequence(*seq);
        }
        for (const std::uint64_t group : m_groups) {
            if (m_ids.contains(group)) {
                fail("link group " + std::to_string(group) + " reuses an object id");
            }
        }
        if (const auto cycle = nestingCycle(m_state)) {
            fail("sequences contain each other: " + *cycle);
        } else if (const auto fanOut = nestingFanOutProblem(m_state)) {
            fail(*fanOut); // review wave 5 E5-P2-9: a file of doubling nests would expand 2^depth layers
        }
    }

    void checkColorManagement() {
        if (const auto problem = colorManagementProblem(m_state.colorManagement)) {
            fail("colour management: " + std::string(*problem));
        }
    }

    const ProjectState& m_state;
    Validation m_level;
    std::vector<std::string> m_problems;
    std::size_t m_dropped = 0;
    std::unordered_set<std::uint64_t> m_ids;
    std::unordered_set<std::uint64_t> m_groups;
    struct GroupUse {
        int count = 0;
        bool onLockedTrack = false; // of the last member seen; only read when count == 1
    };
    std::unordered_map<LinkGroupId, GroupUse> m_groupCounts;
    Ticks m_contentEnd;
};

} // namespace

std::expected<void, Error> validate(const ProjectState& state, Validation level) {
    return Validator(state, level).run();
}

} // namespace wabcut
