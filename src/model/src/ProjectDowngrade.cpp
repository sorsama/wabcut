// Saving a project for an older Wabcut (0.7, card 059). See ProjectDowngrade.h.

#include <wabcut/model/ProjectDowngrade.h>

#include <wabcut/model/ProjectIO.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <set>
#include <unordered_map>

namespace wabcut::io {

namespace {

using json = nlohmann::json;

/// The first release a field belongs to: 2 = Wabcut 0.6 (schema 19), 3 = Wabcut 0.7. A release run lacks every field
/// whose epoch is above its own (0.2 to 0.5: 1, 0.6: 2).
struct Feature {
    std::string_view id;
    std::string_view what;
    std::string_view effect;
    int since;
};

enum class Action : std::uint8_t {
    Remove,          ///< the key goes
    AnimationToNone, ///< a caption animation the older release has no name for becomes "none"
    DropSoundAssets, ///< the project assets of kind "sound" go
    SourceMarkerToComment, ///< a marker of type "source" becomes a comment marker (the type key goes)
    SurroundToFiveOne,     ///< a "7.1" master becomes "5.1" (0.6 has no 7.1)
    SurroundToAdaptive     ///< a "7.1" track or channel map becomes "adaptive": the same eight channels, passed through one to one
};

struct Rule {
    int feature; ///< index into kFeatures
    std::string_view parents; ///< '|'-separated keys the object may sit under ("" = anywhere)
    std::string_view key;
    Action action = Action::Remove;
    /// What the key holds when the person never used the feature (a caption style always writes its emphasis and case rules):
    /// the key is still removed, but only another value counts as something lost. "~" = always counts, "" = never counts
    /// (the key only accompanies another one), else a string, "true"/"false" or "[]".
    std::string_view defaultText = "~";
};

enum FeatureIndex : int {
    kAiInterpolation,
    kMulticamNudges,
    kDolbyVision,
    kChannelNames,
    kMediaRights,
    kScaleWidth,
    kTypography,
    kVr360,
    kSpectralRepair,
    kSurround71,
    kMirrored,
    kBleepSounds,
    kNearest,
    kTrackColors,
    kCaptionTiming,
    kCaptionText,
    kCaptionLooks,
    kSequenceDates,
    kNestBin,
    kLutFolders,
    kTemplateFolders,
    kCustomWords,
    kFieldKeys,
    kEqMatch,
    kBilingual,
    kCitations,
    kTrackGroups,
    kKeyframePins,
    kRippleTrackKeys,
    kClipNotes,
    kRenderedNests,
    kClipLock,
    kNewEffects,
    kAuditions,
    kMasterEffects,
    kColorOutput,
    kTextStyling,
    kTextLayout,
    kShadowSize,
};

// In the order the loss list shows them: 0.7's additions first (the newest), then 0.6's.
constexpr std::array<Feature, 39> kFeatures{{
    {"aiInterpolation", "AI Frame Interpolation of retimed clips", "the clips play with Optical Flow", 3},
    {"multicamNudges", "Multicam angle sync nudges", "the angles keep their offsets, without the nudge marks", 3},
    {"dolbyVision", "Dolby Vision and HDR10+ flags of footage", "the footage is not marked as Dolby Vision or HDR10+", 3},
    {"channelNames", "Channel names of multichannel audio files", "the channels are numbered", 3},
    {"mediaRights", "Rights and clearance notes on media", "they are not shown", 3},
    {"scaleWidth", "Scale Width (Uniform Scale off)", "the clips scale evenly by Scale", 3},
    {"typography", "OpenType features and optical kerning of text", "text uses the normal letterforms and kerning", 3},
    {"vr360", "VR / 360 video settings (projection and stereoscopic layout)",
     "the footage and the sequence are flat pictures (the viewer, the spherical metadata of exports and VR Reorient need 0.7)", 3},
    {"spectralRepair", "Spectral Repair edits", "the clips play without the repair; the effect is kept, empty", 3},
    {"surround71", "A 7.1 audio master, tracks and channel maps",
     "the master plays as 5.1 (its rear pair of surrounds is not heard) and the 7.1 tracks and clips pass their eight channels through as adaptive ones",
     3},
    {"mirroredFootage", "Mirrored (flipped) footage flags", "the footage shows unflipped", 3},
    {"bleepSounds", "Sounds that replace a bleep", "the usual bleep tone plays", 3},
    {"nearestNeighbor", "Nearest Neighbor scaling of clips", "the clips scale smoothly", 3},
    {"trackColors", "Track label colours", "the tracks have no colour", 3},
    {"captionEmphasis", "Caption emphasis words and animation timing", "the default timing is used", 3},
    {"captionTextRules", "Caption case, punctuation and accent rules", "captions show as written", 3},
    {"captionLooks", "Kinetic caption looks (word pop, bounce, highlight bar, typewriter, zoom)",
     "the caption shows without the animation", 3},
    {"sequenceDates", "Sequence creation dates", "no date is shown", 3},
    {"nestBin", "The bin nested sequences are filed in", "nested sequences go beside the sequence they come from", 3},
    {"lutFolders", "LUT folders", "all LUTs are listed together", 3},
    {"templateFolders", "Template folders", "all templates are listed together", 3},
    {"customWords", "Custom words for transcription", "the transcriber is not told them", 3},
    {"templateFieldKeys", "Animated template fields", "each field keeps its value at the start", 3},
    {"eqMatch", "EQ Match curves", "the Graphic Equalizer keeps its sliders, as an ordinary equalizer", 3},
    {"bilingualCaptions", "Captions drawn together (bilingual subtitles)", "one caption track is drawn at a time", 3},
    {"citations", "Sources and citations on markers", "they stay markers named after the claim, without the source details", 3},
    {"trackGroups", "Track groups", "the tracks are not grouped", 3},
    {"keyframePins", "Keyframes pinned to the start or end of their clip", "the keyframes keep their times and stop following a trim", 3},
    {"rippleTrackKeyframes", "Ripple Track Keyframes", "the sequence ripples its track keyframes no more", 3},
    {"clipNotes", "Clip notes", "they are not shown", 3},
    {"renderedNests", "Nested sequences rendered and replaced by a file",
     "the clips play the rendered file, and Restore Unrendered is not available", 3},
    {"lockedClips", "Locked clips", "the clips are not locked", 3},
    {"newEffects", "Effects that are new in 0.7 (AI Restore, AI Denoise, AI Refocus, AI Face Retouch, AI Eye Contact, Motion Blur, Skin Smoothing, "
                   "VR Reorient, Dynamics Processing)",
     "they stay in the file as unknown effects and do nothing", 3},
    {"auditions", "Auditions", "each keeps its active take; the other takes are left out", 2},
    {"masterClipEffects", "Master Clip Effects", "the clips play without them", 2},
    {"colorOutput", "Gamut compression, the output colour space and the SDR conform", "they are off", 2},
    {"textStyling", "Kerning off, capitals, faux bold and faux italic in text", "text uses the normal letterforms", 2},
    {"textLayout", "Text strokes, vertical alignment and Source Text keys", "text has one fill and no keyed properties",
     2},
    {"shadowSize", "The size of text shadows", "shadows keep their other settings", 2},
}};

// A key listed here is a field the release in `since` introduced; where a key name is also used for something older in
// another place, `parents` names the keys the new use sits under.
const std::array<Rule, 69>& rules() {
    static const std::array<Rule, 69> table{{
        {kAiInterpolation, "", "aiInterpolation"},
        {kMulticamNudges, "angles", "autoOffset"},
        {kDolbyVision, "video", "dolbyVision"},
        {kDolbyVision, "video", "hdr10Plus"},
        {kChannelNames, "audio", "channelNames"},
        {kMediaRights, "media", "rights"},
        {kScaleWidth, "parameters", "motion.scaleWidth"},
        {kScaleWidth, "parameters", "motion.uniformScale"},
        {kTypography, "", "opticalKerning"},
        {kTypography, "", "features"},
        {kVr360, "sequences", "vr"},
        {kVr360, "interpretation", "vr"},
        {kVr360, "video", "vr"},
        {kSurround71, "sequences", "masterFormat", Action::SurroundToFiveOne},
        {kSurround71, "audioTracks|submixTracks", "format", Action::SurroundToAdaptive},
        {kSurround71, "channels", "format", Action::SurroundToAdaptive},
        {kMirrored, "video", "mirrored"},
        {kBleepSounds, "redactions", "sound"},
        {kBleepSounds, "", "assets", Action::DropSoundAssets},
        {kNearest, "", "nearestNeighbor"},
        {kTrackColors, "videoTracks|audioTracks|submixTracks", "colorIndex"},
        {kCaptionTiming, "", "animationSpeed", Action::Remove, ""},
        {kCaptionTiming, "", "animationEase", Action::Remove, ""},
        {kCaptionTiming, "", "emphasis", Action::Remove, "none"},
        {kCaptionTiming, "", "emphasisWords", Action::Remove, ""},
        {kCaptionTiming, "", "emphasisColor", Action::Remove, ""},
        {kCaptionTiming, "", "emphasisScale", Action::Remove, ""},
        {kCaptionText, "", "textCase", Action::Remove, "asWritten"},
        {kCaptionText, "", "stripPunctuation", Action::Remove, "false"},
        {kCaptionText, "", "stripAccents", Action::Remove, "false"},
        {kCaptionLooks, "style", "animation", Action::AnimationToNone},
        {kSequenceDates, "sequences", "createdAt"},
        {kNestBin, "bins", "nestTarget"},
        {kLutFolders, "assets", "folder"},
        {kTemplateFolders, "templates", "folder"},
        {kCustomWords, "", "customWords"},
        {kFieldKeys, "fields", "keys"},
        {kEqMatch, "data", "eqMatch"},
        {kEqMatch, "data", "eqMatchReference"},
        {kEqMatch, "data", "eqMatchTarget"},
        {kBilingual, "captionTracks", "showTogether"},
        {kCitations, "markers", "citation"},
        {kCitations, "markers", "type", Action::SourceMarkerToComment},
        {kTrackGroups, "sequences", "trackGroups"},
        {kKeyframePins, "keys", "pinTo"},
        {kRippleTrackKeys, "sequences", "rippleTrackKeyframes"},
        {kClipNotes, "clips", "note"},
        {kRenderedNests, "clips", "renderedFrom"},
        {kClipLock, "clips", "locked"},
        {kAuditions, "", "audition"},
        {kMasterEffects, "media", "masterEffects"},
        {kMasterEffects, "", "masterEffectsOff"},
        {kColorOutput, "colorManagement", "gamutCompression"},
        {kColorOutput, "colorManagement", "gamutCompressionStrength"},
        {kColorOutput, "colorManagement", "outputSpace"},
        {kColorOutput, "colorManagement", "sdrConform"},
        {kTextStyling, "", "kerning"},
        {kTextStyling, "", "caps"},
        {kTextStyling, "", "fauxBold"},
        {kTextStyling, "", "fauxItalic"},
        {kTextLayout, "text", "verticalAlign"},
        {kTextLayout, "text", "strokes"},
        {kTextLayout, "text", "sources"},
        {kTextLayout, "text", "sourceKey"},
        {kTextLayout, "text", "sizeKeys"},
        {kTextLayout, "text", "trackingKeys"},
        {kTextLayout, "text", "leadingKeys"},
        {kTextLayout, "text", "baselineKeys"},
        {kShadowSize, "shadow", "size"},
    }};
    return table;
}

constexpr std::array<OlderRelease, 2> kReleases{{
    {"0.6", "Wabcut 0.6", "0.6.x", 19},
    {"0.5", "Wabcut 0.2 to 0.5", "0.2.x to 0.5.x", 18},
}};

/// The epoch of a release run: what it knows. 0.2 to 0.5 = 1, 0.6 = 2 (this build is 3).
int epochOf(const OlderRelease& release) { return release.id == "0.6" ? 2 : 1; }

bool parentMatches(std::string_view parents, std::string_view enclosing) {
    if (parents.empty()) {
        return true;
    }
    while (!parents.empty()) {
        const std::size_t bar = parents.find('|');
        if (parents.substr(0, bar) == enclosing) {
            return true;
        }
        if (bar == std::string_view::npos) {
            break;
        }
        parents.remove_prefix(bar + 1);
    }
    return false;
}

/// Whether `value` is what an unused feature leaves (Rule::defaultText).
bool isDefaultValue(const json& value, std::string_view defaultText) {
    if (defaultText == "~") {
        return false;
    }
    if (defaultText.empty()) {
        return true;
    }
    if (value.is_string()) {
        return value.get_ref<const std::string&>() == defaultText;
    }
    if (value.is_boolean()) {
        return (value.get<bool>() ? "true" : "false") == defaultText;
    }
    if (value.is_array()) {
        return value.empty() && defaultText == "[]";
    }
    return false;
}

/// The effects 0.7 added to the registry: an older release shows an instance of one as an unknown effect (the stack keeps it).
/// Spectral Repair is counted by its edits (kSpectralRepair).
bool isNewEffect(std::string_view effect) {
    static constexpr std::array<std::string_view, 9> kNew{
        "wabcut.ai.restore",   "wabcut.ai.denoise",             "wabcut.ai.refocus",
        "wabcut.ai.faceRetouch", "wabcut.ai.eyeContact",        "wabcut.blur.motion",
        "wabcut.retouch.skinSmoothing", "wabcut.vr.reorient", "wabcut.audio.dynamicsProcessing"};
    return std::find(kNew.begin(), kNew.end(), effect) != kNew.end();
}

bool isNewCaptionAnimation(const json& value) {
    if (!value.is_string()) {
        return false;
    }
    const std::string& name = value.get_ref<const std::string&>();
    return name == "wordPop" || name == "bounce" || name == "highlightBar" || name == "typewriter" || name == "zoom";
}

class Downgrader {
public:
    explicit Downgrader(int epoch) : m_epoch(epoch), m_touched(kFeatures.size()), m_extra(kFeatures.size(), 0) {
        for (const Rule& rule : rules()) {
            if (kFeatures[static_cast<std::size_t>(rule.feature)].since > epoch) {
                m_byKey.emplace(rule.key, &rule);
            }
        }
    }

