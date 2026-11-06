// 0.7 (card 059): File > Save for an Older Version. A project is written the way an older release reads it: that release's
// schema, no field it does not have, an animation name it cannot parse turned into "none", and a list of what was left out.
// The key-by-key guard against the older releases' key lists is in test_project_io.cpp ("Every key a project file uses ...").

#include "TimelineFixture.h"

#include <wabcut/model/AudioChannelOps.h>
#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/ClipLock.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/KeyPins.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/ProjectDowngrade.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequenceSettings.h>
#include <wabcut/model/SpectralRepair.h>
#include <wabcut/model/TrackGroups.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
namespace fs = std::filesystem;

namespace {

template <typename T>
T must(std::expected<T, Error> value) {
    REQUIRE(value);
    return std::move(*value);
}

std::string readAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

const io::DowngradeLoss* lossOf(const io::DowngradeResult& result, std::string_view id) {
    for (const io::DowngradeLoss& loss : result.losses) {
        if (loss.id == id) {
            return &loss;
        }
    }
    return nullptr;
}

fs::path scratchDir(const char* name) {
    const fs::path dir = fs::temp_directory_path() / "wabcut-downgrade-test" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("Older releases: the versions offered and how a version text finds its run", "[model][io][downgrade]") {
    const auto offered = io::olderReleases();
    REQUIRE(offered.size() == 2);
    CHECK(offered[0].id == "0.6");
    CHECK(offered[0].schema == 19);
    CHECK(offered[1].schema == 18); // 0.2 to 0.5 share one schema
    for (const io::OlderRelease& release : offered) {
        CHECK(release.schema <= io::kSchemaVersion);
    }
    for (const char* text : {"0.6", "0.6.0", "0.6.2", "Wabcut 0.6", "v0.6.1"}) {
        INFO(text);
        REQUIRE(io::findOlderRelease(text));
        CHECK(io::findOlderRelease(text)->id == "0.6");
    }
    for (const char* text : {"0.5", "0.4.1", "0.3", "0.2.0", "Wabcut 0.2", "v0.5.2"}) {
        INFO(text);
        REQUIRE(io::findOlderRelease(text));
        CHECK(io::findOlderRelease(text)->schema == 18);
    }
    // Not offered: 0.1 (a different file), this build's own and newer ones, and anything that is not a version.
    for (const char* text : {"0.1", "0.1.3", "0.7", "0.8.1", "1.0", "", "banana", "0.", "6"}) {
        INFO(text);
        CHECK(io::findOlderRelease(text) == nullptr);
    }
    const auto refused = io::toJsonForRelease(ProjectState{}, "0.1");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::InvalidArgument);
    CHECK(refused.error().message.find("0.1") != std::string::npos);
}

TEST_CASE("Older releases: a project with nothing new is the same file with the release's schema number", "[model][io][downgrade]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
    )");
    const std::string current = must(io::toJson(tl.state));
    const auto six = io::toJsonForRelease(tl.state, "0.6");
    REQUIRE(six);
    CHECK(six->losses.empty());
    CHECK(six->schema == 19);
    CHECK(six->json == current); // 0.7 kept schema 19: nothing to change when nothing new is used

    const auto five = io::toJsonForRelease(tl.state, "0.5");
    REQUIRE(five);
    CHECK(five->losses.empty());
    CHECK(five->schema == 18);
    std::string expected = current;
    const std::string was = "\"schemaVersion\": 19";
    expected.replace(expected.find(was), was.size(), "\"schemaVersion\": 18");
    CHECK(five->json == expected);

    // It reads back in this build as the same project.
    const ProjectState loaded = must(io::fromJson(five->json));
    CHECK(must(io::toJson(loaded)) == current);
}

