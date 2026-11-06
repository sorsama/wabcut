// 0.7 (card 177): sources and citations for video essays. sources.add / update / remove / list (a source is a Source
// marker: claim = name, address = url, the reference on the marker), the uncited claims that stand out, sources.export
// (YouTube description block, numbered list, Markdown, APA bibliography) and sources.lowerThirds (a graphic per cited source
// on new "Sources" tracks), each an undo step of its own; the project file keeps the reference.

#include "CommandHarness.h"

#include <wabcut/command/SourceList.h>
#include <wabcut/model/ProjectDowngrade.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

namespace {

constexpr const char* kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
)";

/// Three claims: a cited one at 0:01, an uncited one at 0:02, and the same source cited again at 0:05 (a range).
void addSources(CommandHarness& h) {
    h.ok("sources.add", Json{{"time", tc(125)}, {"duration", tc(50)}, {"claim", "Unemployment fell 4 percent in 2023"},
                             {"url", "https://example.org/report"}, {"title", "Labour Market Report"}, {"author", "Author, A."},
                             {"publisher", "Example Statistics"}, {"date", "2024-02-01"},
                             {"archive", "https://web.archive.org/web/2024/https://example.org/report"}});
    h.ok("sources.add", Json{{"time", tc(25)}, {"claim", "Most people agree"}});
    h.ok("sources.add", Json{{"time", tc(50)}, {"claim", "Wages rose too"}, {"url", "https://example.org/report"},
                             {"title", "Labour Market Report"}, {"author", "Author, A."}, {"publisher", "Example Statistics"},
                             {"date", "2024-02-01"}});
    h.ok("sources.add", Json{{"time", tc(75)}, {"claim", "A different study"}, {"title", "Zebra Counts"}, {"author", "Zed, B."},
                             {"date", "2022"}, {"url", "https://example.com/zebra"}});
}

} // namespace

TEST_CASE("sources.add makes a Source marker with the claim as its name; a claim without a source is marked as needed",
          "[command][sources]") {
    CommandHarness h(kPicture);
    const auto undoBefore = h.session.undoCount();
    const auto cited = h.ok("sources.add", Json{{"time", tc(50)}, {"claim", "Unemployment fell 4 percent"},
                                               {"url", "https://example.org/report"}, {"title", "Report"}});
    CHECK(h.session.undoCount() == undoBefore + 1);
    CHECK(cited.data["needed"] == false);
    const Sequence& seq = h.sequence(h.state());
    REQUIRE(seq.markers.size() == 1);
    const Marker& m = seq.markers.front();
    CHECK(m.type == MarkerType::Source);
    CHECK(m.name == "Unemployment fell 4 percent");
    CHECK(m.url == "https://example.org/report");
    REQUIRE(m.citation);
    CHECK(m.citation->title == "Report");
    CHECK_FALSE(m.citation->needed);
    CHECK(m.colorIndex == 6); // blue: cited

    const auto uncited = h.ok("sources.add", Json{{"time", tc(10)}, {"claim", "Everyone knows this"}});
    CHECK(uncited.data["needed"] == true);
    const Sequence& after = h.sequence(h.state());
    REQUIRE(after.markers.size() == 2);
    CHECK(after.markers.front().name == "Everyone knows this"); // markers stay sorted by time
    CHECK(after.markers.front().citation->needed);
    CHECK(after.markers.front().colorIndex == 1); // red: still needs a source
    CHECK(uncited.data["index"] == 0);

    // Refusals change nothing.
    const ProjectSnapshot now = h.state();
    CHECK(h.errorOf("sources.add", Json{{"time", tc(5)}}) == "InvalidParams");                                           // no claim
    CHECK(h.errorOf("sources.add", Json{{"time", tc(5)}, {"claim", "x"}, {"url", "ftp://example.org"}}) == "InvalidParams"); // not http(s)
    CHECK(h.errorOf("sources.add", Json{{"time", tc(5)}, {"claim", "x"}, {"archive", "file:///etc/passwd"}}) == "InvalidParams");
    CHECK(h.errorOf("sources.add", Json{{"time", tc(5)}, {"claim", "x"}, {"title", std::string(2000, 'a')}}) == "InvalidParams");
    CHECK(*h.state() == *now);
}

