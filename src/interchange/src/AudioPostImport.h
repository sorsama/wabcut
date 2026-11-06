#pragma once

// AAF and OMF import (0.6, card #68): the format-neutral mob graph both readers translate their files into, and the
// one engine that turns it into the importers' IrDocument (Ir.h).
//
// AAF and OMF 2.0 share one model: a composition mob's slots (tracks) hold Sequences of SourceClips, Fillers,
// Transitions and OperationGroups / Effects, and a SourceClip points through a master mob to a file source mob that
// describes (and may embed) a media file. AafImporter.cpp and OmfImporter.cpp only read their container and fill a
// Graph; buildDocument() does the rest the same way for both:
//
//  * every top-level composition (or, without one, every composition no other refers to) becomes a sequence, with
//    its timecode slot's start timecode, frame rate and the audio slots' sample rate;
//  * SourceClips resolve through master mobs to the file they play, at the right source offset and channel (a
//    multichannel file's slot / physical track number); compositions they point to (Media Composer subclips and
//    adjusted clips, nested sequences) are flattened into the track;
//  * transitions become crossfades or fades at the edit point (Filler on one side: a fade in or out), with the curve
//    the file names (linear or equal power);
//  * gain and pan OperationGroups (Effects in OMF) around a clip become the clip's volume and pan (constants or
//    keyframes); around a whole track's Sequence they become the track's fader and panner; other effects are reported
//    and their input clip is kept;
//  * a channel combiner (Pro Tools / Resolve multichannel tracks) plays one multichannel file as one clip, or several
//    mono files as linked mono tracks: stereo panned left and right, 5.1 routed to a 5.1 master's channels;
//  * six mono tracks named <name> L / R / C / LFE / Ls / Rs (Wabcut's own breakout) are routed to a 5.1 master again;
//  * embedded audio (AAF EssenceData, OMF media data) is written to files in the import's media folder, and media that
//    is not found stays offline with its clips in place (reported, never dropped);
//  * markers (AAF event slots) with their colour.
// Everything is bounded (nesting, clip counts, automation points); malformed graphs give report entries or a
// ParseError, never a crash.