TEST_CASE("Older releases: 0.7 additions are left out and listed, the rest stays", "[model][io][downgrade]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
    )");
    ProjectState s = tl.state;
    const SequenceId seq = tl.seq;
    // Two caption tracks: kinetic looks, emphasis, case rules and a bilingual mark (0.7); a plain animation (0.6 knows it).
    CaptionStyle kinetic;
    kinetic.animation = CaptionAnimation::Typewriter;
    kinetic.emphasis = CaptionEmphasis::Keywords;
    kinetic.emphasisWords = {"epic"};
    kinetic.textCase = CaptionTextCase::Upper;
    kinetic.stripPunctuation = true;
    CaptionStyle plain;
    plain.animation = CaptionAnimation::Pop;
    auto first = must(edit::addCaptionTrack(s, seq, "English", "en", kinetic));
    s = first.state;
    auto second = must(edit::addCaptionTrack(s, seq, "Spanish", "es", kinetic));
    s = second.state;
    auto third = must(edit::addCaptionTrack(s, seq, "French", "fr", plain));
    s = third.state;
    edit::CaptionTrackChange together;
    together.showTogether = true;
    s = must(edit::setCaptionTrackInfo(s, seq, second.createdCaptionTrack, together)).state;
    s = must(edit::setCustomWords(s, {"Malenia"}));

    for (const char* release : {"0.6", "0.5"}) {
        INFO(release);
        const auto down = io::toJsonForRelease(s, release);
        REQUIRE(down);
        const std::string& text = down->json;
        // Gone: every key the older release does not have; the animations it cannot name are "none".
        for (const char* key : {"showTogether", "emphasisWords", "emphasisColor", "textCase", "stripPunctuation", "customWords"}) {
            INFO(key);
            CHECK(text.find(std::string("\"") + key + "\"") == std::string::npos);
        }
        CHECK(text.find("\"typewriter\"") == std::string::npos);
        CHECK(text.find("\"animation\": \"pop\"") != std::string::npos); // 0.6 and before have Pop: kept
        CHECK(text.find("\"animation\": \"none\"") != std::string::npos);
        // The report.
        const auto* looks = lossOf(*down, "captionLooks");
        REQUIRE(looks);
        CHECK(looks->count == 2); // the two kinetic tracks
        CHECK_FALSE(looks->what.empty());
        CHECK_FALSE(looks->effect.empty());
        REQUIRE(lossOf(*down, "captionEmphasis"));
        CHECK(lossOf(*down, "captionEmphasis")->count == 2);
        REQUIRE(lossOf(*down, "captionTextRules"));
        REQUIRE(lossOf(*down, "bilingualCaptions"));
        CHECK(lossOf(*down, "bilingualCaptions")->count == 1);
        REQUIRE(lossOf(*down, "customWords"));
        CHECK(lossOf(*down, "auditions") == nullptr); // nothing of 0.6 in this project
        // What the older release can read stays: the track names, languages and the three tracks.
        const ProjectState loaded = must(io::fromJson(text));
        const Sequence& back = *loaded.findSequence(seq);
        REQUIRE(back.captionTracks.size() == 3);
        CHECK(back.captionTracks[0].name == "English");
        CHECK(back.captionTracks[1].language == "es");
        CHECK_FALSE(back.captionTracks[1].showTogether);
        CHECK(back.captionTracks[0].style.animation == CaptionAnimation::None);
        CHECK(back.captionTracks[2].style.animation == CaptionAnimation::Pop);
        CHECK(back.captionTracks[0].style.textCase == CaptionTextCase::AsWritten);
        CHECK(loaded.customWords.empty());
    }
    // The project in memory was not touched, and a save for the current version keeps everything.
    CHECK(s.findSequence(seq)->captionTracks[1].showTogether);
    const ProjectState same = must(io::fromJson(must(io::toJson(s))));
    CHECK(same.findSequence(seq)->captionTracks[0].style.animation == CaptionAnimation::Typewriter);
}

