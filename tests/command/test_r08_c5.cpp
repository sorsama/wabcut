// Stream C, batch C5 through the engine: Consolidate Duplicates (card 025), the rights and clearance record with its log,
// the Media Usage Report's rights columns and the credits roll (card 179).

#include "CommandHarness.h"

#include <wabcut/model/Rights.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

namespace {

/// Gives media `letter` of `tl` a file identity: items with one hash are the same file.
void setFile(Timeline& tl, char letter, const std::string& hash) {
    for (MediaPtr& m : tl.state.media) {
        if (m->id == tl.media(letter)) {
            auto item = std::make_shared<MediaItem>(*m);
            item->locator.path = "D:/Footage/" + item->name + ".mp4";
            item->locator.fingerprint = SourceFingerprint{9'000'000, 1, hash};
            m = item;
        }
    }
}

void rename(Timeline& tl, char letter, const std::string& name) {
    const MediaId id = tl.media(letter); // (before the name changes: the fixture finds media by its letter-name)
    for (MediaPtr& m : tl.state.media) {
        if (m->id == id) {
            auto item = std::make_shared<MediaItem>(*m);
            item->name = name;
            m = item;
        }
    }
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        out.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    return out;
}

} // namespace

TEST_CASE("project.consolidateDuplicates merges in one undo step and a dry run changes nothing (025)", "[command][v07][library]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Cccc|
    )");
    setFile(tl, 'A', "aaaaaaaaaaaaaaaa");
    setFile(tl, 'B', "aaaaaaaaaaaaaaaa");
    setFile(tl, 'C', "cccccccccccccccc");
    CommandHarness h(std::move(tl));
    const std::size_t before = h.session.undoCount();
    const MediaId a = h.tl.media('A');
    const MediaId b = h.tl.media('B');

    // A dry run reports the plan and leaves the project and the undo history alone.
    const auto dry = h.run(Json{{"command", "project.consolidateDuplicates"}, {"params", Json::object()}, {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK(dry.dryRun);
    CHECK(h.state()->findMedia(b) != nullptr);
    CHECK(h.session.undoCount() == before);
    CHECK(dry.data["removed"] == 1);

    const auto merged = h.ok("project.consolidateDuplicates");
    CHECK(h.session.undoCount() == before + 1);
    CHECK(merged.data["removed"] == 1);
    CHECK(merged.data["repointedClips"] == 1);
    REQUIRE(merged.data["groups"].size() == 1);
    CHECK(merged.data["groups"][0]["keeper"] == a.value);
    CHECK(merged.data["groups"][0]["removed"][0] == b.value);
    CHECK(h.state()->findMedia(b) == nullptr);
    CHECK(h.state()->findMedia(h.tl.media('C')) != nullptr); // another file: untouched
    CHECK(h.sequence(h.state()).findClip(h.clip("V1@6"))->clip().media == a);

    // Nothing left to merge: not an undo step.
    const auto again = h.ok("project.consolidateDuplicates");
    CHECK(again.data["removed"] == 0);
    CHECK(h.session.undoCount() == before + 1);

    REQUIRE(h.session.undo());
    CHECK(h.state()->findMedia(b) != nullptr);
    CHECK(h.sequence(h.state()).findClip(h.clip("V1@6"))->clip().media == b);
}

TEST_CASE("project.consolidateDuplicates can be limited to some items and reports what stayed (025)", "[command][v07][library]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Cccc..Dddd|
    )");
    setFile(tl, 'A', "1111111111111111");
    setFile(tl, 'B', "1111111111111111");
    setFile(tl, 'C', "2222222222222222");
    setFile(tl, 'D', "2222222222222222");
    for (MediaPtr& m : tl.state.media) {
        if (m->id == tl.media('D')) {
            auto item = std::make_shared<MediaItem>(*m);
            item->scaling = MediaScaling::SetToFill; // read differently: stays
            m = item;
        }
    }
    CommandHarness h(std::move(tl));
    const auto only = h.ok("project.consolidateDuplicates", Json{{"media", Json::array({h.tl.media('C').value})}});
    CHECK(only.data["removed"] == 0); // C and D differ in how they are read
    REQUIRE(only.data["groups"].size() == 1);
    REQUIRE(only.data["groups"][0]["skipped"].size() == 1);
    CHECK(only.data["groups"][0]["skipped"][0]["name"] == h.state()->findMedia(h.tl.media('D'))->name);
    CHECK_FALSE(only.warnings.empty());
    CHECK(h.state()->findMedia(h.tl.media('B')) != nullptr); // A and B are another group: left alone

    CHECK(h.errorOf("project.consolidateDuplicates", Json{{"media", Json::array({99999})}}) == "NotFound");
}

TEST_CASE("media.setRights sets the record of several items in one undo step and refuses what is wrong (179)",
          "[command][v07][rights]") {
    CommandHarness h(R"(
        V1 tgt |Aaaa..Bbbb|
    )");
    const MediaId a = h.tl.media('A');
    const MediaId b = h.tl.media('B');
    const std::size_t before = h.session.undoCount();
    const auto set = h.ok("media.setRights", Json{{"media", Json::array({a.value, b.value})},
                                                  {"status", "pending"},
                                                  {"source", "Newsreel Archive"},
                                                  {"licence", "All media, 10 years"},
                                                  {"territory", "Worldwide"},
                                                  {"expiry", "2030-06-30"},
                                                  {"cost", "1,250.50"},
                                                  {"currency", "GBP"},
                                                  {"creditLine", "Archive footage courtesy of Newsreel Archive"}});
    CHECK(h.session.undoCount() == before + 1);
    CHECK(set.data["updated"] == 2);
    const MediaRights& rights = *h.state()->findMedia(a)->rights;
    CHECK(rights.status == ClearanceStatus::Pending);
    CHECK(rights.costMinor == 125050);
    CHECK(rights.currency == "GBP");
    CHECK(h.state()->findMedia(b)->rights == h.state()->findMedia(a)->rights);

    // Only the fields named change; "" clears; a number is an amount too; null clears the cost.
    h.ok("media.setRights", Json{{"media", Json::array({a.value})}, {"status", "cleared"}, {"cost", 99}});
    CHECK(h.state()->findMedia(a)->rights->status == ClearanceStatus::Cleared);
    CHECK(h.state()->findMedia(a)->rights->costMinor == 9900);
    CHECK(h.state()->findMedia(a)->rights->source == "Newsreel Archive");
    h.ok("media.setRights", Json{{"media", Json::array({a.value})}, {"cost", nullptr}, {"expiry", ""}});
    CHECK_FALSE(h.state()->findMedia(a)->rights->costMinor.has_value());
    CHECK(h.state()->findMedia(a)->rights->expiry.empty());

    // The checks.
    CHECK(h.errorOf("media.setRights", Json{{"media", Json::array({a.value})}, {"status", "maybe"}}) == "InvalidParams");
    CHECK(h.errorOf("media.setRights", Json{{"media", Json::array({a.value})}, {"cost", "lots"}}) == "InvalidParams");
    CHECK(h.errorOf("media.setRights", Json{{"media", Json::array({a.value})}, {"cost", -5}}) == "InvalidParams");
    CHECK(h.errorOf("media.setRights", Json{{"media", Json::array({a.value})}}) == "InvalidParams"); // nothing to set
    CHECK_FALSE(h.errorOf("media.setRights", Json{{"media", Json::array({a.value})}, {"expiry", "next year"}}).empty());
    CHECK_FALSE(h.errorOf("media.setRights", Json{{"media", Json::array({a.value})}, {"currency", "usd"}}).empty());
    CHECK(h.errorOf("media.setRights", Json{{"media", Json::array({99999})}, {"status", "cleared"}}) == "NotFound");
    // The refusals changed nothing.
    CHECK(h.state()->findMedia(a)->rights->status == ClearanceStatus::Cleared);
}

TEST_CASE("project.rightsLog lists the sources of the sequence with flags and a summary (179)", "[command][v07][rights]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Cccc..Aaaa|
    )");
    const MediaId a = tl.media('A'); // (the fixture finds media by its letter-name: take the ids before renaming)
    const MediaId b = tl.media('B');
    const MediaId c = tl.media('C');
    rename(tl, 'A', "Harbour 1931");
    rename(tl, 'B', "Parade, 1946");
    rename(tl, 'C', "=HYPERLINK(\"http://example.invalid\")");
    CommandHarness h(std::move(tl));
    h.ok("media.setRights", Json{{"media", Json::array({a.value})}, {"status", "cleared"}, {"source", "Newsreel"},
                                 {"expiry", "2026-12-15"}, {"cost", "400"}, {"currency", "USD"}, {"creditLine", "Courtesy of Newsreel"}});
    h.ok("media.setRights", Json{{"media", Json::array({b.value})}, {"status", "pending"}, {"source", "Pathe, Paris"},
                                 {"cost", "250"}, {"currency", "USD"}});
    // C: no record at all (the editor's own footage).

    const auto log = h.ok("project.rightsLog", Json{{"asOf", "2026-12-01"}});
    CHECK(log.data["rows"] == 3);
    const auto& rows = log.data["report"]["rows"];
    REQUIRE(rows.size() == 3);
    // In the order each source first appears.
    CHECK(rows[0]["media"] == "Harbour 1931");
    CHECK(rows[1]["media"] == "Parade, 1946");
    CHECK(rows[2]["media"] == h.state()->findMedia(c)->name);
    CHECK(rows[0]["uses"] == 2);
    CHECK(rows[0]["firstUse"] == "00:00:00:00");
    CHECK(rows[0]["statusLabel"] == "Cleared");
    CHECK(rows[0]["expiresSoon"] == true); // 14 days after the delivery day
    CHECK(rows[0]["daysLeft"] == 14);
    CHECK(rows[0]["flags"] == "Licence expires in 14 days");
    CHECK(rows[1]["uncleared"] == true);
    CHECK(rows[1]["flags"] == "Not cleared yet");
    CHECK(rows[2]["hasRecord"] == false);
    CHECK(rows[2]["uncleared"] == false); // no record is not "uncleared": it is the editor's own
    const auto& totals = log.data["report"]["totals"];
    CHECK(totals["items"] == 3);
    CHECK(totals["needAttention"] == 2);
    CHECK(totals["withoutRecord"] == 1);
    CHECK(totals["statuses"]["Cleared"] == 1);
    CHECK(totals["statuses"]["Pending"] == 1);
    REQUIRE(totals["costs"].size() == 1);
    CHECK(totals["costs"][0]["currency"] == "USD");
    CHECK(totals["costs"][0]["total"] == "650.00");
    CHECK_FALSE(log.warnings.empty());

    // The spreadsheet: one row per source, quoting, a guard against formulas, the summary and the cost totals.
    const auto csv = lines(log.data["text"].get<std::string>());
    REQUIRE(csv.size() > 6);
    CHECK(csv[0] == "Media Name,File Path,Clearance,Source,Licence,Reference,Territory,Expires,Cost,Currency,Credit Line,Notes,Uses,"
                    "First Used,Seconds Used,Flags");
    CHECK(csv[1].starts_with("Harbour 1931,"));
    CHECK(csv[1].find(",Cleared,Newsreel,") != std::string::npos);
    CHECK(csv[1].find(",400.00,USD,Courtesy of Newsreel,") != std::string::npos);
    CHECK(csv[2].starts_with("\"Parade, 1946\","));
    CHECK(csv[2].find("\"Pathe, Paris\"") != std::string::npos);
    CHECK(csv[3].starts_with("\"'=HYPERLINK(")); // a cell that starts with = is text, not a formula
    CHECK(csv[3].find("No rights record") != std::string::npos);
    bool summary = false;
    bool total = false;
    for (const std::string& line : csv) {
        summary = summary || line == "Needs attention on 2026-12-01,2";
        total = total || line == "USD,650.00,2";
    }
    CHECK(summary);
    CHECK(total);

    // Filters: only what needs attention; only the recorded; the whole project.
    const auto flagged = h.ok("project.rightsLog", Json{{"asOf", "2026-12-01"}, {"flaggedOnly", true}});
    CHECK(flagged.data["rows"] == 2);
    const auto recorded = h.ok("project.rightsLog", Json{{"asOf", "2026-12-01"}, {"includeUnrecorded", false}});
    CHECK(recorded.data["rows"] == 2);
    const auto expired = h.ok("project.rightsLog", Json{{"asOf", "2027-01-01"}});
    CHECK(expired.data["report"]["rows"][0]["expired"] == true);
    CHECK(expired.data["report"]["rows"][0]["flags"] == "Licence expired on 2026-12-15");
    const auto project = h.ok("project.rightsLog", Json{{"scope", "project"}, {"asOf", "2026-12-01"}, {"format", "json"}});
    CHECK(project.data["report"]["scope"] == "project");
    CHECK(Json::parse(project.data["text"].get<std::string>())["rows"].size() >= 3);

    CHECK(h.errorOf("project.rightsLog", Json{{"asOf", "tomorrow"}}) == "InvalidParams");
    CHECK(h.errorOf("project.rightsLog", Json{{"scope", "galaxy"}}) == "InvalidParams");
}

