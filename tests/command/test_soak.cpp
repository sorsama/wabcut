// C.10 soak test ([perf]: `ctest -L perf`; left out of the quick run): 100,000 random edit commands through the
// command engine - parameters drawn from the current project (real clips, times on and around them), so most are
// valid edits and the rest exercise the refusals - with undo and redo interleaved. After every step: an accepted
// command leaves a project that passes validate(Strict) and saves; a refused one changes nothing; every undo and redo
// lands exactly on the state recorded for it. In rounds of 1,000 commands from the same picture, undo-all returns exactly
// to the round's first state and redo-all to its last (the session keeps 1,100 undo steps; an editor keeps 500).

#include "CommandHarness.h"

#include <wabcut/model/ProjectIO.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr std::string_view kPicture = R"(
    V3     |....................................................|
    V2     |......Dddddddd..........Eeeeeeee....................|
    V1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc....Ffffffffff.........|
    A1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc....Ffffffffff.........|
    A2     |......Dddddddd..........Eeeeeeee....................|
)";

struct ClipInfo {
    ClipId id;
    bool video = false;
    std::int64_t start = 0; // frames
    std::int64_t end = 0;
};

class Generator {
public:
    explicit Generator(std::uint64_t seed) : m_rng(seed) {}

    /// A command for the project as it is: {"command", "params"}.
    Json next(const ProjectState& state, SequenceId seq, const Timeline& tl) {
        const Sequence& s = *state.findSequence(seq);
        std::vector<ClipInfo> clips;
        s.forEachTrack([&](const Track& t) {
            for (const Clip& c : t.clips) {
                clips.push_back({c.id, t.kind == TrackKind::Video, frameOf(c.start()), frameOf(c.end())});
            }
        });
        const std::int64_t end = std::max<std::int64_t>(frameOf(s.contentEnd()), 10);
        const auto clip = [&]() -> const ClipInfo& { return clips[pick(clips.size())]; };
        const auto time = [&] { return tc(static_cast<int>(pick(static_cast<std::size_t>(end + 10)))); };
        const auto delta = [&] {
            const int d = static_cast<int>(pick(8)) + 1;
            return pick(2) == 0 ? tc(d) : "-" + tc(d);
        };
        const auto inside = [&](const ClipInfo& c) { return tc(static_cast<int>(c.start + 1 + pick(static_cast<std::size_t>(std::max<std::int64_t>(c.end - c.start - 1, 1))))); };
        const auto edge = [&] { return pick(2) == 0 ? "head" : "tail"; };
        Json params = Json::object();
        std::string name;
        if (clips.size() < 2) {
            name = "clip.overwrite";
            params = {{"at", time()}, {"media", tl.media(static_cast<char>('A' + pick(6))).value},
                      {"sourceIn", tc(static_cast<int>(pick(100)))}, {"sourceOut", tc(static_cast<int>(100 + pick(50)))}};
        } else {
            const ClipInfo& c = clip();
            switch (pick(27)) {
            case 0: name = "clip.trim"; params = {{"clip", c.id.value}, {"edge", edge()}, {"delta", delta()}, {"linked", pick(2) == 0}}; break;
            case 1: name = "clip.rippleTrim"; params = {{"clip", c.id.value}, {"edge", edge()}, {"delta", delta()}}; break;
            case 2: name = "clip.roll"; params = {{"clip", c.id.value}, {"edge", edge()}, {"delta", delta()}}; break;
            case 3: name = "clip.slide"; params = {{"clip", c.id.value}, {"delta", delta()}}; break;
            case 4: name = "clip.slip"; params = {{"clips", {c.id.value}}, {"delta", delta()}}; break;
            case 5:
                name = "clip.move";
                params = {{"clips", {c.id.value}}, {"delta", delta()}, {"mode", pick(2) == 0 ? "overwrite" : "insert"}};
                if (c.video && pick(3) == 0) {
                    params["videoTracks"] = static_cast<int>(pick(3)) - 1;
                }
                break;
            case 6: name = "clip.enable"; params = {{"clips", {c.id.value}}, {"enabled", pick(2) == 0}}; break;
            case 7: name = "clip.rename"; params = {{"clips", {c.id.value}}, {"name", "Take " + std::to_string(pick(100))}}; break;
            case 8: name = "clip.setColor"; params = {{"clips", {c.id.value}}, {"colorIndex", pick(16)}}; break;
            case 9: name = "clip.link"; params = {{"clips", {c.id.value, clip().id.value}}}; break;
            case 10: name = "clip.unlink"; params = {{"clips", {c.id.value}}}; break;
            case 11: {
                static constexpr double kSpeeds[] = {25.0, 50.0, 100.0, 200.0, 400.0};
                name = "clip.speedDuration";
                params = {{"clips", {c.id.value}}, {"speed", kSpeeds[pick(5)]}, {"ripple", pick(2) == 0}};
                break;
            }
            case 12: name = "clip.addEffect"; params = {{"clips", {c.id.value}}, {"effect", "wabcut.blur.gaussian"}}; break;
            case 13:
                name = "clip.addKeyframe";
                params = {{"clip", c.id.value}, {"param", c.video ? "opacity.opacity" : "volume.level"}, {"time", inside(c)},
                          {"value", c.video ? 0.25 * static_cast<double>(pick(5)) : -6.0}};
                break;
            case 14: name = "clip.fade"; params = {{"clips", {c.id.value}}, {"duration", tc(static_cast<int>(1 + pick(4)))}, {"edge", pick(2) == 0 ? "in" : "out"}}; break;
            case 15: name = "timeline.rippleDelete"; params = {{"clips", {c.id.value}}}; break;
            case 16: name = "edit.clear"; params = {{"clips", {c.id.value}}}; break;
            case 17: name = "sequence.addEdit"; params = {{"time", inside(c)}, {"tracks", {c.video ? "V1" : "A1"}}}; break;
            case 18: name = "sequence.addEditToAllTracks"; params = {{"time", time()}}; break;
            case 19: name = "marker.add"; params = {{"time", time()}, {"name", "M" + std::to_string(pick(1000))}}; break;
            case 20: name = "marker.remove"; params = {{"index", pick(4)}}; break;
            case 21: name = "transition.add"; params = {{"clip", c.id.value}, {"edge", edge()}, {"duration", tc(static_cast<int>(2 + pick(4)))}}; break;
            case 22:
                name = pick(2) == 0 ? "clip.overwrite" : "clip.insert";
                params = {{"at", time()}, {"media", tl.media(static_cast<char>('A' + pick(6))).value},
                          {"sourceIn", tc(static_cast<int>(pick(100)))}, {"sourceOut", tc(static_cast<int>(100 + pick(12)))}};
                break;
            case 23: {
                const int in = static_cast<int>(pick(static_cast<std::size_t>(end)));
                name = pick(2) == 0 ? "sequence.lift" : "sequence.extract";
                params = {{"in", tc(in)}, {"out", tc(in + 1 + static_cast<int>(pick(6)))}};
                break;
            }
            case 24: name = "sequence.nest"; params = {{"clips", {c.id.value}}}; break;
            case 25: name = "review.add"; params = {{"time", time()}, {"text", "Check this"}}; break;
            default: name = "clip.frameHold"; params = {{"clip", c.id.value}, {"time", inside(c)}, {"duration", tc(static_cast<int>(2 + pick(4)))}}; break;
            }
        }
        params["sequence"] = seq.value;
        return Json{{"command", name}, {"params", std::move(params)}};
    }

