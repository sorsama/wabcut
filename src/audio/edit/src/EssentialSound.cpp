#include <wabcut/audio/edit/EssentialSound.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace wabcut::audio::edit {

std::string_view toString(SoundType type) {
    switch (type) {
    case SoundType::Music:
        return "music";
    case SoundType::Sfx:
        return "sfx";
    case SoundType::Ambience:
        return "ambience";
    case SoundType::Dialogue:
        break;
    }
    return "dialogue";
}

std::optional<SoundType> soundTypeFromString(std::string_view text) {
    for (const SoundType t : {SoundType::Dialogue, SoundType::Music, SoundType::Sfx, SoundType::Ambience}) {
        if (text == toString(t)) {
            return t;
        }
    }
    return std::nullopt;
}

std::optional<SoundType> soundTypeOf(TrackRole role) {
    switch (role) {
    case TrackRole::Dialogue:
        return SoundType::Dialogue;
    case TrackRole::Music:
        return SoundType::Music;
    case TrackRole::Effects:
        return SoundType::Sfx;
    case TrackRole::None:
        break;
    }
    return std::nullopt;
}

TrackRole trackRoleOf(SoundType type) {
    switch (type) {
    case SoundType::Dialogue:
        return TrackRole::Dialogue;
    case SoundType::Music:
        return TrackRole::Music;
    case SoundType::Sfx:
        return TrackRole::Effects;
    case SoundType::Ambience:
        break;
    }
    return TrackRole::None;
}