TEST_CASE("sources.update gives a claim its source, moves it, and sources.remove takes it away", "[command][sources]") {
    CommandHarness h(kPicture);
    h.ok("sources.add", Json{{"time", tc(10)}, {"claim", "Everyone knows this"}});
    CHECK(h.sequence(h.state()).markers.front().citation->needed);

    const auto undoBefore = h.session.undoCount();
    h.ok("sources.update", Json{{"index", 0}, {"url", "https://example.org/a"}, {"title", "A study"}});
    CHECK(h.session.undoCount() == undoBefore + 1);
    const Marker& settled = h.sequence(h.state()).markers.front();
    CHECK_FALSE(settled.citation->needed); // its first source settles it
    CHECK(settled.citation->title == "A study");
    CHECK(settled.url == "https://example.org/a");
    CHECK(settled.name == "Everyone knows this");

    h.ok("sources.update", Json{{"index", 0}, {"needed", true}, {"time", tc(30)}, {"note", "check the number"}});
    const Marker& again = h.sequence(h.state()).markers.front();
    CHECK(again.citation->needed);
    CHECK(again.start == f(30));
    CHECK(again.comment == "check the number");
    CHECK(again.citation->title == "A study"); // fields not given stay

    // A marker that is not a source is not touched by these commands.
    h.ok("marker.add", Json{{"time", tc(60)}, {"name", "Chapter"}, {"type", "chapter"}});
    const std::size_t chapter = 1;
    CHECK(h.errorOf("sources.update", Json{{"index", chapter}, {"title", "x"}}) == "NotFound");
    CHECK(h.errorOf("sources.remove", Json{{"index", chapter}}) == "NotFound");
    CHECK(h.errorOf("sources.remove", Json{{"index", 9}}) == "NotFound");
    h.ok("sources.remove", Json{{"index", 0}});
    CHECK(h.sequence(h.state()).markers.size() == 1);
    CHECK(h.sequence(h.state()).markers.front().type == MarkerType::Chapter);
}

TEST_CASE("sources.list gives the sources by time with the uncited claims counted", "[command][sources]") {
    CommandHarness h(kPicture);
    addSources(h);
    h.ok("marker.add", Json{{"time", tc(20)}, {"name", "Not a source"}});
    const auto all = h.ok("sources.list", Json::object());
    CHECK(all.data["count"] == 4);
    CHECK(all.data["needed"] == 1);
    CHECK(all.data["cited"] == 3);
    REQUIRE(all.data["sources"].size() == 4);
    CHECK(all.data["sources"][0]["claim"] == "Most people agree");
    CHECK(all.data["sources"][0]["number"] == 1);
    CHECK(all.data["sources"][0]["needed"] == true);
    CHECK(all.data["sources"][1]["claim"] == "Wages rose too");
    CHECK(all.data["sources"][3]["claim"] == "Unemployment fell 4 percent in 2023");
    CHECK(all.data["sources"][3]["start"]["timecode"] == "00:00:05:00");
    CHECK(all.data["sources"][3]["archive"] == "https://web.archive.org/web/2024/https://example.org/report");
    const auto onlyNeeded = h.ok("sources.list", Json{{"onlyNeeded", true}});
    REQUIRE(onlyNeeded.data["sources"].size() == 1);
    CHECK(onlyNeeded.data["sources"][0]["claim"] == "Most people agree");
}

