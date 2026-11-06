// Review links: publishing, revoking, the local records and the comments reviewers leave (ReviewLink.h).

#include <wabcut/cloud/ReviewLink.h>

#include "FileUtil.h"

#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/ProjectIO.h>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace wabcut::cloud {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr std::size_t kMaxRecords = 200;

std::string text(const json& j, const char* key, std::size_t limit) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return {};
    }
    std::string value = it->get<std::string>();
    if (value.size() > limit) {
        value.resize(limit);
    }
    return value;
}

std::int64_t integer(const json& j, const char* key, std::int64_t fallback = 0) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number_integer() ? it->get<std::int64_t>() : fallback;
}

} // namespace

fs::path reviewLinksPath(const fs::path& projectFile) {
    fs::path p = projectFile;
    p += "-review-links"; // "Film.wabcut" -> "Film.wabcut-review-links"
    return p;
}

std::vector<ReviewLinkRecord> readReviewLinks(const fs::path& projectFile) {
    std::vector<ReviewLinkRecord> out;
    const auto data = detail::readFileBounded(reviewLinksPath(projectFile), 4u * 1024u * 1024u);
    if (!data || !io::jsonNestingWithin(*data, 8)) {
        return out;
    }
    const json doc = json::parse(*data, nullptr, false);
    if (!doc.is_object() || text(doc, "format", 64) != "wabcut-review-links" || !doc.contains("links") || !doc["links"].is_array()) {
        return out;
    }
    for (const json& j : doc["links"]) {
        if (!j.is_object() || out.size() >= kMaxRecords) {
            continue;
        }
        ReviewLinkRecord r;
        r.token = text(j, "token", 64);
        r.linkId = text(j, "linkId", 64);
        r.location = text(j, "location", 128);
        r.kind = text(j, "kind", 16);
        r.title = text(j, "title", 256);
        r.created = text(j, "created", 40);
        r.expiresAt = integer(j, "expiresAt");
        r.removeWhenExpired = j.value("removeWhenExpired", true);
        r.sequence = static_cast<std::uint64_t>(std::max<std::int64_t>(0, integer(j, "sequence")));
        r.startTicks = integer(j, "startTicks");
        r.durationTicks = integer(j, "durationTicks");
        r.rateNum = static_cast<std::int32_t>(std::clamp<std::int64_t>(integer(j, "rateNum", 30), 1, 1'000'000));
        r.rateDen = static_cast<std::int32_t>(std::clamp<std::int64_t>(integer(j, "rateDen", 1), 1, 1'000'000));
        r.contentHash = text(j, "contentHash", 64);
        r.bytes = static_cast<std::uint64_t>(std::max<std::int64_t>(0, integer(j, "bytes")));
        r.url = text(j, "url", 8192);
        // a record whose token is not one this library makes is not used (it names a folder on the storage)
        if (!validSegment(r.token) || r.linkId != publicLinkId(r.token)) {
            continue;
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::expected<void, Error> writeReviewLinks(const fs::path& projectFile, const std::vector<ReviewLinkRecord>& links) {
    json list = json::array();
    for (const ReviewLinkRecord& r : links) {
        list.push_back(json{{"token", r.token}, {"linkId", r.linkId}, {"location", r.location}, {"kind", r.kind}, {"title", r.title},
                            {"created", r.created}, {"expiresAt", r.expiresAt}, {"removeWhenExpired", r.removeWhenExpired},
                            {"sequence", r.sequence}, {"startTicks", r.startTicks}, {"durationTicks", r.durationTicks},
                            {"rateNum", r.rateNum}, {"rateDen", r.rateDen}, {"contentHash", r.contentHash}, {"bytes", r.bytes},
                            {"url", r.url}});
    }
    const json doc{{"format", "wabcut-review-links"}, {"version", 1}, {"links", std::move(list)}};
    if (links.empty()) {
        std::error_code ec;
        fs::remove(reviewLinksPath(projectFile), ec);
        return {};
    }
    return io::writeFileAtomically(reviewLinksPath(projectFile), doc.dump(2) + "\n");
}

StoreResult<Published> publishReviewLink(RemoteStore& store, const PublishRequest& request, const RandomBytes& random, const UnixClock& clock,
                                         const Progress& progress, std::stop_token stop) {
    std::error_code ec;
    if (!fs::is_regular_file(request.video, ec)) {
        return std::unexpected(StoreError{StoreFailure::NotFound, "The review copy is missing: render it first."});
    }
    const std::uint64_t videoBytes = fs::file_size(request.video, ec);
    const std::string token = newToken(random);
    const std::string folder = "reviews/" + token;
    const std::int64_t now = clock();

    std::int64_t expiresAt = request.expiresAt;
    if (request.presign) {
        const std::int64_t latest = now + kMaxPresignSeconds;
        expiresAt = expiresAt <= 0 ? latest : std::min(expiresAt, latest);
    }
    ReviewLinkRecord record;
    record.token = token;
    record.linkId = publicLinkId(token);
    record.location = request.location;
    record.kind = store.kind();
    record.title = request.title;
    record.created = formatIsoUtc(now);
    record.expiresAt = expiresAt;
    record.removeWhenExpired = request.removeWhenExpired;
    record.sequence = request.sequence;
    record.startTicks = request.startTicks;
    record.durationTicks = request.durationTicks;
    record.rateNum = request.page.frameRate.num;
    record.rateDen = request.page.frameRate.den;
    record.contentHash = request.contentHash;

    const auto cleanUp = [&] { (void)store.removeTree(folder); };

    // the video first (the big upload), with progress; then the page that points at it
    std::string videoUrl = "review.mp4";
    if (auto up = store.putFile(folder + "/review.mp4", request.video, Precondition::absent(), "video/mp4", progress, stop); !up) {
        cleanUp();
        return std::unexpected(up.error());
    }
    std::string pageLink;
    if (request.presign) {
        const int seconds = static_cast<int>(std::max<std::int64_t>(60, expiresAt - now));
        auto video = request.presign(folder + "/review.mp4", seconds);
        auto page = request.presign(folder + "/index.html", seconds);
        if (!video || !page) {
            cleanUp();
            return std::unexpected(!video ? video.error() : page.error());
        }
        videoUrl = *video;
        pageLink = *page;
    }
    ReviewPageInput input = request.page;
    input.linkId = record.linkId;
    input.videoUrl = videoUrl;
    input.expiresAt = expiresAt;
    input.title = request.page.title.empty() ? request.title : request.page.title;
    input.fromFiles = !request.presign;
    const ReviewPage page = buildReviewPage(input);

    // the page is uploaded from a file: a presigned address inside it must never reach the request log
    const fs::path pageFile = request.stagingFolder / "index.html";
    if (auto written = io::writeFileAtomically(pageFile, page.html); !written) {
        cleanUp();
        return std::unexpected(StoreError{StoreFailure::Other, "Cannot write the review page: " + written.error().message});
    }
    if (auto up = store.putFile(folder + "/index.html", pageFile, Precondition::absent(), "text/html; charset=utf-8", {}, stop); !up) {
        cleanUp();
        return std::unexpected(up.error());
    }
    // a file in the comments folder makes the folder exist on every kind of storage, and tells reviewers what it is for
    static const std::string note =
        "Put the comments file you downloaded from the review page into this folder.\nThe person who shared the page reads it from here.\n";
    (void)store.put(folder + "/comments/README.txt", note, Precondition::none(), "text/plain; charset=utf-8", stop);

    record.url = pageLink;
    record.bytes = videoBytes + page.html.size();
    Published out;
    out.record = std::move(record);
    out.bytes = out.record.bytes;
    return out;
}

StoreResult<std::size_t> revokeReviewLink(RemoteStore& store, const ReviewLinkRecord& record, std::stop_token stop) {
    if (!validSegment(record.token) || record.linkId != publicLinkId(record.token)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "This review link record is damaged."});
    }
    return store.removeTree(record.path(), stop);
}

StoreResult<CollectedComments> collectComments(RemoteStore& store, const ReviewLinkRecord& record, std::stop_token stop) {
    if (!validSegment(record.token) || record.linkId != publicLinkId(record.token)) {
        return std::unexpected(StoreError{StoreFailure::Invalid, "This review link record is damaged."});
    }
    auto listed = store.list(record.path() + "/comments", stop);
    if (!listed) {
        return std::unexpected(listed.error());
    }
    CollectedComments out;
    for (const StoreEntry& e : *listed) {
        const std::string_view name = baseName(e.path);
        if (e.folder || name.size() < 6 || name.substr(name.size() - 5) != ".json" || e.size > kMaxCommentFileBytes || out.files.size() >= 200) {
            if (!(e.path.size() >= 10 && name == "README.txt")) {
                ++out.skipped;
            }
            continue;
        }
        auto got = store.get(e.path, kMaxCommentFileBytes, stop);
        if (!got) {
            if (got.error().kind == StoreFailure::NotFound) {
                continue;
            }
            return std::unexpected(got.error());
        }
        out.files.push_back({std::string(name), std::move(got->data)});
    }
    return out;
}

} // namespace wabcut::cloud