namespace {

// ---- Presets -----------------------------------------------------------------------------------------------------

/// Loudness targets: Premiere's Auto-Match matches dialogue to -23 LUFS; music and sound effects sit 2 LU under it and
/// ambience 7 LU under it. The mix is brought to its delivery target afterwards (audio.normalize).
constexpr double kDialogueLufs = -23.0;
constexpr double kMusicLufs = -25.0;
constexpr double kSfxLufs = -25.0;
constexpr double kAmbienceLufs = -30.0;
/// A.10, Streamer voice: a gaming commentary mic on its own at -16 LUFS. The upload is normalised to YouTube's -14 LUFS
/// integrated (audio.normalize "youtube"): the voice is the loudest part of the mix, and at -16 it leaves about 2 LU for
/// the game under and between the sentences, so the final normalisation moves the mix by a dB or two instead of the
/// 9 dB a voice at Auto-Match's -23 would need (and the voice keeps its place over the game).
constexpr double kStreamerVoiceLufs = -16.0;
/// A voice's peak envelope (what the peak detectors of the compressor and the expanders follow) over its loudness, about:
/// Dynamics at 0 sets its threshold there, and the makeup is reckoned from it.
constexpr double kVoicePeakOverLoudnessDb = 9.0;

EssentialSoundOptions base(double lufs) {
    EssentialSoundOptions o;
    o.targetLufs = lufs;
    return o;
}

std::vector<EssentialSoundPreset> makeDialoguePresets() {
    std::vector<EssentialSoundPreset> out;
    out.push_back({"default", "Default (match loudness)", SoundType::Dialogue, base(kDialogueLufs)});
    {
        // A.10: the headset or desk mic of a gaming recording (OBS, ShadowPlay), for an upload. Rumble cut and RNNoise
        // for the desk and the fan, the gate for the keyboard and the breaths between sentences, the de-esser before
        // the EQ's presence lift, a compressor with makeup for a steady voice over the game, a true-peak limiter last.
        // Enhance Speech (DeepFilterNet3) is the heavier denoise: "repair": {"enhance": true, "noise": 0} uses it
        // instead of RNNoise, but it plays a take audio.enhanceSpeech renders with the model, so the preset keeps
        // RNNoise and works on any computer, offline, at once.
        EssentialSoundOptions o = base(kStreamerVoiceLufs);
        o.reduceNoise = 0.8;
        o.rumbleHz = 80.0;
        o.gate = 0.5;
        o.deess = 0.45;
        o.eq = "streamerVoice";
        o.dynamics = 0.6;
        o.dynamicsMakeup = true;
        o.limiter = true;
        o.limiterCeiling = -1.0;
        out.push_back({"streamerVoice", "Streamer voice (mic: denoise, gate, EQ, compressor, de-esser, limiter)",
                       SoundType::Dialogue, o});
    }
    {
        EssentialSoundOptions o = base(kDialogueLufs);
        o.reduceNoise = 0.7;
        o.rumbleHz = 80.0;
        o.deess = 0.3;
        out.push_back({"cleanUpNoisy", "Clean up noisy dialogue", SoundType::Dialogue, o});
    }
    {
        EssentialSoundOptions o = base(kDialogueLufs);
        o.reduceNoise = 0.3;
        o.rumbleHz = 80.0;
        o.deess = 0.4;
        o.dynamics = 0.5;
        o.eq = "podcastVoice";
        out.push_back({"podcastVoice", "Podcast voice", SoundType::Dialogue, o});
    }
    {
        EssentialSoundOptions o = base(kDialogueLufs);
        o.dynamics = 0.6;
        o.eq = "phone";
        out.push_back({"phone", "Telephone call", SoundType::Dialogue, o});
    }
    {
        EssentialSoundOptions o = base(kDialogueLufs);
        o.deess = 0.3;
        o.dynamics = 0.7;
        o.eq = "radio";
        out.push_back({"radio", "From the radio", SoundType::Dialogue, o});
    }
    {
        EssentialSoundOptions o = base(kDialogueLufs);
        o.rumbleHz = 80.0;
        o.reduceReverb = 0.6;
        out.push_back({"reduceRoomEcho", "Reduce room echo", SoundType::Dialogue, o});
    }
    {
        EssentialSoundOptions o = base(kDialogueLufs);
        o.dehumHz = 50.0;
        out.push_back({"removeHum", "Remove mains hum (50 Hz; options.repair.hum 60 for the Americas)", SoundType::Dialogue, o});
    }
    {
        EssentialSoundOptions o = base(kDialogueLufs);
        o.reverb = "smallRoom";
        out.push_back({"smallRoom", "In a small room", SoundType::Dialogue, o});
    }
    {
        EssentialSoundOptions o = base(kDialogueLufs);
        o.reverb = "largeRoom";
        out.push_back({"largeRoom", "In a large reverberant room", SoundType::Dialogue, o});
    }
    {
        EssentialSoundOptions o = base(kDialogueLufs);
        o.reverb = "outside";
        out.push_back({"outside", "Outside", SoundType::Dialogue, o});
    }
    return out;
}

std::vector<EssentialSoundPreset> makeMusicPresets() {
    std::vector<EssentialSoundPreset> out;
    out.push_back({"default", "Default (match loudness)", SoundType::Music, base(kMusicLufs)});
    {
        EssentialSoundOptions o = base(kMusicLufs);
        o.duck = true;
        out.push_back({"background", "Background music (ducked under dialogue)", SoundType::Music, o});
    }
    {
        EssentialSoundOptions o = base(kMusicLufs);
        o.width = 1.3;
        out.push_back({"wide", "Wider stereo", SoundType::Music, o});
    }
    return out;
}

std::vector<EssentialSoundPreset> makeSfxPresets() {
    std::vector<EssentialSoundPreset> out;
    out.push_back({"default", "Default (match loudness)", SoundType::Sfx, base(kSfxLufs)});
    out.push_back({"background", "Background effect", SoundType::Sfx, base(kSfxLufs - 7.0)});
    return out;
}

std::vector<EssentialSoundPreset> makeAmbiencePresets() {
    std::vector<EssentialSoundPreset> out;
    out.push_back({"default", "Default (match loudness)", SoundType::Ambience, base(kAmbienceLufs)});
    {
        EssentialSoundOptions o = base(kAmbienceLufs);
        o.duck = true;
        o.ducking.amountDb = -8.0;
        out.push_back({"underDialogue", "Under dialogue (ducked)", SoundType::Ambience, o});
    }
    {
        EssentialSoundOptions o = base(kAmbienceLufs);
        o.width = 1.4;
        out.push_back({"wide", "Wide room tone", SoundType::Ambience, o});
    }
    return out;
}

const std::vector<EssentialSoundPreset>& presetsOf(SoundType type) {
    static const std::array<std::vector<EssentialSoundPreset>, 4> kPresets{makeDialoguePresets(), makeMusicPresets(),
                                                                           makeSfxPresets(), makeAmbiencePresets()};
    return kPresets[static_cast<std::size_t>(type)];
}

constexpr std::array<std::string_view, 4> kEqPresets{"podcastVoice", "phone", "radio", "streamerVoice"};
constexpr std::array<std::string_view, 5> kReverbPresets{"smallRoom", "largeRoom", "hall", "plate", "outside"};

struct ReverbPreset {
    std::string_view id;
    double size, predelay, decay, damping, mix, width;
};
constexpr std::array<ReverbPreset, 5> kReverbs{{
    {"smallRoom", 0.2, 4.0, 500.0, 0.6, 0.18, 0.8},
    {"largeRoom", 0.55, 15.0, 1300.0, 0.5, 0.22, 1.0},
    {"hall", 0.9, 25.0, 2600.0, 0.4, 0.25, 1.0},
    {"plate", 0.4, 6.0, 1800.0, 0.15, 0.22, 1.0},
    {"outside", 0.1, 0.0, 250.0, 0.8, 0.1, 0.6},
}};

// ---- Building --------------------------------------------------------------------------------------------------

std::vector<EffectParam> sorted(std::vector<EffectParam> params) {
    std::sort(params.begin(), params.end(), [](const EffectParam& a, const EffectParam& b) { return a.key < b.key; });
    return params;
}

EffectParam p(std::string key, double value) { return EffectParam{std::move(key), ParamValue(value)}; }

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

} // namespace