TEST_CASE("sources.export writes the YouTube block, the numbered list, Markdown and the APA bibliography", "[command][sources]") {
    CommandHarness h(kPicture);
    addSources(h);

    const auto youtube = h.ok("sources.export", Json{{"format", "youtube"}});
    const std::string yt = youtube.data["text"].get<std::string>();
    CHECK(yt.rfind("Sources\n", 0) == 0);
    CHECK(yt.find("0:02 Wages rose too - Labour Market Report, Author, A., Example Statistics, 2024-02-01 https://example.org/report\n") !=
          std::string::npos);
    CHECK(yt.find("0:03 A different study - Zebra Counts, Zed, B., 2022 https://example.com/zebra\n") != std::string::npos);
    CHECK(yt.find("0:05 Unemployment fell 4 percent in 2023") != std::string::npos);
    CHECK(yt.find("Most people agree") == std::string::npos); // uncited: named in the warnings, not listed
    CHECK(youtube.data["cited"] == 3);
    CHECK(youtube.data["uncited"] == 1);
    REQUIRE(youtube.warnings.size() == 1);
    CHECK(youtube.warnings[0].find("1 claim still needs a source: 0:01 Most people agree") != std::string::npos);

    // With the uncited claims listed too: marked.
    const auto withUncited = h.ok("sources.export", Json{{"format", "youtube"}, {"includeUncited", true}});
    CHECK(withUncited.data["text"].get<std::string>().find("0:01 Most people agree [source needed]\n") != std::string::npos);

    const std::string plain = h.ok("sources.export", Json{{"format", "plain"}}).data["text"].get<std::string>();
    CHECK(plain.rfind("1. [0:02] Wages rose too - ", 0) == 0);
    CHECK(plain.find("3. [0:05] Unemployment fell 4 percent in 2023 - ") != std::string::npos);
    CHECK(plain.find("(archive: https://web.archive.org/web/2024/https://example.org/report)") != std::string::npos);

    const std::string md = h.ok("sources.export", Json{{"format", "markdown"}}).data["text"].get<std::string>();
    CHECK(md.rfind("# Sources\n\n", 0) == 0);
    CHECK(md.find("**0:02** Wages rose too - [Labour Market Report](https://example.org/report), Author, A.") != std::string::npos);
    CHECK(md.find("([archive](https://web.archive.org/") != std::string::npos);

    // APA: one entry per distinct source (the report cited twice once), alphabetical by author, the date in APA form.
    const std::string apa = h.ok("sources.export", Json{{"format", "apa"}}).data["text"].get<std::string>();
    CHECK(apa.rfind("References\n\n", 0) == 0);
    const std::string first = "Author, A. (2024, February 1). Labour Market Report. Example Statistics. https://example.org/report\n";
    const std::string second = "Zed, B. (2022). Zebra Counts. https://example.com/zebra\n";
    const auto at = apa.find(first);
    REQUIRE(at != std::string::npos);
    CHECK(apa.find(first, at + 1) == std::string::npos); // listed once
    CHECK(apa.find(second) > at);

    CHECK(h.errorOf("sources.export", Json{{"format", "bibtex"}}) == "InvalidParams");
}

TEST_CASE("Source formats: dates, timestamps and a source without an author", "[command][sources]") {
    using namespace wabcut::command;
    CHECK(clockText(Ticks::fromSeconds(0)) == "0:00");
    CHECK(clockText(Ticks::fromSeconds(65)) == "1:05");
    CHECK(clockText(Ticks::fromSeconds(3723)) == "1:02:03");
    CHECK(sourceFormatFromName("YouTube") == SourceFormat::YouTube);
    CHECK(sourceFormatFromName("md") == SourceFormat::Markdown);
    CHECK_FALSE(sourceFormatFromName("pdf"));

    SourceEntry noAuthor;
    noAuthor.url = "https://example.org/x";
    noAuthor.citation.title = "An Untitled Page";
    noAuthor.claim = "A claim";
    SourceEntry onlyAddress;
    onlyAddress.url = "https://example.org/y";
    onlyAddress.claim = "Another";
    onlyAddress.start = Ticks::fromSeconds(9);
    const SourceEntry list[] = {noAuthor, onlyAddress};
    const std::string apa = formatSources(list, SourceFormat::Apa).text;
    CHECK(apa.find("An Untitled Page. (n.d.). https://example.org/x\n") != std::string::npos); // no author: the title leads
    CHECK(apa.find("https://example.org/y. (n.d.).") != std::string::npos);
    CHECK(citationCaption(noAuthor) == "Source: An Untitled Page");
    noAuthor.citation.author = "Someone";
    CHECK(citationCaption(noAuthor) == "Source: An Untitled Page - Someone");
    CHECK(formatSources({}, SourceFormat::YouTube).text.empty());
}