TEST_CASE("A subclip's rights are its master's and a record counts its cost once (179)", "[command][v07][rights]") {
    CommandHarness h(R"(
        V1 tgt |Aaaa|
    )");
    const MediaId a = h.tl.media('A');
    h.ok("media.setRights", Json{{"media", Json::array({a.value})}, {"status", "cleared"}, {"cost", "100"}, {"currency", "EUR"}});
    const auto sub = h.ok("media.makeSubclip", Json{{"media", a.value}, {"in", "00:00:00:00"}, {"out", "00:00:00:02"}, {"name", "Part"}});
    (void)sub;
    const auto log = h.ok("project.rightsLog", Json{{"scope", "project"}, {"asOf", "2026-12-01"}});
    CHECK(log.data["report"]["totals"]["costs"][0]["total"] == "100.00"); // the subclip's inherited record is not counted again
}

TEST_CASE("sequence.mediaUsage carries the rights columns and warns about uncleared sources (179)", "[command][v07][rights]") {
    CommandHarness h(R"(
        V1 tgt |Aaaa..Bbbb|
    )");
    const MediaId a = h.tl.media('A');
    h.ok("media.setRights", Json{{"media", Json::array({a.value})}, {"status", "pending"}, {"source", "Archive X"},
                                 {"licence", "Ten years"}, {"territory", "UK"}, {"cost", "50"}, {"currency", "GBP"},
                                 {"creditLine", "Courtesy of X"}});
    const auto usage = h.ok("sequence.mediaUsage", Json{{"asOf", "2026-12-01"}});
    const auto& uses = usage.data["report"]["uses"];
    REQUIRE(uses.size() == 2);
    CHECK(uses[0]["rights"]["status"] == "pending");
    CHECK(uses[0]["rights"]["uncleared"] == true);
    CHECK(uses[0]["rights"]["flags"] == "Not cleared yet");
    CHECK(uses[1]["rights"]["hasRecord"] == false);
    CHECK(usage.data["flaggedSources"] == 1);
    CHECK_FALSE(usage.warnings.empty());
    const auto csv = lines(usage.data["text"].get<std::string>());
    CHECK(csv[0].ends_with(",Clearance,Source,Licence,Territory,Expires,Cost,Credit Line,Rights Flags"));
    CHECK(csv[1].ends_with(",Pending,Archive X,Ten years,UK,,50.00 GBP,Courtesy of X,Not cleared yet"));
    CHECK(csv[2].ends_with(",,,,,,,,")); // B has no record
}