std::span<const EssentialSoundPreset> essentialSoundPresets(SoundType type) { return presetsOf(type); }

const EssentialSoundPreset* findEssentialSoundPreset(SoundType type, std::string_view id) {
    for (const EssentialSoundPreset& preset : presetsOf(type)) {
        if (preset.id == id) {
            return &preset;
        }
    }
    return nullptr;
}

std::span<const std::string_view> clarityEqPresets() { return kEqPresets; }
std::span<const std::string_view> reverbPresets() { return kReverbPresets; }

std::vector<BuiltEffect> buildEssentialSoundStack(SoundType type, const EssentialSoundOptions& o) {
    (void)type; // the options say everything; the type chose the preset
    std::vector<BuiltEffect> out;
    const auto add = [&](std::string effect, std::vector<EffectParam> params, std::string section) {
        out.push_back(BuiltEffect{std::move(effect), sorted(std::move(params)), std::move(section)});
    };
    // The level-dependent effects below come after the loudness Gain, so they hear the clip at about its target. Their
    // thresholds are set for dialogue at -23 LUFS (Premiere's Auto-Match) and move with the target (A.10): a streamer
    // voice at -16 LUFS is gated, de-essed and compressed as a voice at -23 would be. Without loudness the clip's level
    // is not known; they stay as set for -23.
    const double shift = o.loudness ? o.targetLufs - kDialogueLufs : 0.0;
    if (o.loudness) {
        add("wabcut.audio.gain", {p("gain", 0.0)}, "loudness");
    }
    // Repair: the source first (Enhance Speech replaces it), then rumble and hum before the noise reduction, which
    // handles broadband noise best once they are gone; the expander after it (the noise floor it gates is lower).
    if (o.enhanceSpeech) {
        add("wabcut.audio.enhanceSpeech", {p("strength", 1.0)}, "repair.enhance");
    }
    if (o.rumbleHz > 0.0) {
        add("wabcut.audio.filter", {p("lowCut", std::clamp(o.rumbleHz, 20.0, 2000.0)), p("slope", 24.0)}, "repair.rumble");
    }
    if (o.dehumHz > 0.0) {
        add("wabcut.audio.dehum", {p("frequency", std::clamp(o.dehumHz, 40.0, 70.0)), p("harmonics", 4.0), p("reduction", 30.0)},
            "repair.hum");
    }
    if (o.reduceNoise > 0.0) {
        add("wabcut.audio.denoise", {p("amount", clamp01(o.reduceNoise))}, "repair.noise");
    }
    // Noise Gate (A.10): an expander with a high ratio and a 1 ms opening, after the noise reduction (the floor it closes
    // on is lower there) and before Reduce Reverb. At 0.5 it closes 8 dB under the voice's loudness on the expander's
    // peak detector, some 17 dB under the voice's peaks (OBS's noise gate closes about 20 dB under a typical voice) and
    // well over a fan's floor, by 26 dB. Higher amounts close higher, deeper and sooner.
    if (o.gate > 0.0) {
        const double g = clamp01(o.gate);
        add("wabcut.audio.expander",
            {p("threshold", std::clamp(kDialogueLufs - 12.0 + 8.0 * g + shift, -80.0, 0.0)), p("ratio", 2.0 + 8.0 * g),
             p("range", 12.0 + 28.0 * g), p("attack", 1.0), p("release", 200.0 - 80.0 * g)},
            "repair.gate");
    }
    if (o.reduceReverb > 0.0) {
        const double r = clamp01(o.reduceReverb);
        add("wabcut.audio.expander",
            {p("threshold", std::clamp(-50.0 + 15.0 * r + shift, -80.0, 0.0)), p("ratio", 1.5 + 2.5 * r),
             p("range", 6.0 + 18.0 * r), p("attack", 2.0), p("release", 100.0 + 100.0 * r)},
            "repair.reverb");
    }
    if (o.deess > 0.0) {
        const double a = clamp01(o.deess);
        add("wabcut.audio.deess",
            {p("threshold", std::clamp(-18.0 - 22.0 * a + shift, -60.0, 0.0)), p("reduction", 4.0 + 12.0 * a),
             p("frequency", 5500.0)},
            "repair.deess");
    }
    // Clarity: the EQ preset (its band filter, then its tone), then the dynamics.
    const double eqAmount = clamp01(o.eqAmount);
    if (!o.eq.empty() && eqAmount > 0.0) {
        const auto g = [&](double db) { return db * eqAmount; };
        if (o.eq == "podcastVoice") {
            add("wabcut.audio.filter", {p("lowCut", 80.0), p("slope", 24.0)}, "clarity.band");
            add("wabcut.audio.eq3",
                {p("lowFrequency", 150.0), p("lowGain", g(-2.0)), p("midFrequency", 3000.0), p("midGain", g(3.0)), p("midQ", 0.9),
                 p("highFrequency", 10000.0), p("highGain", g(2.0))},
                "clarity.eq");
        } else if (o.eq == "phone") {
            add("wabcut.audio.filter", {p("lowCut", 300.0), p("highCut", 3400.0), p("slope", 24.0)}, "clarity.band");
            add("wabcut.audio.eq3", {p("midFrequency", 1500.0), p("midGain", g(5.0)), p("midQ", 1.2)}, "clarity.eq");
        } else if (o.eq == "radio") {
            add("wabcut.audio.filter", {p("lowCut", 120.0), p("highCut", 7000.0), p("slope", 24.0)}, "clarity.band");
            add("wabcut.audio.eq3",
                {p("lowFrequency", 200.0), p("lowGain", g(2.0)), p("midFrequency", 2500.0), p("midGain", g(3.0)), p("midQ", 1.0),
                 p("highFrequency", 5000.0), p("highGain", g(1.0))},
                "clarity.eq");
        } else if (o.eq == "streamerVoice") {
            // A.10: a close headset or desk mic over game audio. The low cut a little above a podcast's (desk knocks, a
            // close mic's proximity boom), a little mud out, presence where speech cuts through the game (3.2 kHz), a
            // little air. eq3 has a low shelf, not a fourth band: the mud cut is a -2.5 dB shelf under 300 Hz (half of
            // it at 300 Hz), and the low cut takes what is under 90 Hz.
            add("wabcut.audio.filter", {p("lowCut", 90.0), p("slope", 24.0)}, "clarity.band");
            add("wabcut.audio.eq3",
                {p("lowFrequency", 300.0), p("lowGain", g(-2.5)), p("midFrequency", 3200.0), p("midGain", g(3.5)), p("midQ", 1.0),
                 p("highFrequency", 10000.0), p("highGain", g(2.0))},
                "clarity.eq");
        }
    }
    if (o.dynamics > 0.0) {
        const double d = clamp01(o.dynamics);
        const double threshold = -14.0 - 16.0 * d + shift;
        const double ratio = 1.5 + 4.5 * d;
        // Makeup (A.10, "streamerVoice"): what the compressor takes from a voice at the target (its peak envelope about
        // kVoicePeakOverLoudnessDb over its loudness) is given back. Without it the loudness Gain has to push the voice
        // far into the compressor to reach the target (each dB of Gain then adds 1/ratio dB), and the noise floor the
        // gate and the de-esser hear goes up with it.
        const double voicePeak = kDialogueLufs + shift + kVoicePeakOverLoudnessDb;
        const double makeup =
            o.dynamicsMakeup ? std::clamp((voicePeak - threshold) * (1.0 - 1.0 / ratio), 0.0, 24.0) : 0.0;
        add("wabcut.audio.compressor",
            {p("threshold", std::clamp(threshold, -60.0, 0.0)), p("ratio", ratio), p("attack", 5.0), p("release", 120.0),
             p("makeup", makeup)},
            "clarity.dynamics");
    }
    // Space.
    if (std::fabs(o.width - 1.0) > 1e-9) {
        add("wabcut.audio.width", {p("width", std::clamp(o.width, 0.0, 2.0))}, "width");
    }
    if (!o.reverb.empty()) {
        for (const ReverbPreset& r : kReverbs) {
            if (r.id == o.reverb) {
                const double mix = o.reverbAmount >= 0.0 ? clamp01(o.reverbAmount) : r.mix;
                add("wabcut.audio.reverb",
                    {p("size", r.size), p("predelay", r.predelay), p("decay", r.decay), p("damping", r.damping), p("mix", mix),
                     p("width", r.width)},
                    "creative.reverb");
            }
        }
    }
    // Limiter (A.10): last, so nothing after it in the clip's chain raises a peak again (the clip's Volume comes before
    // its effects). A true-peak ceiling: the streaming maximum is -1 dBTP.
    if (o.limiter) {
        add("wabcut.audio.limiter",
            {p("ceiling", std::clamp(o.limiterCeiling, -24.0, 0.0)), p("release", 50.0), p("lookahead", 5.0)},
            "clarity.limiter");
    }
    return out;
}

