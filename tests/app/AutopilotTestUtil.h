#pragma once

// Fixtures of the creator autopilot tests (#62): a sequence of one A-roll recording with what is said in it (words laid
// out one by one), library items with names, Descriptions and clip markers, a smart search whose embedder maps colour
// words to pictures (one red shot), drawn frames (black, flat, then colourful) and a still writer that only remembers
// where it was asked for.

#include "TimelineFixture.h"

#include "../ai/search/SearchTestUtil.h"

#include <wabcut/ai/autopilot/AutopilotCommands.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/command/FrameServices.h>
#include <wabcut/model/EditSession.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wabcut::test::autopilotfx {

using command::Json;

inline Ticks secs(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

/// Lays out the words of `text` from `start` seconds (0.32 s each, 0.08 s apart) into `words`.
inline void say(std::vector<ai::TranscriptWord>& words, double start, std::string_view text) {
    double t = start;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t space = text.find(' ', i);
        const std::size_t end = space == std::string_view::npos ? text.size() : space;
        if (end > i) {
            ai::TranscriptWord w;
            w.text = std::string(text.substr(i, end - i));
            w.t0 = secs(t);
            w.t1 = secs(t + 0.32);
            w.confidence = 0.9f;
            words.push_back(std::move(w));
            t += 0.40;
        }
        i = end + 1;
    }
}

/// A sequence of one A-roll recording (media A on V1 and A1, linked) of `seconds`, with what is said in it.
struct Recording {
    Recording(int seconds, const std::vector<std::pair<double, std::string>>& lines, std::string picture = {}) : tl(pictureOf(seconds, picture), {{'A', seconds * 25 + 50}}) {
        tl.linkMatching();
        for (char letter = 'A'; letter <= 'Z'; ++letter) {
            ids[letter] = tl.media(letter); // Timeline::media() finds items by name: describe() renames them
        }
        std::vector<ai::TranscriptWord> words;
        for (const auto& [at, text] : lines) {
            say(words, at, text);
        }
        auto t = std::make_shared<ai::Transcript>();
        t->language = "en";
        t->words = std::move(words);
        transcripts = std::make_shared<ai::edit::MapTranscriptProvider>();
        transcripts->add(media('A'), 0, t);
        auto seq = std::make_shared<Sequence>(*tl.state.findSequence(tl.seq));
        seq->name = "Telescope build";
        tl.state = tl.state.withSequence(seq);
    }
    static std::string pictureOf(int seconds, const std::string& picture) {
        if (!picture.empty()) {
            return picture;
        }
        const std::string clip = "A" + std::string(static_cast<std::size_t>(seconds * 25 - 1), 'a');
        return "V1 tgt |" + clip + "|\nA1 tgt |" + clip + "|\n";
    }
    /// Gives media `letter` a name, a Description and clip markers (source seconds, name).
    void describe(char letter, std::string name, std::string description = {}, std::vector<std::pair<double, std::string>> markers = {}) {
        for (MediaPtr& m : tl.state.media) {
            if (m->id == media(letter)) {
                auto item = std::make_shared<MediaItem>(*m);
                item->name = std::move(name);
                item->description = std::move(description);
                for (const auto& [at, label] : markers) {
                    Marker marker;
                    marker.start = secs(at);
                    marker.name = label;
                    item->markers.push_back(marker);
                }
                m = item;
            }
        }
    }
    void addMarker(double at, std::string name, MarkerType type = MarkerType::Chapter) {
        auto seq = std::make_shared<Sequence>(*tl.state.findSequence(tl.seq));
        Marker m;
        m.start = secs(at);
        m.name = std::move(name);
        m.type = type;
        seq->markers.push_back(std::move(m));
        std::ranges::stable_sort(seq->markers, [](const Marker& a, const Marker& b) { return a.start < b.start; });
        tl.state = tl.state.withSequence(seq);
    }

    MediaId media(char letter) const { return ids.at(letter); }

    Timeline tl;
    std::map<char, MediaId> ids;
    std::shared_ptr<ai::edit::MapTranscriptProvider> transcripts;
};

/// The editing catalog on a session of `state` with `services`.
struct Harness {
    Harness(const ProjectState& state, std::shared_ptr<command::CommandServices> services)
        : session(state), engine(session, ai::edit::editingCommands(), command::EnginePolicy{command::CapabilitySet::all(), 64, std::move(services)}) {}
    command::CommandResult run(std::string name, Json params, bool dryRun = false) {
        command::CommandRequest r;
        r.command = std::move(name);
        r.params = std::move(params);
        r.dryRun = dryRun;
        return engine.execute(r);
    }
    command::CommandResult ok(std::string name, Json params, bool dryRun = false) {
        command::CommandResult result = run(std::move(name), std::move(params), dryRun);
        INFO(command::toJson(result).dump(2));
        REQUIRE(result.ok());
        return result;
    }
    EditSession session;
    command::CommandEngine engine;
};

inline std::shared_ptr<command::CommandServices> bundle(std::vector<std::shared_ptr<command::CommandServices>> parts) {
    return std::make_shared<command::ServiceBundle>(std::move(parts));
}

