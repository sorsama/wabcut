// An extension's key-value store and the throttle of the editor events it follows (C.12).

#include "ScriptTestUtil.h"

#include <wabcut/script/Events.h>
#include <wabcut/script/Storage.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::script;
using namespace wabcut::script::test;
using namespace std::chrono_literals;

TEST_CASE("Extension storage holds JSON under short keys; within its bounds; across runs", "[script][storage]") {
    const fs::path dir = scratch("storage");
    const fs::path file = storageFileFor(dir, "com.example.store");
    CHECK(file.filename() == "com.example.store.json");
    {
        KeyValueStore store = KeyValueStore::load(file);
        CHECK(store.values().empty());
        REQUIRE(store.set("every", "downbeat").has_value());
        REQUIRE(store.set("recent", Json::array({1, 2, 3})).has_value());
        REQUIRE(store.set("every", "beat").has_value()); // replaced
        CHECK(store.dirty());
        REQUIRE(store.save().has_value());
        CHECK_FALSE(store.dirty());
    }
    KeyValueStore store = KeyValueStore::load(file);
    CHECK(store.get("every") == Json("beat"));
    CHECK(store.get("recent") == Json::array({1, 2, 3}));
    CHECK_FALSE(store.get("missing").has_value());
    CHECK(store.remove("recent"));
    CHECK_FALSE(store.remove("recent"));

    // Bounds: key length, value size, key count, total size.
    CHECK_FALSE(store.set("", 1).has_value());
    CHECK_FALSE(store.set(std::string(kMaxStorageKeyBytes + 1, 'k'), 1).has_value());
    CHECK_FALSE(store.set("big", std::string(kMaxStorageValueBytes, 'v')).has_value());
    KeyValueStore many;
    for (std::size_t i = 0; i < kMaxStorageKeys; ++i) {
        REQUIRE(many.set("k" + std::to_string(i), static_cast<int>(i)).has_value());
    }
    auto tooMany = many.set("one-more", 1);
    REQUIRE_FALSE(tooMany.has_value());
    CHECK(tooMany.error().message.find("keys") != std::string::npos);
    CHECK(many.set("k0", "replacing is fine").has_value());
    KeyValueStore full;
    const std::string chunk(kMaxStorageValueBytes - 16, 'x');
    std::size_t stored = 0;
    while (full.set("c" + std::to_string(stored), chunk).has_value()) {
        ++stored;
    }
    CHECK(stored * chunk.size() <= kMaxStorageBytes);
    CHECK(stored >= kMaxStorageBytes / kMaxStorageValueBytes - 1);

    // A damaged or hand-edited file loads what fits the bounds.
    writeFile(file, "{\"ok\": 1, \"" + std::string(kMaxStorageKeyBytes + 1, 'k') + "\": 2}");
    KeyValueStore edited = KeyValueStore::load(file);
    CHECK(edited.values().size() == 1);
    writeFile(file, "not json");
    CHECK(KeyValueStore::load(file).values().empty());
}

TEST_CASE("Editor events reach an extension at most once per interval; the latest winning", "[script][events]") {
    EventThrottle throttle;
    const auto t0 = EventThrottle::Clock::time_point{} + 1h;
    const auto at = [](int time) { return Event{EventKind::PlayheadMoved, Json{{"time", time}}, 0}; };
    // The first passes at once.
    auto first = throttle.offer(at(0), t0);
    REQUIRE(first.has_value());
    CHECK(first->data["time"] == 0);
    // A scrub: 20 more within 100 ms are held, each replacing the last.
    for (int i = 1; i <= 20; ++i) {
        CHECK_FALSE(throttle.offer(at(i), t0 + std::chrono::milliseconds(i * 4)).has_value());
    }
    REQUIRE(throttle.nextDue().has_value());
    CHECK(*throttle.nextDue() == t0 + 100ms);
    CHECK(throttle.due(t0 + 99ms).empty());
    auto due = throttle.due(t0 + 100ms);
    REQUIRE(due.size() == 1);
    CHECK(due[0].data["time"] == 20);   // the latest
    CHECK(due[0].coalesced == 19);      // the 19 it replaced
    CHECK_FALSE(throttle.nextDue().has_value());

    // Kinds are throttled apart.
    CHECK(throttle.offer(Event{EventKind::SelectionChanged, Json::object(), 0}, t0 + 101ms).has_value());
    CHECK_FALSE(throttle.offer(Event{EventKind::PlayheadMoved, Json::object(), 0}, t0 + 150ms).has_value());
    CHECK(throttle.due(t0 + 199ms).empty());
    CHECK(throttle.due(t0 + 200ms).size() == 1);
    // After a quiet interval an event passes at once again.
    CHECK(throttle.offer(Event{EventKind::PlayheadMoved, Json::object(), 0}, t0 + 400ms).has_value());

    // Saves wait 250 ms; intervals can be set.
    throttle.setInterval(EventKind::CommandApplied, 0ms);
    CHECK(throttle.offer(Event{EventKind::CommandApplied, Json::object(), 0}, t0 + 401ms).has_value());
    CHECK(throttle.offer(Event{EventKind::CommandApplied, Json::object(), 0}, t0 + 401ms).has_value());
    CHECK(throttle.offer(Event{EventKind::ProjectSaved, Json::object(), 0}, t0 + 401ms).has_value());
    CHECK_FALSE(throttle.offer(Event{EventKind::ProjectSaved, Json::object(), 0}, t0 + 600ms).has_value());
    CHECK(throttle.due(t0 + 651ms).size() == 1);

    for (const EventKind kind : {EventKind::SelectionChanged, EventKind::PlayheadMoved, EventKind::ProjectSaved,
                                 EventKind::CommandApplied}) {
        CHECK(eventKindFromString(toString(kind)) == kind);
    }
    CHECK_FALSE(eventKindFromString("fileOpened").has_value());
}