std::vector<EffectData> essentialSoundRecord(SoundType type, std::string_view preset, std::string optionsJson) {
    std::vector<EffectData> data;
    if (optionsJson.size() > kMaxEffectText) {
        optionsJson.clear(); // too long to keep: the preset and type still say what was applied
    }
    if (!optionsJson.empty()) {
        data.push_back(EffectData{"options", std::move(optionsJson)});
    }
    if (!preset.empty()) {
        data.push_back(EffectData{"preset", std::string(preset)});
    }
    data.push_back(EffectData{"type", std::string(toString(type))});
    std::sort(data.begin(), data.end(), [](const EffectData& a, const EffectData& b) { return a.key < b.key; });
    return data;
}

bool isEssentialSoundEffect(const EffectInstance& instance) {
    return instance.effect == kEssentialSoundEffect || instance.findData(kSectionKey) != nullptr;
}

std::optional<SoundType> recordedSoundType(const EffectStack& stack) {
    for (const EffectInstance& e : stack) {
        if (e.effect == kEssentialSoundEffect) {
            if (const EffectData* d = e.findData("type"); d != nullptr && d->text() != nullptr) {
                return soundTypeFromString(*d->text());
            }
        }
    }
    return std::nullopt;
}

} // namespace wabcut::audio::edit
