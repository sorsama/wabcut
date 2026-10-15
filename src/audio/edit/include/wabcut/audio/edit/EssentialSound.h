#pragma once

// Essential Sound (A.7, docs/ai.md "Essential Sound"): Premiere's Essential Sound panel as data. A clip gets a type
// (dialogue, music, sound effects, ambience) and a preset of the type builds an ordinary effect stack for it, nothing
// hidden: the clip's stack starts with the wabcut.audio.essentialSound record (its data: "type", "preset" and the
// "options" applied, as JSON) and then the effects the options ask for, each marked with its panel section (data
// "essentialSound": "loudness", "repair.noise", ...). Every one is a normal effect the user can edit, keyframe,
// disable or delete; applying a preset again replaces the marked effects and keeps the clip's other effects.
//
//   Dialogue  loudness (matched to a target), Repair (Reduce Noise: RNNoise, or Enhance Speech; rumble high-pass;
//             DeHum 50/60 Hz and harmonics; Noise Gate; DeEsser; Reduce Reverb: an expander), Clarity (dynamics: a
//             compressor, with makeup on request; EQ presets "podcastVoice", "phone", "radio", "streamerVoice"; a
//             true-peak limiter last), Creative (reverb presets)
//   Music     loudness, ducking under dialogue (volume keyframes, audio.autoDuck's envelope), duration (Remix),
//             stereo width
//   SFX       loudness, position (the clip's panner)
//   Ambience  loudness, ducking, stereo width
//
// The level-dependent effects (the gate, Reduce Reverb, the de-esser, the compressor) come after the loudness Gain, so
// they hear the clip at about its loudness target: their thresholds are set for dialogue at -23 LUFS (Premiere's
// Auto-Match) and move with the target (A.10: the "streamerVoice" preset's -16 LUFS raises them 7 dB). Every preset at
// -23 LUFS builds what it built before A.10.
//
// Pure: the presets, the options and the stack an option set builds. The command (audio.essentialSound) measures the
// loudness, ducks and remixes through the host's analysis.

#include <wabcut/audio/edit/Ducking.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/Track.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::audio::edit {

enum class SoundType : std::uint8_t { Dialogue, Music, Sfx, Ambience };

/// "dialogue", "music", "sfx", "ambience".
std::string_view toString(SoundType type);
std::optional<SoundType> soundTypeFromString(std::string_view text);
/// The type a track role stands for (effects: sfx); none for TrackRole::None.
std::optional<SoundType> soundTypeOf(TrackRole role);
/// The track role of a type (sfx: effects); None for ambience, which has no role.
TrackRole trackRoleOf(SoundType type);

inline constexpr std::string_view kEssentialSoundEffect = "wabcut.audio.essentialSound";
/// Data key of the effects a preset built: their panel section.
inline constexpr std::string_view kSectionKey = "essentialSound";

/// The panel's settings. A preset is a full set; a request's options change some of them.
struct EssentialSoundOptions {
    // Loudness (Premiere's Auto-Match): the clip on its own matched to targetLufs by a Gain effect.
    bool loudness = true;
    double targetLufs = -23.0;
    // Repair (dialogue).
    double reduceNoise = 0.0;  ///< 0 = off .. 1: Reduce Noise (RNNoise) amount
    bool enhanceSpeech = false; ///< Enhance Speech (DeepFilterNet3, a rendered take) instead of or with Reduce Noise
    double rumbleHz = 0.0;      ///< 0 = off, else the high-pass at this frequency (24 dB per octave)
    double dehumHz = 0.0;       ///< 0 = off, 50 or 60 (40..70)
    double gate = 0.0;          ///< 0 = off .. 1: Noise Gate (A.10), an expander with a high ratio after Reduce Noise
    double deess = 0.0;         ///< 0 = off .. 1
    double reduceReverb = 0.0;  ///< 0 = off .. 1: the expander
    // Clarity (dialogue).
    double dynamics = 0.0;      ///< 0 = off .. 1: the compressor
    bool dynamicsMakeup = false; ///< A.10: the compressor gives back what it takes from a voice at the loudness target
    std::string eq;             ///< "" (none), "podcastVoice", "phone", "radio", "streamerVoice"
    double eqAmount = 1.0;      ///< 0..1: scales the EQ preset's boosts and cuts
    bool limiter = false;       ///< A.10: a true-peak limiter, the last effect of the stack
    double limiterCeiling = -1.0; ///< its ceiling, dBTP (-24..0)
    // Creative (dialogue; any type may use it).
    std::string reverb;         ///< "" (none), "smallRoom", "largeRoom", "hall", "plate", "outside"
    double reverbAmount = -1.0; ///< the reverb's mix 0..1; < 0: the reverb preset's
    // Music and ambience.
    bool duck = false;
    DuckingOptions ducking;
    std::optional<double> durationSeconds; ///< music: remix the clip to this length
    double width = 1.0;         ///< 1 = off; 0..2
    // Sound effects.
    std::optional<double> pan;  ///< -1..1: the clip's panner

    bool operator==(const EssentialSoundOptions&) const = default;
};

struct EssentialSoundPreset {
    std::string_view id;
    std::string_view name;
    SoundType type;
    EssentialSoundOptions options;
};

/// The presets of a type, the "default" one first.
std::span<const EssentialSoundPreset> essentialSoundPresets(SoundType type);
const EssentialSoundPreset* findEssentialSoundPreset(SoundType type, std::string_view id);
/// The EQ presets ("podcastVoice", "phone", "radio", "streamerVoice") and reverb presets ("smallRoom", ...), for schemas
/// and checks.
std::span<const std::string_view> clarityEqPresets();
std::span<const std::string_view> reverbPresets();

/// One effect of a built stack: the instance (its id still to be given) and its panel section.
struct BuiltEffect {
    std::string effect;
    std::vector<EffectParam> params; ///< sorted by key
    std::string section;             ///< kSectionKey's value ("loudness", "repair.noise", ...)
};

/// The effects `options` build for `type`, in stack order, after the wabcut.audio.essentialSound record (not
/// included). The loudness Gain comes first (0 dB until the command measures; audio.normalize finds it too), the
/// limiter (when on) last.
std::vector<BuiltEffect> buildEssentialSoundStack(SoundType type, const EssentialSoundOptions& options);

/// The record's data: "type", "preset", "options" (`optionsJson`, at most kMaxEffectText bytes).
std::vector<EffectData> essentialSoundRecord(SoundType type, std::string_view preset, std::string optionsJson);

/// Whether an instance belongs to Essential Sound: the record, or an effect marked with a section.
bool isEssentialSoundEffect(const EffectInstance& instance);
/// The type recorded on a clip's stack, if any.
std::optional<SoundType> recordedSoundType(const EffectStack& stack);

} // namespace wabcut::audio::edit
