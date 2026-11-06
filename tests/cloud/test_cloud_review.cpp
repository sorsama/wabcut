// Review links (docs/cloud.md): the static page and its CSP, publishing and revoking on every kind of store, the link records, and
// the comments reviewers send back - parsed as untrusted input and imported into the review notes in one step.

#include "FakeServers.h"
#include "TimelineFixture.h"

#include <wabcut/ai/Sha256.h>
#include <wabcut/cloud/HttpStores.h>
#include <wabcut/cloud/RemoteStore.h>
#include <wabcut/cloud/ReviewLink.h>
#include <wabcut/model/ReviewComments.h>
#include <wabcut/model/ReviewOps.h>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <regex>

using namespace wabcut;
using namespace wabcut::cloud;
using namespace wabcut::cloud::test;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::size_t countOf(const std::string& text, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

/// The text between the Nth "<script" ... ">" and "</script>".
std::string scriptBody(const std::string& html, std::size_t index) {
    std::size_t at = 0;
    for (std::size_t i = 0; i <= index; ++i) {
        at = html.find("<script", at == 0 ? 0 : at + 1);
        REQUIRE(at != std::string::npos);
    }
    const std::size_t open = html.find('>', at) + 1;
    return html.substr(open, html.find("</script>", open) - open);
}

ReviewPageInput samplePage() {
    ReviewPageInput in;
    in.title = "Client cut v2";
    in.linkId = publicLinkId("abcdefghijklmnopqrstuvwxyz");
    in.videoUrl = "review.mp4";
    in.frameRate = Rate{25, 1};
    in.durationFrames = 250;
    in.expiresAt = 1800000000;
    return in;
}

RandomBytes fixedRandom() {
    return [](std::size_t n) { return std::string(n, '\x2a'); };
}

} // namespace

TEST_CASE("The review page: nothing from outside runs nothing reaches out", "[cloud][review]") {
    ReviewPageInput in = samplePage();
    in.title = "</script><img src=x onerror=alert(1)>";
    in.instructions = "Please <b>watch</b> & comment <!-- \xE2\x80\xA8 -->";
    const ReviewPage page = buildReviewPage(in);

    // exactly two scripts (the data block and ours), and the injected markup is data, not markup
    CHECK(countOf(page.html, "<script") == 2);
    CHECK(countOf(page.html, "</script>") == 2);
    CHECK(page.html.find("<img") == std::string::npos);
    CHECK(page.html.find("onerror=alert") != std::string::npos); // (only inside the escaped data block and the escaped title)
    const std::string manifestText = scriptBody(page.html, 0);
    CHECK(manifestText.find('<') == std::string::npos);
    CHECK(manifestText.find('>') == std::string::npos);
    CHECK(manifestText.find("\xE2\x80\xA8") == std::string::npos);
    const json manifest = json::parse(manifestText);
    CHECK(manifest["title"] == in.title);
    CHECK(manifest["instructions"] == in.instructions);
    CHECK(manifest["linkId"] == in.linkId);
    CHECK(manifest["fpsNum"] == 25);
    CHECK(manifest["durationFrames"] == 250);
    CHECK(manifest["video"] == "review.mp4");

    // the title element is escaped
    CHECK(page.html.find("<title>&lt;/script&gt;&lt;img") != std::string::npos);

    // the CSP names our script and style by hash, allows no inline code and no connection
    const std::string script = scriptBody(page.html, 1);
    CHECK(page.scriptHash == "sha256-" + base64Encode([&] {
              ai::Sha256 h;
              h.update(script);
              const auto d = h.finish();
              return std::string(reinterpret_cast<const char*>(d.data()), d.size());
          }()));
    const std::size_t styleOpen = page.html.find("<style>") + 7;
    const std::string style = page.html.substr(styleOpen, page.html.find("</style>") - styleOpen);
    CHECK(page.styleHash == "sha256-" + base64Encode([&] {
              ai::Sha256 h;
              h.update(style);
              const auto d = h.finish();
              return std::string(reinterpret_cast<const char*>(d.data()), d.size());
          }()));
    CHECK(page.html.find("script-src &#39;" + page.scriptHash + "&#39;") != std::string::npos);
    CHECK(page.html.find("style-src &#39;" + page.styleHash + "&#39;") != std::string::npos);
    CHECK(page.html.find("default-src &#39;none&#39;") != std::string::npos);
    CHECK(page.html.find("connect-src &#39;none&#39;") != std::string::npos);
    CHECK(page.html.find("unsafe-inline") == std::string::npos);
    CHECK(page.html.find("unsafe-eval") == std::string::npos);
    CHECK(page.html.find("media-src &#39;self&#39; blob:") != std::string::npos);
    // the policy is the first thing after the character set
    CHECK(page.html.find("Content-Security-Policy") < page.html.find("<style>"));
    CHECK(page.html.find("Content-Security-Policy") < page.html.find("<title>"));

    // no inline style attributes (the style hash would block them), no handlers, no external resources
    CHECK(page.html.find(" style=") == std::string::npos);
    CHECK_FALSE(std::regex_search(page.html, std::regex(" on[a-z]+=\"")));
    CHECK(page.html.find("src=\"http") == std::string::npos);
    CHECK(page.html.find("href=\"http") == std::string::npos);
    CHECK(page.html.find("@import") == std::string::npos);
    CHECK(page.html.find("url(") == std::string::npos);
    CHECK(script.find("innerHTML") == std::string::npos);
    CHECK(script.find("eval(") == std::string::npos);
    CHECK(script.find("document.write") == std::string::npos);
    CHECK(script.find("XMLHttpRequest") == std::string::npos);
    CHECK(script.find("fetch(") == std::string::npos);

    // what the page writes is what the editor reads
    CHECK(script.find("'" + std::string(kReviewCommentsFormat) + "'") != std::string::npos);
    CHECK(script.find("version: " + std::to_string(kReviewCommentsVersion)) != std::string::npos);
    CHECK(script.find("requestVideoFrameCallback") != std::string::npos);
}