TEST_CASE("sources.lowerThirds shows each cited source as a graphic over its claim, in one undo step", "[command][sources]") {
    CommandHarness h(kPicture);
    addSources(h);
    const auto undoBefore = h.session.undoCount();
    const auto made = h.ok("sources.lowerThirds", Json{{"seconds", 3.0}});
    CHECK(h.session.undoCount() == undoBefore + 1);
    CHECK(made.data["clips"].size() == 3);   // three cited sources
    CHECK(made.data["skippedUncited"] == 1); // the claim without a source shows nothing
    const Sequence& seq = h.sequence(h.state());
    REQUIRE(seq.videoTracks.size() >= 2); // V1 and the "Sources" tracks (sources that overlap in time need more than one)
    std::map<std::int64_t, const Clip*> byStart;
    for (std::size_t i = 1; i < seq.videoTracks.size(); ++i) {
        CHECK(seq.videoTracks[i]->name == "Sources");
        for (const Clip& clip : seq.videoTracks[i]->clips) {
            REQUIRE(clip.graphic());
            byStart[clip.start().value] = &clip;
        }
    }
    REQUIRE(byStart.size() == 3);
    const Clip& first = *byStart.at(f(50).value);
    CHECK(first.graphic()->layers.front().text.text() == "Source: Labour Market Report - Author, A.");
    CHECK(first.duration() == f(75)); // a point source: the 3 seconds asked for
    CHECK(byStart.at(f(75).value)->graphic()->layers.front().text.text() == "Source: Zebra Counts - Zed, B.");
    CHECK(byStart.at(f(125).value)->duration() == f(50)); // the ranged source keeps its own length
    h.session.undo();
    CHECK(h.sequence(h.state()).videoTracks.size() == 1);

    // Nothing cited, nothing to show.
    CommandHarness bare(kPicture);
    bare.ok("sources.add", Json{{"time", tc(10)}, {"claim", "Everyone knows this"}});
    CHECK(bare.errorOf("sources.lowerThirds", Json::object()) == "EditRejected");
}

TEST_CASE("A source is saved with the project; an older Wabcut gets a comment marker named after the claim",
          "[command][sources][io]") {
    CommandHarness h(kPicture);
    addSources(h);
    const auto text = io::toJson(*h.state());
    REQUIRE(text);
    CHECK(text->find("\"citation\"") != std::string::npos);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    const auto entries = command::sourceEntries(*loaded->findSequence(h.tl.seq));
    REQUIRE(entries.size() == 4);
    CHECK(entries[0].claim == "Most people agree");
    CHECK(entries[0].needed());
    CHECK(entries[3].citation.archive == "https://web.archive.org/web/2024/https://example.org/report");
    CHECK(entries[3].citation.publisher == "Example Statistics");
    CHECK(entries[3].duration == f(50));

    for (const char* release : {"0.6", "0.5"}) {
        INFO(release);
        const auto down = io::toJsonForRelease(*h.state(), release);
        REQUIRE(down);
        CHECK(down->json.find("\"citation\"") == std::string::npos);
        const auto old = io::fromJson(down->json);
        REQUIRE(old);
        const Sequence& seq = *old->findSequence(h.tl.seq);
        REQUIRE(seq.markers.size() == 4);
        for (const Marker& m : seq.markers) {
            CHECK(m.type == MarkerType::Comment);
            CHECK_FALSE(m.name.empty()); // the claim stays as the marker's name
            CHECK_FALSE(m.citation);
        }
        bool listed = false;
        for (const io::DowngradeLoss& loss : down->losses) {
            listed = listed || (loss.id == "citations" && loss.count == 4);
        }
        CHECK(listed);
    }
}