    std::size_t pick(std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t>(m_rng() % n); }

private:
    static std::int64_t frameOf(Ticks t) { return t.value / kRate.ticksPerUnit(); }
    std::mt19937_64 m_rng;
};

struct Stats {
    std::size_t accepted = 0;
    std::size_t refused = 0;
    std::size_t undos = 0;
    std::size_t redos = 0;
    std::size_t rounds = 0;
    std::map<std::string, std::pair<std::size_t, std::size_t>> perCommand; // accepted, refused
};

} // namespace

TEST_CASE("Soak: 100,000 random commands with undo and redo keep every invariant", "[command][perf][soak]") {
    constexpr std::size_t kCommands = 100'000;
    constexpr std::size_t kRound = 1'000;
    Timeline tl(kPicture);
    tl.linkMatching();
    const ProjectState initial = tl.state;
    EditSessionOptions options;
    options.undoLimit = kRound + 100;
    options.validateEdits = false; // checked here, after every step
    EditSession session(initial, options);
    command::CommandEngine engine(session, command::builtinCommands(), command::EnginePolicy{});
    Generator gen(0xC10);
    Stats stats;
    // The states the session must show: [0] the round's first, [i] after the i-th change; `at` is the current one.
    std::vector<ProjectSnapshot> states{session.snapshot()};
    std::size_t at = 0;
    const auto expectAt = [&](const char* what) {
        if (!(*session.snapshot() == *states[at])) {
            FAIL(what << " does not land on the recorded state (round " << stats.rounds << ", step " << at << ")");
        }
    };
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 1; i <= kCommands; ++i) {
        const std::size_t roll = gen.pick(100);
        if (roll < 8 && at > 0) {
            session.undo();
            --at;
            ++stats.undos;
            expectAt("undo");
        } else if (roll < 12 && at + 1 < states.size()) {
            session.redo();
            ++at;
            ++stats.redos;
            expectAt("redo");
        } else {
            const Json request = gen.next(*session.snapshot(), tl.seq, tl);
            const std::string name = request["command"].get<std::string>();
            const ProjectSnapshot before = session.snapshot();
            const std::uint64_t revision = session.revision();
            const command::CommandResult result = engine.executeJson(request);
            if (!result.ok()) {
                ++stats.refused;
                ++stats.perCommand[name].second;
                if (session.snapshot() != before) {
                    FAIL("a refused " << name << " changed the project: " << request.dump());
                }
            } else {
                ++stats.accepted;
                ++stats.perCommand[name].first;
                if (session.revision() != revision) {
                    const auto valid = validate(*session.snapshot(), Validation::Strict);
                    if (!valid) {
                        FAIL(name << " left an invalid project: " << valid.error().message << "\n" << request.dump());
                    }
                    if (!io::toJson(*session.snapshot())) {
                        FAIL(name << " left a project that does not save: " << request.dump());
                    }
                    states.resize(at + 1);
                    states.push_back(session.snapshot());
                    ++at;
                }
            }
        }
        if (i % kRound == 0) {
            // Undo all the way to the round's first state, redo all the way to its last, then go on from there.
            const std::size_t last = states.size() - 1;
            while (at > 0) {
                REQUIRE(session.canUndo());
                session.undo();
                --at;
                expectAt("undo-all");
            }
            REQUIRE(*session.snapshot() == *states.front());
            while (at < last) {
                REQUIRE(session.canRedo());
                session.redo();
                ++at;
                expectAt("redo-all");
            }
            ++stats.rounds;
            // Each round starts again from the picture: 1,000 random edits grow the project (nested sequences, markers,
            // inserted clips), and a project that grows for 100,000 steps measures its size, not the edits.
            session.reset(initial);
            session.releasePending(); // what the app does between frames
            states = {session.snapshot()};
            at = 0;
            if (stats.rounds % 10 == 0) {
                std::printf("soak: %zu rounds, %.1f s\n", stats.rounds,
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
                std::fflush(stdout);
            }
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("soak: %zu commands (%zu accepted, %zu refused), %zu undos, %zu redos, %zu rounds of undo-all/redo-all "
                "in %.1f s (%.0f us a step)\n",
                stats.accepted + stats.refused, stats.accepted, stats.refused, stats.undos, stats.redos, stats.rounds,
                seconds, 1e6 * seconds / static_cast<double>(kCommands));
    for (const auto& [name, counts] : stats.perCommand) {
        std::printf("  %-28s %6zu accepted %6zu refused\n", name.c_str(), counts.first, counts.second);
    }
    CHECK(stats.rounds == kCommands / kRound);
    CHECK(stats.accepted > stats.refused); // mostly valid edits
}