TEST_CASE("The review page: the video address folders and drop-frame", "[cloud][review]") {
    SECTION("an absolute video address is allowed as one origin, nothing else") {
        ReviewPageInput in = samplePage();
        in.videoUrl = "https://reviews.s3.eu-west-1.amazonaws.com/reviews/abc/review.mp4?X-Amz-Signature=deadbeef&X-Amz-Expires=3600";
        const ReviewPage page = buildReviewPage(in);
        CHECK(page.html.find("media-src &#39;self&#39; blob: https://reviews.s3.eu-west-1.amazonaws.com;") != std::string::npos);
        CHECK(page.html.find("file:") == std::string::npos);
        const json manifest = json::parse(scriptBody(page.html, 0));
        CHECK(manifest["video"] == in.videoUrl);
    }
    SECTION("a page meant for a folder also plays from file:") {
        ReviewPageInput in = samplePage();
        in.fromFiles = true;
        CHECK(buildReviewPage(in).html.find("blob: file:") != std::string::npos);
    }
    SECTION("the frame rate and drop-frame travel as numbers") {
        ReviewPageInput in = samplePage();
        in.frameRate = Rate{30000, 1001};
        in.dropFrame = true;
        const json manifest = json::parse(scriptBody(buildReviewPage(in).html, 0));
        CHECK(manifest["fpsNum"] == 30000);
        CHECK(manifest["fpsDen"] == 1001);
        CHECK(manifest["dropFrame"] == true);
    }
    SECTION("the public id is not the token") {
        const std::string token = "abcdefghijklmnopqrstuvwxyz";
        const std::string id = publicLinkId(token);
        CHECK(id.size() == 24);
        CHECK(id != token);
        CHECK(id == publicLinkId(token));
        CHECK(id != publicLinkId(token + "a"));
        CHECK(buildReviewPage(samplePage()).html.find(token) == std::string::npos);
    }
}