    void walk(json& node, std::string_view enclosing) {
        if (node.is_array()) {
            for (json& element : node) {
                walk(element, enclosing); // an array is transparent: its elements sit under the array's own key
            }
            return;
        }
        if (!node.is_object()) {
            return;
        }
        // 0.7: an effect instance ({"id", "effect", "params", ...}) of an effect an older release does not have.
        if (const auto effect = node.find("effect"); effect != node.end() && effect->is_string() && node.contains("params") &&
                                                   isNewEffect(effect->get_ref<const std::string&>())) {
            m_touched[static_cast<std::size_t>(kNewEffects)].insert(&node);
        }
        for (auto it = node.begin(); it != node.end();) {
            const std::string key = it.key();
            // 0.7 (card 047): the edit lines of a clip's Spectral Repair ("e00" ... "e62" in an effect's data) are unknown to 0.6.
            if (m_epoch < 3 && enclosing == "data" && key.size() == 3 && key[0] == 'e' && std::isdigit(static_cast<unsigned char>(key[1])) &&
                std::isdigit(static_cast<unsigned char>(key[2])) && it.value().is_object() && it.value().contains("text")) {
                m_touched[static_cast<std::size_t>(kSpectralRepair)].insert(&node);
                it = node.erase(it);
                continue;
            }
            bool erased = false;
            const auto [first, last] = m_byKey.equal_range(key);
            for (auto r = first; r != last && !erased; ++r) {
                const Rule& rule = *r->second;
                if (!parentMatches(rule.parents, enclosing)) {
                    continue;
                }
                switch (rule.action) {
                case Action::Remove:
                    if (!isDefaultValue(it.value(), rule.defaultText)) {
                        m_touched[static_cast<std::size_t>(rule.feature)].insert(&node);
                    }
                    it = node.erase(it);
                    erased = true;
                    break;
                case Action::AnimationToNone:
                    if (isNewCaptionAnimation(it.value())) {
                        it.value() = "none";
                        m_touched[static_cast<std::size_t>(rule.feature)].insert(&node);
                    }
                    break;
                case Action::SourceMarkerToComment:
                    if (it.value().is_string() && it.value() == "source") {
                        m_touched[static_cast<std::size_t>(rule.feature)].insert(&node);
                        it = node.erase(it);
                        erased = true;
                    }
                    break;
                case Action::SurroundToFiveOne:
                case Action::SurroundToAdaptive:
                    if (it.value().is_string() && it.value() == "7.1") {
                        it.value() = rule.action == Action::SurroundToFiveOne ? "5.1" : "adaptive";
                        m_touched[static_cast<std::size_t>(rule.feature)].insert(&node);
                    }
                    break;
                case Action::DropSoundAssets:
                    if (it.value().is_array()) {
                        auto& list = it.value();
                        const std::size_t before = list.size();
                        list.erase(std::remove_if(list.begin(), list.end(),
                                                  [](const json& asset) {
                                                      return asset.is_object() && asset.contains("kind") &&
                                                             asset["kind"] == "sound";
                                                  }),
                                   list.end());
                        m_extra[static_cast<std::size_t>(rule.feature)] += static_cast<int>(before - list.size());
                    }
                    break;
                }
            }
            if (erased) {
                continue;
            }
            if (key == "parameters" && it.value().is_object()) {
                // Keyed by parameter ids: the walk does not go inside, but a parameter an older release does not have goes
                // (Scale Width and Uniform Scale, 0.7).
                json& parameters = it.value();
                for (auto p = parameters.begin(); p != parameters.end();) {
                    const auto [firstRule, lastRule] = m_byKey.equal_range(p.key());
                    bool gone = false;
                    for (auto r = firstRule; r != lastRule && !gone; ++r) {
                        const Rule& rule = *r->second;
                        if (rule.action == Action::Remove && parentMatches(rule.parents, "parameters")) {
                            m_touched[static_cast<std::size_t>(rule.feature)].insert(&parameters);
                            p = parameters.erase(p);
                            gone = true;
                        }
                    }
                    if (!gone) {
                        walk(p.value(), "parameter"); // its keyframes' pins (0.7, card 120)
                        ++p;
                    }
                }
            }
            if (key != "parameters") { // keyed by parameter ids: values are {"value"} / {"keys"}, nothing to look for
                const std::size_t had = it.value().is_object() ? it.value().size() : 0;
                walk(it.value(), key);
                if (key == "data" && had > 0 && it.value().is_object() && it.value().empty()) {
                    it = node.erase(it); // an effect whose data all went carries none (the writer omits an empty one)
                    continue;
                }
            }
            ++it;
        }
    }

