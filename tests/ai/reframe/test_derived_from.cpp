// Sequence::derivedFrom (AI-9): where a reframed or shorts sequence came from. Optional and additive: a project without
// it saves byte-identically; it takes part in equality and diffs; it round-trips through the project file, which
// bounds it (untrusted input); a source that was deleted since is not an error.

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <functional>

using namespace wabcut;
using namespace wabcut::test;
using wabcut::io::fromJson;
using wabcut::io::toJson;

namespace {

ProjectState withDerivation(const ProjectState& state, SequenceId seq, std::optional<SequenceDerivation> derivation) {
    auto copy = std::make_shared<Sequence>(*state.sequencePtr(seq));
    copy->derivedFrom = std::move(derivation);
    return state.withSequence(copy);
}

} // namespace

TEST_CASE("A sequence records what it was derived from, saved only when set", "[ai][reframe][model]") {
    const Timeline t("V1 |Aaaaa|");
    const auto plain = toJson(t.state);
    REQUIRE(plain);
    CHECK(plain->find("derivedFrom") == std::string::npos); // absent: the file is unchanged

    const SequenceDerivation derivation{SequenceId{987654}, "reframe/1", R"({"aspect":"9:16","mode":"follow","smoothness":0.5})"};
    const ProjectState derived = withDerivation(t.state, t.seq, derivation);
    REQUIRE(validate(derived)); // a dangling source id is allowed: re-running reports "source missing"
    CHECK_FALSE(*derived.sequencePtr(t.seq) == *t.state.sequencePtr(t.seq));

    const ChangeSet changes = diff(t.state, derived);
    REQUIRE(changes.sequence(t.seq) != nullptr);
    CHECK(changes.sequence(t.seq)->settingsChanged);
    CHECK(diff(derived, derived).empty());

    const auto saved = toJson(derived);
    REQUIRE(saved);
    const auto json = nlohmann::json::parse(*saved);
    const auto& record = json["sequences"][0]["derivedFrom"];
    CHECK(record["source"] == 987654);
    CHECK(record["generator"] == "reframe/1");
    CHECK(record["params"]["aspect"] == "9:16"); // an object in the file, not a string
    auto loaded = fromJson(*saved);
    REQUIRE(loaded);
    REQUIRE(loaded->sequencePtr(t.seq)->derivedFrom);
    CHECK(*loaded->sequencePtr(t.seq)->derivedFrom == derivation);
    CHECK(*toJson(*loaded) == *saved);
    CHECK(*toJson(withDerivation(derived, t.seq, std::nullopt)) == *plain);
}

TEST_CASE("Project files with a bad derivation record are refused", "[ai][reframe][model]") {
    const Timeline t("V1 |Aaaaa|");
    const ProjectState derived = withDerivation(t.state, t.seq, SequenceDerivation{t.seq, "reframe/1", R"({"a":1})"});
    const auto saved = toJson(derived);
    REQUIRE(saved);
    const auto tampered = [&](const std::function<void(nlohmann::json&)>& change) {
        auto json = nlohmann::json::parse(*saved);
        change(json["sequences"][0]["derivedFrom"]);
        return fromJson(json.dump());
    };
    CHECK(tampered([](nlohmann::json&) {}));
    CHECK_FALSE(tampered([](nlohmann::json& d) { d["params"] = "not an object"; }));
    CHECK_FALSE(tampered([](nlohmann::json& d) { d["params"] = nlohmann::json::array(); }));
    CHECK_FALSE(tampered([](nlohmann::json& d) { d["generator"] = std::string(65, 'g'); }));
    CHECK_FALSE(tampered([](nlohmann::json& d) { d["params"] = nlohmann::json{{"big", std::string(20'000, 'x')}}; }));
    CHECK_FALSE(tampered([](nlohmann::json& d) { d["source"] = "seven"; }));
    CHECK_FALSE(tampered([](nlohmann::json& d) { d.erase("params"); }));

    // The in-memory state is bounded the same way.
    CHECK_FALSE(validate(withDerivation(t.state, t.seq, SequenceDerivation{t.seq, std::string(65, 'g'), "{}"})));
    CHECK_FALSE(validate(withDerivation(t.state, t.seq, SequenceDerivation{t.seq, "x", std::string(17'000, ' ')})));
}