TEST_CASE("Publishing a review link then revoking it on every kind of store", "[cloud][review]") {
    TempFolder work("publish");
    writeAll(work.path / "copy.mp4", std::string("\0\0\0\x18" "ftypmp42", 12) + std::string(5000, 'v'));
    TempFolder folder("publish-store");
    MemoryStore memory;
    FolderStore disk(folder.path / "root");
    FakeS3 s3("reviews", "AKIATESTKEY0001", "secret-secret-secret-1234");
    S3Config sc;
    sc.endpoint = "https://s3.example.com";
    sc.bucket = "reviews";
    S3Store s3store(s3, sc, ai::Secret("AKIATESTKEY0001"), ai::Secret("secret-secret-secret-1234"), [] { return std::int64_t{1793874937}; });
    FakeDav dav("/dav/reviews", "ann", "password-1234");
    WebDavConfig dc;
    dc.url = "https://dav.example.com/dav/reviews";
    dc.user = "ann";
    WebDavStore webdav(dav, dc, ai::Secret("password-1234"));

    for (RemoteStore* store : std::vector<RemoteStore*>{&memory, &disk, &s3store, &webdav}) {
        INFO(store->kind());
        PublishRequest request;
        request.video = work.path / "copy.mp4";
        request.stagingFolder = work.path;
        request.page = samplePage();
        request.page.title = "Client cut v2";
        request.location = "loc-1";
        request.title = "Client cut v2";
        request.expiresAt = 1793874937 + 3 * 24 * 3600;
        request.sequence = 7;
        request.startTicks = 1000;
        request.durationTicks = 2000;
        request.contentHash = "abc123";
        if (store == &s3store) {
            request.presign = [&](std::string_view path, int seconds) { return s3store.presignGet(path, seconds); };
        }
        std::uint64_t reported = 0;
        auto published = publishReviewLink(*store, request, fixedRandom(), [] { return std::int64_t{1793874937}; },
                                           [&](std::uint64_t done, std::uint64_t) {
                                               reported = std::max(reported, done);
                                               return true;
                                           });
        REQUIRE(published);
        const ReviewLinkRecord& record = published->record;
        CHECK(record.token.size() == 26);
        CHECK(record.linkId == publicLinkId(record.token));
        CHECK(record.kind == store->kind());
        CHECK(record.sequence == 7);
        CHECK(record.expiresAt == 1793874937 + 3 * 24 * 3600);
        CHECK(published->bytes > 5000);

        const std::string base = "reviews/" + record.token;
        REQUIRE((*store->stat(base + "/review.mp4")).has_value());
        REQUIRE((*store->stat(base + "/index.html")).has_value());
        REQUIRE((*store->stat(base + "/comments/README.txt")).has_value());
        auto page = store->get(base + "/index.html", 1u << 20);
        REQUIRE(page);
        // nothing about the project or the computer is in the folder
        CHECK(page->data.find("D:/") == std::string::npos);
        CHECK(page->data.find(record.linkId) != std::string::npos);
        if (store != &s3store) {
            CHECK(page->data.find(record.token) == std::string::npos); // (a signed link has the token in its path)
        }
        if (store == &s3store) {
            // presigned: the page plays the signed video link, valid for the shorter of what was asked and 7 days
            CHECK(record.url.find("X-Amz-Signature=") != std::string::npos);
            CHECK(page->data.find("X-Amz-Signature=") != std::string::npos);
            CHECK(page->data.find("X-Amz-Expires=259200") != std::string::npos);
            // the signed address never travelled in a request body
            CHECK(s3.everythingSent().find("X-Amz-Signature") == std::string::npos);
        } else {
            CHECK(record.url.empty());
            CHECK(page->data.find("\"video\":\"review.mp4\"") != std::string::npos);
        }

        // reviewers' files are found, others are counted and not read
        REQUIRE(store->put(base + "/comments/comments-maria-1a2b.json", "{\"format\":\"wabcut-review-comments\"}", Precondition::none()));
        REQUIRE(store->put(base + "/comments/notes.txt", "hello", Precondition::none()));
        auto collected = collectComments(*store, record);
        REQUIRE(collected);
        REQUIRE(collected->files.size() == 1);
        CHECK(collected->files[0].name == "comments-maria-1a2b.json");
        CHECK(collected->skipped == 1);

        // revoke: everything of the link is gone
        auto removed = revokeReviewLink(*store, record);
        REQUIRE(removed);
        CHECK(*removed >= 5);
        CHECK(store->listAll("reviews")->empty());
        CHECK_FALSE((*store->stat(base + "/index.html")).has_value());
    }
    CHECK_FALSE(fs::exists(folder.path / "root" / "reviews")); // the folder store leaves no empty folders
}