#include "Importers.h"
#include "Ir.h"

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wabcut::interchange::detail::graph {

enum class DataKind : std::uint8_t { Other, Sound, Picture, Timecode, Descriptive };

/// A parameter over a component: a constant, or points at fractions of its length (linear between them), in the
/// file's units (an amplitude multiplier, a pan 0 = left .. 1 = right).
struct Control {
    double constant = 1.0;
    std::vector<std::pair<double, double>> points; ///< (fraction 0..1, value)
};

enum class FadeCurve : std::uint8_t { Unknown, Linear, Power };

struct Component {
    enum class Kind : std::uint8_t {
        Sequence,
        SourceClip,
        Filler,
        Transition,
        Gain,     ///< clip or track volume: children[0] is the input; controls multiply
        Pan,      ///< clip or track pan: children[0] is the input; controls[0] is the pan
        Combiner, ///< one input per channel (Avid's Audio Channel Combiner)
        Effect,   ///< any other effect: children[0] (if any) is the input it changes
        Selector, ///< children[0] is the selected segment, the alternates follow
        Timecode,
        Marker,
        Other,
    };
    Kind kind = Kind::Other;
    DataKind data = DataKind::Other;
    std::int64_t length = 0; ///< in edit units of the slot
    // SourceClip
    std::string sourceId; ///< the mob it plays (Graph key); empty: none (silence)
    std::uint32_t sourceSlot = 0;
    std::int64_t startTime = 0;
    std::int64_t fadeIn = 0;  ///< OMF / old AAF fade properties of the clip, edit units
    std::int64_t fadeOut = 0;
    FadeCurve fadeInCurve = FadeCurve::Unknown;
    FadeCurve fadeOutCurve = FadeCurve::Unknown;
    // Transition
    std::int64_t cutPoint = 0;
    FadeCurve curve = FadeCurve::Unknown;
    // Gain / Pan
    std::vector<Control> controls;
    // Effect / Other / Selector: what it is, for the report
    std::string name;
    bool timeWarp = false;
    std::size_t alternates = 0; ///< Selector: segments not selected
    // Timecode
    std::int64_t tcStart = 0;
    std::int32_t fps = 0;
    bool drop = false;
    // Marker
    std::int64_t position = 0;
    std::string comment;
    std::optional<std::array<int, 3>> color; ///< RGB 0..255
    std::vector<Component> children;
    /// Element path for report locations ("CompositionMob 'Mix'/Slot 3/Sequence/component 4").
    std::string where;
};

struct Slot {
    std::uint32_t id = 0;
    Rate editRate{0, 1}; ///< invalid: the slot is skipped
    std::int64_t origin = 0;
    std::uint32_t physical = 0;
    std::string name;
    bool event = false; ///< an event slot (markers)
    Component segment;
};

/// Embedded media a file mob carries (AAF EssenceData, OMF media data).
struct Essence {
    std::string extension; ///< ".wav", ".aif"
    std::uint64_t size = 0;
    /// Written before the data: a WAV header for raw PCM essence; empty when the data is a whole file.
    std::string header;
    /// Hands the data to the sink in blocks; the sink returns false to stop.
    std::function<std::expected<void, Error>(const std::function<bool(std::string_view)>&)> copy;
};

struct Mob {
    enum class Kind : std::uint8_t { Composition, Master, File, Physical, Other };
    Kind kind = Kind::Other;
    std::string key;
    std::string name;
    bool topLevel = false;   ///< usage code TopLevel / a primary mob
    bool lowerLevel = false; ///< a usage code saying it is not a top-level sequence (subclip, adjusted clip, ...)
    std::vector<Slot> slots;
    // File (and physical) source mobs: what the descriptor says.
    bool audio = false;
    bool video = false;
    std::int32_t channels = 0;
    Rate sampleRate{0, 1};
    std::optional<std::int64_t> length; ///< in sampleRate units
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<std::string> locators; ///< URLs or paths as written, best first
    std::optional<Essence> essence;
    /// Why embedded media cannot be used (embedded video, a codec): reported with the media.
    std::string essenceProblem;
    std::string where;
};

struct Graph {
    std::vector<Mob> mobs;
    std::unordered_map<std::string, std::size_t> byKey;
    /// Entries the reader produced.
    ImportReport report;

    void add(Mob mob) {
        if (byKey.contains(mob.key)) {
            return;
        }
        byKey.emplace(mob.key, mobs.size());
        mobs.push_back(std::move(mob));
    }
    const Mob* find(const std::string& key) const {
        const auto it = byKey.find(key);
        return it == byKey.end() ? nullptr : &mobs[it->second];
    }
};

struct BuildOptions {
    Format format = Format::Aaf;
    /// OMF: video tracks and markers are not imported (card #68: reported as dropped).
    bool video = true;
};

/// Builds the importers' document from `graph`. Errors: ParseError (no composition to import).
std::expected<IrDocument, Error> buildDocument(Graph graph, const ParseContext& context, const BuildOptions& options);

// --- Helpers the readers share --------------------------------------------------------------------------------

/// What a WAV (RIFF / RF64) or AIFF / AIFF-C header says.
struct AudioHeader {
    std::int32_t channels = 0;
    std::int32_t sampleRate = 0;
    std::int32_t bits = 0;
    bool aiff = false;
    std::optional<std::uint64_t> frames;
};
std::optional<AudioHeader> parseAudioHeader(std::string_view bytes);

/// A canonical WAV header for `dataBytes` of little-endian PCM (raw PCM essence written as a .wav file).
std::string wavHeader(std::int32_t channels, std::int32_t sampleRate, std::int32_t bits, std::uint64_t dataBytes);

/// Text from a file that may not be UTF-8 (OMF strings: Mac Roman or Windows Latin 1): valid UTF-8 is kept, other
/// bytes are read as Latin 1. Control characters are dropped.
std::string textToUtf8(std::string_view text);

/// The speaker a track or source name ends with ("Dialog.L", "Mix Ls", "Audio 1 Right", "FX_LFE"): 0..5 in Wabcut's
/// 5.1 order L R C LFE Ls Rs, or (0.7) 6 and 7 for the rear surrounds of 7.1 ("Mix Lrs", "Mix Rrs", Pro Tools' Lsr / Rsr), or
/// -1. In a 7.1 layout Ls / Rs (4, 5) are the side pair.
int speakerOfName(std::string_view name);
/// The channel of a 7.1 mix (L R C LFE Lrs Rrs Ls Rs) a speaker of speakerOfName() plays from: the rear surrounds 6 and 7 are
/// channels 4 and 5, Ls and Rs (4, 5) channels 6 and 7. -1 for a speaker outside 0..7.
int channelOf71Speaker(int speaker);
/// The name without that suffix ("Audio 1 Right" -> "Audio 1").
std::string withoutSpeaker(std::string_view name);

} // namespace wabcut::interchange::detail::graph