    std::vector<DowngradeLoss> losses() const {
        std::vector<DowngradeLoss> out;
        for (std::size_t i = 0; i < kFeatures.size(); ++i) {
            const int count = static_cast<int>(m_touched[i].size()) + m_extra[i];
            if (count > 0 && kFeatures[i].since > m_epoch) {
                out.push_back(DowngradeLoss{std::string(kFeatures[i].id), std::string(kFeatures[i].what),
                                            std::string(kFeatures[i].effect), count});
            }
        }
        return out;
    }

private:
    int m_epoch;
    std::unordered_multimap<std::string_view, const Rule*> m_byKey;
    std::vector<std::set<const json*>> m_touched;
    std::vector<int> m_extra;
};

} // namespace

std::span<const OlderRelease> olderReleases() { return kReleases; }

const OlderRelease* findOlderRelease(std::string_view version) {
    // "Wabcut 0.6.2", "v0.4", "0.3.1": the first "major.minor" in the text.
    std::size_t i = 0;
    while (i < version.size() && !std::isdigit(static_cast<unsigned char>(version[i]))) {
        ++i;
    }
    int major = 0;
    int minor = 0;
    std::size_t digits = 0;
    while (i < version.size() && std::isdigit(static_cast<unsigned char>(version[i])) && digits < 4) {
        major = major * 10 + (version[i++] - '0');
        ++digits;
    }
    if (digits == 0 || i >= version.size() || version[i] != '.') {
        return nullptr;
    }
    ++i;
    digits = 0;
    while (i < version.size() && std::isdigit(static_cast<unsigned char>(version[i])) && digits < 4) {
        minor = minor * 10 + (version[i++] - '0');
        ++digits;
    }
    if (digits == 0 || major != 0) {
        return nullptr;
    }
    if (minor == 6) {
        return &kReleases[0];
    }
    if (minor >= 2 && minor <= 5) {
        return &kReleases[1];
    }
    return nullptr;
}

std::expected<DowngradeResult, Error> downgradeJson(std::string_view currentJson, std::string_view release) {
    const OlderRelease* target = findOlderRelease(release);
    if (!target) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     "Wabcut cannot write a project for \"" + std::string(release) +
                                         "\": choose Wabcut 0.6 or one of 0.2 to 0.5"});
    }
    if (!jsonNestingWithin(currentJson)) {
        return std::unexpected(Error{ErrorCode::ParseError, "the project nests too deep"});
    }
    json root = json::parse(currentJson.begin(), currentJson.end(), nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("schemaVersion") ||
        root["schemaVersion"] != kSchemaVersion) {
        return std::unexpected(Error{ErrorCode::ParseError, "not a project of this version of Wabcut"});
    }
    Downgrader downgrader(epochOf(*target));
    try {
        downgrader.walk(root, "");
        root["schemaVersion"] = target->schema;
        DowngradeResult result;
        result.json = root.dump(2);
        result.release = std::string(target->id);
        result.schema = target->schema;
        result.losses = downgrader.losses();
        return result;
    } catch (const json::exception& e) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, std::string("cannot write the project: ") + e.what()});
    }
}