TEST_CASE("Older releases: sounds that replace a bleep are dropped from the assets, LUTs stay", "[model][io][downgrade]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    ProjectState s = tl.state;
    ProjectAsset lut;
    lut.id = s.ids.allocate<AssetId>();
    lut.kind = "lut";
    lut.name = "Warm";
    lut.locator.path = "warm.cube";
    lut.folder = "Film/Kodak"; // 0.7: LUT folders
    ProjectAsset sound = lut;
    sound.id = s.ids.allocate<AssetId>();
    sound.kind = std::string(kAssetKindSound);
    sound.name = "Ding";
    sound.locator.path = "ding.wav";
    sound.folder.clear();
    s.assets = {lut, sound};
    const auto down = io::toJsonForRelease(s, "0.6");
    REQUIRE(down);
    const ProjectState loaded = must(io::fromJson(down->json));
    REQUIRE(loaded.assets.size() == 1);
    CHECK(loaded.assets[0].kind == "lut");
    CHECK(loaded.assets[0].name == "Warm");
    CHECK(loaded.assets[0].folder.empty());
    REQUIRE(lossOf(*down, "bleepSounds"));
    CHECK(lossOf(*down, "bleepSounds")->count == 1);
    REQUIRE(lossOf(*down, "lutFolders"));
}

TEST_CASE("Older releases: saving writes the file atomically after checking it reads back", "[model][io][downgrade]") {
    const fs::path dir = scratchDir("save");
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
    )");
    ProjectState s = tl.state;
    s = must(edit::setCustomWords(s, {"Radahn"}));
    const fs::path file = dir / "handoff.wabcut";
    const auto saved = io::saveProjectForRelease(s, file, "Wabcut 0.5");
    REQUIRE(saved);
    CHECK(saved->release == "0.5");
    REQUIRE(lossOf(*saved, "customWords"));
    CHECK(fs::exists(file));
    CHECK(readAll(file) == saved->json);
    CHECK_FALSE(fs::exists(io::backupPath(file, 1))); // a copy for someone else: no rotation
    const auto loaded = io::loadProject(file);
    REQUIRE(loaded);
    CHECK(loaded->customWords.empty());
    // A second save over it does not leave a backup either, and replaces the file.
    const auto again = io::saveProjectForRelease(tl.state, file, "0.6");
    REQUIRE(again);
    CHECK(again->losses.empty());
    CHECK_FALSE(fs::exists(io::backupPath(file, 1)));
    // An unknown release writes nothing.
    const fs::path other = dir / "other.wabcut";
    CHECK_FALSE(io::saveProjectForRelease(tl.state, other, "0.1"));
    CHECK_FALSE(fs::exists(other));
}

TEST_CASE("Older releases: a file from a newer Wabcut says how to get one this build can open", "[model][io][downgrade]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )");
    std::string text = must(io::toJson(tl.state));
    const std::string was = "\"schemaVersion\": " + std::to_string(io::kSchemaVersion);
    text.replace(text.find(was), was.size(), "\"schemaVersion\": " + std::to_string(io::kSchemaVersion + 1));
    const auto refused = io::fromJson(text);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::VersionMismatch);
    CHECK(refused.error().message.find("Save for an Older Version") != std::string::npos);
}

TEST_CASE("Older releases: the rule table names every field once and orders the report newest first", "[model][io][downgrade]") {
    const auto rules = io::downgradeRules();
    CHECK(rules.size() > 20);
    bool seenOlder = false;
    for (const io::DowngradeRuleInfo& rule : rules) {
        CHECK_FALSE(rule.key.empty());
        CHECK((rule.since == 2 || rule.since == 3));
        if (rule.since == 2) {
            seenOlder = true;
        } else {
            CHECK_FALSE(seenOlder); // 0.7's rules first, then 0.6's
        }
    }
}