TEST_CASE("A failed upload leaves nothing on the storage; links last at most 7 days where they are signed", "[cloud][review]") {
    TempFolder work("publish-fail");
    writeAll(work.path / "copy.mp4", std::string(3000, 'v'));
    MemoryStore store;
    PublishRequest request;
    request.video = work.path / "copy.mp4";
    request.stagingFolder = work.path;
    request.page = samplePage();
    request.title = "x";
    SECTION("the video upload fails") {
        store.failNext("put", "", StoreError{StoreFailure::Network, "connection lost"});
        auto r = publishReviewLink(store, request, fixedRandom(), [] { return std::int64_t{1793874937}; });
        REQUIRE_FALSE(r);
        CHECK(r.error().kind == StoreFailure::Network);
        CHECK(store.listAll("")->empty());
    }
    SECTION("the page upload fails after the video went up") {
        store.failNext("put", "reviews/" + newToken(fixedRandom()) + "/index.html", StoreError{StoreFailure::Network, "connection lost"});
        auto r = publishReviewLink(store, request, fixedRandom(), [] { return std::int64_t{1793874937}; });
        REQUIRE_FALSE(r);
        CHECK(r.error().kind == StoreFailure::Network);
        CHECK(store.listAll("")->empty());
    }
    SECTION("no review copy yet") {
        request.video = work.path / "missing.mp4";
        auto r = publishReviewLink(store, request, fixedRandom(), [] { return std::int64_t{1793874937}; });
        REQUIRE_FALSE(r);
        CHECK(r.error().kind == StoreFailure::NotFound);
    }
    SECTION("a signed link never lasts longer than 7 days, and an unlimited request becomes 7 days") {
        int asked = 0;
        request.presign = [&](std::string_view path, int seconds) -> StoreResult<std::string> {
            asked = std::max(asked, seconds);
            return "https://signed.example.com/" + std::string(path) + "?X-Amz-Signature=abc";
        };
        request.expiresAt = 0;
        auto never = publishReviewLink(store, request, fixedRandom(), [] { return std::int64_t{1793874937}; });
        REQUIRE(never);
        CHECK(never->record.expiresAt == 1793874937 + 7 * 24 * 3600);
        CHECK(asked == 7 * 24 * 3600);
        request.expiresAt = 1793874937 + 30 * 24 * 3600;
        auto tooLong = publishReviewLink(store, request, [](std::size_t n) { return std::string(n, '\x11'); }, [] { return std::int64_t{1793874937}; });
        REQUIRE(tooLong);
        CHECK(tooLong->record.expiresAt == 1793874937 + 7 * 24 * 3600);
    }
}

TEST_CASE("Review link records stay beside the project and are checked when read", "[cloud][review]") {
    TempFolder dir("records");
    const fs::path project = dir.path / "Film.wabcut";
    ReviewLinkRecord a;
    a.token = "abcdefghijklmnopqrstuvwxyz";
    a.linkId = publicLinkId(a.token);
    a.location = "loc-1";
    a.kind = "s3";
    a.title = "Client cut v2";
    a.created = "2026-11-05T09:30:00Z";
    a.expiresAt = 1800000000;
    a.sequence = 7;
    a.startTicks = 5;
    a.durationTicks = 9;
    a.rateNum = 30000;
    a.rateDen = 1001;
    a.contentHash = "abc";
    a.bytes = 1234;
    a.url = "https://signed.example.com/x?X-Amz-Signature=abc";
    CHECK(reviewLinksPath(project).filename() == "Film.wabcut-review-links");
    REQUIRE(writeReviewLinks(project, {a}));
    auto read = readReviewLinks(project);
    REQUIRE(read.size() == 1);
    CHECK(read[0] == a);
    // nothing about the project is written beside the link, and a hostile record is dropped
    ReviewLinkRecord evil = a;
    evil.token = "../../etc";
    evil.linkId = publicLinkId(evil.token);
    ReviewLinkRecord forged = a;
    forged.token = "zyxwvutsrqponmlkjihgfedcba"; // linkId does not belong to the token
    REQUIRE(writeReviewLinks(project, {a, evil, forged}));
    CHECK(readReviewLinks(project).size() == 1);
    REQUIRE(writeReviewLinks(project, {}));
    CHECK_FALSE(fs::exists(reviewLinksPath(project)));
    auto revoked = [&] {
        MemoryStore store;
        return revokeReviewLink(store, evil);
    }();
    REQUIRE_FALSE(revoked);
    CHECK(revoked.error().kind == StoreFailure::Invalid);
}