std::expected<DowngradeResult, Error> toJsonForRelease(const ProjectState& state, std::string_view release) {
    if (!findOlderRelease(release)) {
        return downgradeJson({}, release); // the same message
    }
    auto text = toJson(state);
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    return downgradeJson(*text, release);
}

std::expected<DowngradeResult, Error> saveProjectForRelease(const ProjectState& state, const std::filesystem::path& file,
                                                            std::string_view release) {
    auto result = toJsonForRelease(state, release);
    if (!result) {
        return result;
    }
    // The text must read back before a file is touched (the build that reads it later migrates it up like any old file).
    if (auto loaded = fromJson(result->json); !loaded) {
        return std::unexpected(Error{ErrorCode::InvalidArgument,
                                     "the project written for " + result->release +
                                         " does not load back: " + loaded.error().message});
    }
    if (auto written = writeFileAtomically(file, result->json); !written) {
        return std::unexpected(std::move(written.error()));
    }
    return result;
}

std::vector<DowngradeRuleInfo> downgradeRules() {
    std::vector<DowngradeRuleInfo> out;
    for (const Rule& rule : rules()) {
        const Feature& feature = kFeatures[static_cast<std::size_t>(rule.feature)];
        out.push_back(DowngradeRuleInfo{feature.id, rule.key, feature.since});
    }
    return out;
}

} // namespace wabcut::io