TEST_CASE("Older releases: AI Frame Interpolation becomes Optical Flow and is listed", "[model][io][downgrade]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
    )");
    ProjectState s = tl.state;
    const ClipId first = tl.clip("V1@0");
    s = must(edit::setTimeInterpolation(s, tl.seq, std::span(&first, 1), TimeInterpolation::AiInterpolation)).state;
    const auto current = io::toJson(s);
    REQUIRE(current);
    // The current file keeps what an older build can read: the mode as Optical Flow, plus the extra key.
    CHECK(current->find("\"timeInterpolation\": \"opticalFlow\"") != std::string::npos);
    CHECK(current->find("\"aiInterpolation\": true") != std::string::npos);
    CHECK(current->find("\"timeInterpolation\": \"aiInterpolation\"") == std::string::npos);
    const ProjectState back = must(io::fromJson(*current));
    CHECK(back.findSequence(tl.seq)->findClip(first)->clip().timeInterpolation() == TimeInterpolation::AiInterpolation);
    for (const char* release : {"0.6", "0.5"}) {
        INFO(release);
        const auto down = io::toJsonForRelease(s, release);
        REQUIRE(down);
        CHECK(down->json.find("\"aiInterpolation\"") == std::string::npos);
        CHECK(down->json.find("\"timeInterpolation\": \"opticalFlow\"") != std::string::npos);
        const auto* loss = lossOf(*down, "aiInterpolation");
        REQUIRE(loss);
        CHECK(loss->count == 1);
        CHECK_FALSE(loss->effect.empty());
        const ProjectState older = must(io::fromJson(down->json));
        CHECK(older.findSequence(tl.seq)->findClip(first)->clip().timeInterpolation() == TimeInterpolation::OpticalFlow);
    }
    // Another mode is not an AI interpolation: nothing to list.
    s = must(edit::setTimeInterpolation(s, tl.seq, std::span(&first, 1), TimeInterpolation::OpticalFlow)).state;
    const auto plain = io::toJsonForRelease(s, "0.6");
    REQUIRE(plain);
    CHECK(lossOf(*plain, "aiInterpolation") == nullptr);
}

TEST_CASE("Older releases: Scale Width and Uniform Scale off are left out and listed (card 12)", "[model][io][downgrade]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
    )");
    ProjectState s = tl.state;
    const ClipId first = tl.clip("V1@0");
    s = must(edit::setParameter(s, tl.seq, first, edit::ParamRef::of(ParamId::UniformScale), ParamValue(0.0))).state;
    s = must(edit::setParameter(s, tl.seq, first, edit::ParamRef::of(ParamId::ScaleWidth), ParamValue(0.5))).state;
    const auto current = io::toJson(s);
    REQUIRE(current);
    CHECK(current->find("motion.scaleWidth") != std::string::npos);
    for (const char* release : {"0.6", "0.5"}) {
        INFO(release);
        const auto down = io::toJsonForRelease(s, release);
        REQUIRE(down);
        const std::size_t at = down->json.find("motion.scaleWidth");
        INFO((at == std::string::npos ? std::string("gone") : down->json.substr(at > 200 ? at - 200 : 0, 400)));
        CHECK(at == std::string::npos);
        CHECK(down->json.find("motion.uniformScale") == std::string::npos);
        const auto* loss = lossOf(*down, "scaleWidth");
        REQUIRE(loss);
        CHECK(loss->count >= 1);
        // The older release reads the clip: it scales evenly by Scale.
        const ProjectState older = must(io::fromJson(down->json));
        CHECK(older.findSequence(tl.seq)->findClip(first)->clip().properties().uniformScale());
    }
}