/// The video essay of the B-roll tests: what is said, every 10 s, over a minute.
inline std::vector<std::pair<double, std::string>> brollLines() {
    return {{1.0, "Welcome back to the channel everyone."},
            {8.0, "Last week I flew my drone over the city at night."},
            {18.0, "Then I cooked a big pot of pasta in my kitchen."},
            {28.0, "My friend drives a red sports car on the track."},
            {38.0, "We talked about the weather for a while."},
            {46.0, "In the morning we went surfing at the beach."},
            {54.0, "Thanks for watching and see you next time."}};
}

inline Recording brollEssay(const std::string& picture = {}) {
    Recording r(60, brollLines(), picture);
    r.describe('B', "drone_city_night.mp4");
    r.describe('C', "IMG_2041.MOV", "cooking pasta in the kitchen");
    r.describe('D', "Beach day.mp4", {}, {{12.0, "surfing at the beach"}});
    return r;
}

inline const Json* suggestionFor(const Json& data, MediaId media) {
    for (const Json& s : data["suggestions"]) {
        if (s["media"].get<std::uint64_t>() == media.value) {
            return &s;
        }
    }
    return nullptr;
}

/// A smart search over the fixture's media E: one red shot (the fixture embedder maps "red" in a text to it).
struct RedShots {
    explicit RedShots(const Recording& r) : dir("autopilot") {
        access = std::make_shared<ai::search::test::FixtureAccess>();
        access->items[r.media('E').value] =
            ai::search::test::FixtureAccess::Item{0xE1, "2026-10-01", ai::search::test::shotsAt(40, {20}),
                                                  {ai::search::test::kRed, ai::search::test::kRed}, nullptr, false};
        embedder = std::make_shared<ai::search::test::FixtureEmbedder>();
        auto opened = ai::search::SearchIndex::open(dir.path / "index", embedder->modelVersion(), ai::search::test::kFixtureDim);
        REQUIRE(opened);
        search = std::make_shared<ai::search::FootageSearch>(ai::search::FootageSearchOptions{},
                                                             std::shared_ptr<ai::search::SearchIndex>(opened->release()), access, embedder);
        const MediaId ids[] = {r.media('E')};
        REQUIRE(search->index(r.tl.state, ids));
    }
    ai::search::test::TempDir dir;
    std::shared_ptr<ai::search::test::FixtureAccess> access;
    std::shared_ptr<ai::search::test::FixtureEmbedder> embedder;
    std::shared_ptr<ai::search::FootageSearch> search;
};

/// Pictures by source time: black before 30 s, flat grey to 50 s, a colourful checkerboard after.
inline std::optional<ai::search::Image> drawnFrame(const MediaItem&, Ticks t) {
    ai::search::Image image;
    image.width = 320;
    image.height = 180;
    image.rgb.resize(320 * 180 * 3);
    const double s = t.toSeconds();
    for (int y = 0; y < 180; ++y) {
        for (int x = 0; x < 320; ++x) {
            std::uint8_t* p = &image.rgb[(static_cast<std::size_t>(y) * 320 + static_cast<std::size_t>(x)) * 3];
            if (s < 30.0) {
                p[0] = p[1] = p[2] = 4;
            } else if (s < 50.0) {
                p[0] = p[1] = p[2] = 128;
            } else {
                const bool on = ((x / 8) + (y / 8)) % 2 == 0;
                p[0] = on ? 230 : 30;
                p[1] = on ? 60 : 200;
                p[2] = on ? 40 : 220;
            }
        }
    }
    return image;
}

/// A still writer that writes a few bytes and remembers where it was asked for.
inline std::shared_ptr<command::FrameExportServices> fakeStills(std::shared_ptr<std::vector<Ticks>> asked) {
    return std::make_shared<command::FrameExportServices>(
        [asked](const ProjectState&, SequenceId, Ticks time, const std::filesystem::path& file, const std::string&,
                const command::FrameExportServices::Options& o) -> std::expected<command::FrameExportServices::Written, Error> {
            asked->push_back(time);
            std::ofstream(file, std::ios::binary) << "JPEG";
            return command::FrameExportServices::Written{file, o.width, o.height, 4, 92};
        },
        command::FrameExportServices::Describer{}, std::vector<std::string>{"jpeg", "png"});
}

/// The telescope essay of the kit tests: two minutes, with repeated topic words.
inline std::vector<std::pair<double, std::string>> telescopeLines() {
    return {{0.5, "Welcome back, today I am building my own telescope at home."},
            {10.0, "A telescope needs a good mirror and a steady mount."},
            {20.0, "I bought a glass blank for the mirror from a local shop."},
            {30.0, "Grinding the mirror by hand took me three long weekends."},
            {40.0, "Next I built the tube and the mount for the telescope."},
            {50.0, "The mount has to be steady or the stars will shake."},
            {60.0, "Then came first light, the night I pointed it at the planets."},
            {70.0, "Jupiter and its moons looked sharp through the telescope."},
            {80.0, "Saturn and its rings were the best sight of the night."},
            {90.0, "If you want to build a telescope, start with a small mirror."},
            {100.0, "It costs less than you think and the planets are worth it."},
            {110.0, "Thanks for watching and see you next time."}};
}

inline std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace wabcut::test::autopilotfx