// ---- comments from reviewers ----

namespace {

std::string commentsFile(const std::string& link, const json& comments, const std::string& reviewer = "Maria") {
    return json{{"format", "wabcut-review-comments"}, {"version", 1}, {"link", link}, {"reviewer", reviewer}, {"comments", comments}}.dump();
}

/// 300 frames of one clip (one character is one frame): long enough for notes at frame 100 and later.
wabcut::test::Timeline makeTimeline() {
    const std::string row = "A" + std::string(299, 'a');
    return wabcut::test::Timeline("V1 tgt |" + row + "|\nA1 tgt |" + row + "|");
}

} // namespace

TEST_CASE("Comments files from reviewers are untrusted input", "[cloud][review]") {
    SECTION("a good file") {
        const auto file = parseReviewComments(commentsFile("lid", json::array({json{{"frame", 12}, {"text", "The logo is too small"},
                                                                                     {"mark", json{{"x", 0.7}, {"y", 0.05}, {"width", 0.2}, {"height", 0.1}}},
                                                                                     {"createdAt", "2026-11-05T09:30:00Z"}}})));
        REQUIRE(file);
        CHECK(file->link == "lid");
        CHECK(file->reviewer == "Maria");
        REQUIRE(file->comments.size() == 1);
        CHECK(file->comments[0].frame == 12);
        CHECK(file->comments[0].text == "The logo is too small");
        REQUIRE(file->comments[0].mark.has_value());
        CHECK(file->comments[0].mark->x == 0.7);
        CHECK(file->skipped.empty());
    }
    SECTION("not what it says it is") {
        CHECK_FALSE(parseReviewComments("not json"));
        CHECK_FALSE(parseReviewComments("{\"format\":\"something else\",\"comments\":[]}"));
        CHECK_FALSE(parseReviewComments("{\"format\":\"wabcut-review-comments\",\"version\":1}"));
        auto newer = parseReviewComments("{\"format\":\"wabcut-review-comments\",\"version\":9,\"comments\":[]}");
        REQUIRE_FALSE(newer);
        CHECK(newer.error().code == ErrorCode::VersionMismatch);
        CHECK_FALSE(parseReviewComments(std::string(kMaxReviewCommentsBytes + 1, ' ')));
        CHECK_FALSE(parseReviewComments(std::string(2000, '[') + std::string(2000, ']')));
    }
    SECTION("single comments that are wrong are left out and listed") {
        json list = json::array();
        list.push_back(json{{"frame", 1}, {"text", "fine"}});
        list.push_back(json{{"frame", -4}, {"text", "negative frame"}});
        list.push_back(json{{"frame", 2.5}, {"text", "fractional frame"}});
        list.push_back(json{{"frame", 3}, {"text", "   "}});
        list.push_back(json{{"frame", 4}});
        list.push_back("a string");
        list.push_back(json{{"frame", 5}, {"text", "ok again"}, {"mark", json{{"x", 5}, {"y", 0.1}}}}); // mark outside the frame: dropped, comment kept
        list.push_back(json{{"frame", 200000000}, {"text", "absurd frame"}});
        const auto file = parseReviewComments(commentsFile("lid", list));
        REQUIRE(file);
        REQUIRE(file->comments.size() == 2);
        CHECK(file->comments[1].frame == 5);
        CHECK_FALSE(file->comments[1].mark.has_value());
        CHECK(file->skipped.size() == 6);
    }
    SECTION("text is cleaned: control characters and bidirectional overrides go, newlines stay") {
        const std::string text = std::string("line one\n") + "two\x01\x02" + "\xE2\x80\xAE" + "evil" + "\xE2\x80\xAC" + "\ttab";
        const auto file = parseReviewComments(commentsFile("lid", json::array({json{{"frame", 0}, {"text", text}}}), std::string("Ma\nria\x07") + "\xE2\x80\xAE"));
        REQUIRE(file);
        REQUIRE(file->comments.size() == 1);
        CHECK(file->comments[0].text == "line one\ntwoevil\ttab");
        CHECK(file->reviewer == "Ma ria");
    }
    SECTION("text can never be read as markup by whatever shows it") {
        const auto file = parseReviewComments(commentsFile("lid", json::array({json{{"frame", 0}, {"text", "<img src=\"file://evil/x.png\"> a < b"}}}), "<b>Maria</b>"));
        REQUIRE(file);
        REQUIRE(file->comments.size() == 1);
        CHECK(file->comments[0].text.find('<') == std::string::npos);
        CHECK(file->comments[0].text.find('>') == std::string::npos);
        CHECK(file->comments[0].text.find("img src") != std::string::npos); // still readable
        CHECK(file->reviewer.find('<') == std::string::npos);
    }
    SECTION("sizes are bounded") {
        const auto file = parseReviewComments(commentsFile("lid", json::array({json{{"frame", 0}, {"text", std::string(20000, 'x')}}}), std::string(500, 'n')));
        REQUIRE(file);
        CHECK(file->comments[0].text.size() == kMaxReviewTextBytes);
        CHECK(file->reviewer.size() == 100);
        json many = json::array();
        for (std::size_t i = 0; i < kMaxReviewComments + 10; ++i) {
            many.push_back(json{{"frame", 1}, {"text", "c"}});
        }
        const auto big = parseReviewComments(commentsFile("lid", many));
        REQUIRE(big);
        CHECK(big->comments.size() == kMaxReviewComments);
        CHECK_FALSE(big->skipped.empty());
    }
    SECTION("text that is not UTF-8 is refused") {
        const auto file = parseReviewComments("{\"format\":\"wabcut-review-comments\",\"version\":1,\"link\":\"l\",\"comments\":[{\"frame\":1,\"text\":\"bad\xFF\xFE\"}]}");
        // nlohmann refuses the whole document (invalid UTF-8 in a string), which is also a refusal
        if (file) {
            CHECK(file->comments.empty());
        }
    }
}