TEST_CASE("Older releases: a 7.1 mix is saved as a 5.1 master with adaptive tracks and listed", "[model][io][downgrade][channels]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
        A1 tgt |AaaaBbbb|
        A2     |........|
    )");
    ProjectState s = tl.state;
    s = must(edit::setMasterFormat(s, tl.seq, ChannelFormat::Surround71)).state;
    edit::TrackStateChange eight;
    eight.format = ChannelFormat::Surround71;
    s = must(edit::setTrackState(s, tl.seq, tl.track("A1"), eight)).state;
    const ClipId clip[] = {tl.clip("A1@0")};
    s = must(edit::setClipChannels(s, tl.seq, clip, AudioChannelMap::identity(8))).state;

    for (const char* release : {"0.6", "0.5"}) {
        INFO(release);
        const auto down = io::toJsonForRelease(s, release);
        REQUIRE(down);
        // The older release has no 7.1: nothing in the file says it any more.
        CHECK(down->json.find("\"7.1\"") == std::string::npos);
        CHECK(down->json.find("\"masterFormat\": \"5.1\"") != std::string::npos);
        const auto* loss = lossOf(*down, "surround71");
        REQUIRE(loss);
        CHECK(loss->count == 3); // the master, the track and the clip's map
        CHECK_FALSE(loss->what.empty());
        CHECK_FALSE(loss->effect.empty());
        // What the older release reads: a 5.1 master, an adaptive track and an eight-channel adaptive map.
        const ProjectState loaded = must(io::fromJson(down->json));
        CHECK(loaded.findSequence(tl.seq)->masterFormat == ChannelFormat::Surround51);
        CHECK(loaded.findSequence(tl.seq)->findTrack(tl.track("A1"))->format == ChannelFormat::Adaptive);
        const AudioChannelMap* map = loaded.findSequence(tl.seq)->findClip(tl.clip("A1@0"))->clip().channelMap();
        REQUIRE(map);
        CHECK(map->format == ChannelFormat::Adaptive);
        CHECK(map->channelCount() == 8);
    }
    // The project in memory keeps its 7.1; a project without it lists nothing.
    CHECK(s.findSequence(tl.seq)->masterFormat == ChannelFormat::Surround71);
    CHECK(lossOf(must(io::toJsonForRelease(tl.state, "0.6")), "surround71") == nullptr);
}

TEST_CASE("Older releases: a clip's Spectral Repair edits are left out and listed", "[model][io][downgrade][spectral]") {
    Timeline tl(R"(
        A1 tgt |Aaaa|
    )");
    ProjectState s = tl.state;
    const ClipId clip = clipAt(s, tl.seq, "A1@0");
    const auto added = must(edit::addEffect(s, tl.seq, edit::EffectTarget::ofClip(clip), std::string(kSpectralRepairEffect), {}, 0));
    SpectralEdit heal;
    heal.kind = SpectralKind::Heal;
    heal.start = 1.0;
    heal.end = 2.0;
    heal.low = 200.0;
    heal.high = 900.0;
    edit::EffectChange change;
    change.data = packSpectralEdits(std::vector<SpectralEdit>{heal});
    REQUIRE_FALSE(change.data.empty());
    s = must(edit::setEffect(added.state, tl.seq, clip, added.createdEffects.front(), change)).state;
    // This version writes the edits; an older release's file has none of them and the report names the loss.
    const auto current = io::toJson(s);
    REQUIRE(current);
    CHECK(current->find("\"e00\"") != std::string::npos);
    for (const char* release : {"0.6", "0.5"}) {
        INFO(release);
        const auto down = io::toJsonForRelease(s, release);
        REQUIRE(down);
        CHECK(down->json.find("\"e00\"") == std::string::npos);
        REQUIRE(lossOf(*down, "spectralRepair"));
        CHECK(lossOf(*down, "spectralRepair")->count == 1);
        CHECK_FALSE(lossOf(*down, "spectralRepair")->effect.empty());
        // The effect stays, without edits: the older release keeps it and plays the clip without it.
        const ProjectState back = must(io::fromJson(down->json));
        bool found = false;
        for (const EffectInstance& e : back.findSequence(tl.seq)->findClip(clip)->clip().effects()) {
            found = found || (e.effect == kSpectralRepairEffect && e.data.empty());
        }
        CHECK(found);
    }
    // A project without a repair reports nothing of it.
    const auto none = io::toJsonForRelease(tl.state, "0.6");
    REQUIRE(none);
    CHECK(lossOf(*none, "spectralRepair") == nullptr);
}