TEST_CASE("sequence.creditsRoll makes a rolling graphic of the credit lines in one undo step (179)", "[command][v07][rights]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb..Cccc..Aaaa|
    )");
    CommandHarness h(std::move(tl));
    const MediaId a = h.tl.media('A');
    const MediaId b = h.tl.media('B');
    const MediaId c = h.tl.media('C');
    // Nothing to credit yet.
    CHECK(h.errorOf("sequence.creditsRoll") == "EditRejected");
    h.ok("media.setRights", Json{{"media", Json::array({a.value})}, {"status", "cleared"}, {"creditLine", "Archive footage courtesy of Newsreel"}});
    h.ok("media.setRights", Json{{"media", Json::array({b.value})}, {"status", "cleared"}, {"creditLine", "  archive footage courtesy of   newsreel "}});
    h.ok("media.setRights", Json{{"media", Json::array({c.value})}, {"status", "pending"}, {"source", "Someone"}}); // no credit line

    const std::size_t before = h.session.undoCount();
    const auto roll = h.ok("sequence.creditsRoll", Json{{"heading", "Archive Material"}});
    CHECK(h.session.undoCount() == before + 1);
    REQUIRE(roll.data["credits"].size() == 1); // the same line twice (case and spacing) is one credit
    CHECK(roll.data["credits"][0] == "Archive footage courtesy of Newsreel");
    REQUIRE(roll.data["missingCreditLine"].size() == 1);
    CHECK(roll.data["missingCreditLine"][0]["mediaId"] == c.value);
    CHECK_FALSE(roll.warnings.empty());
    const ClipId clipId{roll.data["clip"].get<std::uint64_t>()};
    const auto found = h.sequence(h.state()).findClip(clipId);
    REQUIRE(found);
    const Graphic* graphic = found->clip().graphic();
    REQUIRE(graphic != nullptr);
    REQUIRE(graphic->roll.has_value());
    CHECK(graphic->roll->kind == RollKind::Roll);
    REQUIRE(graphic->layers.size() == 1);
    const std::string text = graphic->layers[0].text.text();
    CHECK(text.find("Archive Material") != std::string::npos);
    CHECK(text.find("Archive footage courtesy of Newsreel") != std::string::npos);
    // It starts where the cut ends, on the track that is free there.
    CHECK(found->clip().start() == f(22));
    REQUIRE(h.session.undo());
    CHECK_FALSE(h.sequence(h.state()).findClip(clipId).has_value());

    // Alphabetical order and an explicit length.
    h.ok("media.setRights", Json{{"media", Json::array({c.value})}, {"creditLine", "Aerial views by Zed Films"}});
    const auto sorted = h.ok("sequence.creditsRoll", Json{{"order", "alphabetical"}, {"duration", "00:00:12:00"}, {"heading", ""}});
    REQUIRE(sorted.data["credits"].size() == 2);
    CHECK(sorted.data["credits"][0] == "Aerial views by Zed Films");
    CHECK(sorted.data["missingCreditLine"].empty());
}