TEST_CASE("Returned comments become review notes on the right frames in one step", "[cloud][review]") {
    auto tl = makeTimeline();
    const SequenceId seq = tl.seq;
    ReviewLinkTarget target;
    target.link = "lid";
    target.sequence = seq;
    target.start = framesToTicks(100, kRate25);
    target.duration = framesToTicks(150, kRate25);

    const ProjectState before = tl.state;
    ReviewCommentsFile file;
    file.link = "lid";
    file.reviewer = "Maria";
    file.comments = {ReviewComment{0, "First frame", std::nullopt, "2026-11-05T09:30:00Z"},
                     ReviewComment{25, "One second in", ReviewMark{0.7, 0.05, 0.2, 0.1}, ""},
                     ReviewComment{149, "Last frame", std::nullopt, ""},
                     ReviewComment{150, "After the end", std::nullopt, ""}};
    auto imported = edit::importReviewComments(before, file, target, false, "2026-11-06T00:00:00Z");
    REQUIRE(imported);
    for (const std::string& c : imported->conflicts) {
        INFO(c);
    }
    INFO("sequence duration " << before.findSequence(seq)->duration.value << " start " << target.start.value);
    CHECK(imported->added == 3);
    REQUIRE(imported->conflicts.size() == 1);
    CHECK(imported->conflicts[0].find("After the end") != std::string::npos);
    REQUIRE(imported->state.reviewNotes.size() == 3);
    const auto& notes = imported->state.reviewNotes;
    CHECK(notes[0].start == framesToTicks(100, kRate25));
    CHECK(notes[1].start == framesToTicks(125, kRate25));
    CHECK(notes[2].start == framesToTicks(249, kRate25));
    CHECK(notes[0].author == "Maria (review link)");
    CHECK(notes[0].createdAt == "2026-11-05T09:30:00Z");
    CHECK(notes[1].createdAt == "2026-11-06T00:00:00Z"); // no time of its own: when it was imported
    REQUIRE(notes[1].mark.has_value());
    CHECK(notes[1].mark->x == 0.7);
    CHECK(notes[0].status == ReviewStatus::Open);
    CHECK(before.reviewNotes.size() == 0); // the input state is untouched (the caller commits the result as one undo step)

    // importing the same file again adds nothing
    auto again = edit::importReviewComments(imported->state, file, target, false, "2026-11-07T00:00:00Z");
    REQUIRE(again);
    CHECK(again->added == 0);
    CHECK(again->unchanged == 3);
    CHECK(again->state.reviewNotes.size() == 3);

    // at 29.97 the same frame number lands on the sequence's own frame grid
    ProjectState ntsc = tl.state;
    auto seqCopy = std::make_shared<Sequence>(*ntsc.findSequence(seq));
    seqCopy->frameRate = kRate29_97;
    seqCopy->duration = framesToTicks(600, kRate29_97);
    ntsc = ntsc.withSequence(seqCopy);
    ReviewLinkTarget t2 = target;
    t2.start = Ticks{};
    t2.duration = framesToTicks(600, kRate29_97);
    ReviewCommentsFile f2;
    f2.link = "lid";
    f2.reviewer = "Maria";
    f2.comments = {ReviewComment{299, "Ten seconds almost", std::nullopt, ""}};
    auto onNtsc = edit::importReviewComments(ntsc, f2, t2, false, "2026-11-06T00:00:00Z");
    REQUIRE(onNtsc);
    REQUIRE(onNtsc->state.reviewNotes.size() == 1);
    CHECK(onNtsc->state.reviewNotes[0].start == framesToTicks(299, kRate29_97));

    // also as sequence markers, in the same step
    auto withMarkers = edit::importReviewComments(before, file, target, true, "2026-11-06T00:00:00Z");
    REQUIRE(withMarkers);
    CHECK(withMarkers->markers == 3);
    CHECK(withMarkers->state.findSequence(seq)->markers.size() == 3);
    CHECK(withMarkers->state.reviewNotes.size() == 3);
}