TEST_CASE("Older releases: track groups, key pins, clip notes, locked clips, Ripple Track Keyframes and the 0.7 effects are left out and listed",
          "[model][io][downgrade]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb|
        A1 tgt |AaaaBbbb|
    )");
    ProjectState s = tl.state;
    const SequenceId seq = tl.seq;
    const ClipId video = tl.clip("V1@0");
    // A note and a lock on a clip, a keyframe pinned to the clip's start, Ripple Track Keyframes, a track group and an effect
    // 0.6 has no registry entry for.
    edit::ClipAttributeChange note;
    note.note = "Cut the cough";
    s = must(edit::setClipAttributes(s, seq, std::span(&video, 1), note)).state;
    s = must(edit::addKeyframe(s, seq, video, edit::ParamRef::of(ParamId::Opacity), Keyframe{f(1), 0.5})).state;
    s = must(edit::setKeyframePin(s, seq, video, edit::ParamRef::of(ParamId::Opacity), 0, KeyPin::ClipStart)).state;
    edit::SequenceSettingsChange ripple;
    ripple.rippleTrackKeyframes = true;
    s = must(edit::setSequenceSettings(s, seq, ripple)).state;
    const std::array<TrackId, 1> grouped{tl.track("A1")};
    s = must(edit::createTrackGroup(s, seq, grouped, "Dialogue", 3)).state;
    s = must(edit::addEffect(s, seq, std::span(&video, 1), "wabcut.blur.motion")).state;
    s = must(edit::addEffect(s, seq, std::span(&video, 1), "wabcut.vr.reorient")).state;
    const ClipId locked = tl.clip("V1@4");
    s = must(edit::setClipsLocked(s, seq, std::span(&locked, 1), true)).state;

    const auto current = io::toJson(s);
    REQUIRE(current);
    for (const char* key : {"trackGroups", "pinTo", "rippleTrackKeyframes", "\"note\"", "wabcut.blur.motion"}) {
        INFO(key);
        CHECK(current->find(key) != std::string::npos);
    }
    for (const char* release : {"0.6", "0.5"}) {
        INFO(release);
        const auto down = io::toJsonForRelease(s, release);
        REQUIRE(down);
        // Gone: the keys the older release does not read.
        for (const char* key : {"trackGroups", "pinTo", "rippleTrackKeyframes", "\"note\""}) {
            INFO(key);
            CHECK(down->json.find(key) == std::string::npos);
        }
        // The clip lock key only: a track's "locked" stays (0.6 knows it).
        CHECK(down->json.find("\"locked\": true") == std::string::npos);
        // Reported, once per place.
        for (const char* id : {"trackGroups", "keyframePins", "rippleTrackKeyframes", "clipNotes", "lockedClips"}) {
            INFO(id);
            const auto* loss = lossOf(*down, id);
            REQUIRE(loss);
            CHECK(loss->count == 1);
            CHECK_FALSE(loss->what.empty());
            CHECK_FALSE(loss->effect.empty());
        }
        // The two new effects stay in the file (the older release lists them as unknown effects) and are reported.
        CHECK(down->json.find("wabcut.blur.motion") != std::string::npos);
        REQUIRE(lossOf(*down, "newEffects"));
        CHECK(lossOf(*down, "newEffects")->count == 2);
        // The older release's file loads back, with the clip and its keyframe where they were.
        const ProjectState back = must(io::fromJson(down->json));
        CHECK(back.findSequence(seq)->trackGroups.empty());
        CHECK(back.findSequence(seq)->findClip(video)->clip().note().empty());
        CHECK_FALSE(back.findSequence(seq)->findClip(locked)->clip().locked);
    }
    // The project in memory keeps all of it; a project without them reports none.
    CHECK_FALSE(s.findSequence(seq)->trackGroups.empty());
    const auto none = must(io::toJsonForRelease(tl.state, "0.6"));
    for (const char* id : {"trackGroups", "keyframePins", "rippleTrackKeyframes", "clipNotes", "lockedClips", "newEffects"}) {
        INFO(id);
        CHECK(lossOf(none, id) == nullptr);
    }
}