TEST_CASE("Comments only ever land on the sequence of the link they belong to", "[cloud][review]") {
    auto tl = makeTimeline();
    ReviewLinkTarget target;
    target.link = "lid";
    target.sequence = tl.seq;
    target.start = Ticks{};
    target.duration = framesToTicks(250, kRate25);
    ReviewCommentsFile file;
    file.reviewer = "Maria";
    file.comments = {ReviewComment{1, "Hello", std::nullopt, ""}};
    SECTION("a file of another link") {
        file.link = "another";
        auto r = edit::importReviewComments(tl.state, file, target, false, "2026-11-06T00:00:00Z");
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::InvalidArgument);
    }
    SECTION("a sequence that is gone") {
        file.link = "lid";
        target.sequence = SequenceId{999};
        auto r = edit::importReviewComments(tl.state, file, target, false, "2026-11-06T00:00:00Z");
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::NotFound);
    }
    SECTION("a long reviewer name still fits the author field") {
        file.link = "lid";
        file.reviewer = std::string(100, 'n');
        auto r = edit::importReviewComments(tl.state, file, target, false, "2026-11-06T00:00:00Z");
        REQUIRE(r);
        REQUIRE(r->state.reviewNotes.size() == 1);
        CHECK(r->state.reviewNotes[0].author.size() <= kMaxReviewAuthorBytes);
        CHECK(r->state.reviewNotes[0].author.ends_with(" (review link)"));
    }
}

TEST_CASE("The review page is written for the browser check", "[cloud][review]") {
    // tests/cloud/browser/run.ps1 opens these pages in a real browser (ctest has none); here they are only written, with a real video
    // next to the first one when the media fixtures exist.
    const fs::path folder = fs::path(WABCUT_TEST_OUTPUT_DIR) / "cloud" / "browser";
    std::error_code ec;
    fs::remove_all(folder, ec);
    ReviewPageInput in = samplePage();
    in.title = "Browser check";
    in.frameRate = Rate{120, 1};
    in.durationFrames = 240;
    in.expiresAt = 4102444800; // the year 2100
    in.instructions = "Please watch it and comment.";
    in.fromFiles = true;
    writeAll(folder / "review" / "index.html", buildReviewPage(in).html);
    in.expiresAt = 1000; // long gone
    writeAll(folder / "review-expired" / "index.html", buildReviewPage(in).html);
    CHECK(fs::exists(folder / "review" / "index.html"));
    CHECK(fs::exists(folder / "review-expired" / "index.html"));
    const fs::path video = fs::path(WABCUT_TEST_MEDIA_DIR) / "h264_360p120.mp4";
    if (fs::exists(video)) {
        fs::copy_file(video, folder / "review" / "review.mp4", fs::copy_options::overwrite_existing, ec);
    }
}
