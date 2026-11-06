#pragma once

// Review links (0.7, docs/cloud.md): a review copy of a sequence (an H.264 video the export engine renders) and ONE static web page
// that plays it and lets a reviewer comment on exact frames, kept on storage the user owns. There is no Wabcut server: the page
// is a file, the reviewer's comments leave the browser as a file the reviewer sends back (or drops into the shared folder), and
// the editor reads that file into the project's review notes (model/ReviewComments.h).
//
// The page (buildReviewPage) has no external request of any kind. Its CSP forbids everything but its own script and style (named
// by hash) and the video; everything a reviewer or the project supplies reaches the page as JSON in a non-executing data block
// and is inserted only as text. The page works from a downloaded folder as well as from a web address.
//
// What "hard to guess, expires, revocable" means, per kind of storage (docs/cloud.md says the same to the user):
//   * the address holds 128 random bits (the token), the page's own comments carry a different public id (a hash of the token), so
//     a comments file that gets passed around does not reveal the address;
//   * S3: the page and the video are shared by presigned links that stop working by themselves (7 days at most), no public bucket;
//   * folder and WebDAV: the user shares the review folder with the sync service's or the server's own sharing; the page
//     refuses to play after the expiry date, and "Remove" deletes the files (a downloaded copy of course stays downloaded);
//   * revoking = deleting the review folder from the storage.
// The record of a link (which sequence, from where, until when) stays on the computer that made it, next to the project file;
// nothing about the project is put into the review folder.

#include <wabcut/cloud/RemoteStore.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::cloud {

inline constexpr std::int64_t kMaxPresignSeconds = 7 * 24 * 3600;
inline constexpr std::uint64_t kMaxCommentFileBytes = 4u * 1024u * 1024u;

/// The public id of a link: what the page and the comments file carry instead of the token (24 hex digits of sha256(token)).
std::string publicLinkId(std::string_view token);

struct ReviewPageInput {
    std::string title;          ///< the page's heading (the sequence's name by default)
    std::string linkId;
    std::string videoUrl;       ///< "review.mp4" (next to the page) or an absolute https URL (a presigned link)
    Rate frameRate{30, 1};
    bool dropFrame = false;
    std::int64_t durationFrames = 0;
    std::int64_t expiresAt = 0; ///< unix seconds, 0 = never
    std::string instructions;   ///< a few words from the editor, shown above the comment box
    bool fromFiles = false;     ///< the page is meant to open from a folder too (media-src also allows file:)
};

struct ReviewPage {
    std::string html;
    std::string scriptHash; ///< "sha256-..." as the CSP names it
    std::string styleHash;
};

ReviewPage buildReviewPage(const ReviewPageInput& input);

/// What this computer remembers about a link it published.
struct ReviewLinkRecord {
    std::string token;        ///< secret: the folder's name on the storage
    std::string linkId;
    std::string location;     ///< id of the storage location it was published to
    std::string kind;         ///< "folder" | "webdav" | "s3"
    std::string title;
    std::string created;      ///< ISO-8601 UTC
    std::int64_t expiresAt = 0;
    bool removeWhenExpired = true;
    std::uint64_t sequence = 0;
    std::int64_t startTicks = 0;     ///< where the copy starts in the sequence
    std::int64_t durationTicks = 0;
    std::int32_t rateNum = 30;
    std::int32_t rateDen = 1;
    std::string contentHash;  ///< hash of what was rendered (the cut at that time), to warn when it changed
    std::uint64_t bytes = 0;  ///< what was uploaded
    std::string url;          ///< the link to give out (S3: the presigned page link; otherwise empty)
    std::string path() const { return "reviews/" + token; }
    bool operator==(const ReviewLinkRecord&) const = default;
};

std::filesystem::path reviewLinksPath(const std::filesystem::path& projectFile);
std::vector<ReviewLinkRecord> readReviewLinks(const std::filesystem::path& projectFile);
std::expected<void, Error> writeReviewLinks(const std::filesystem::path& projectFile, const std::vector<ReviewLinkRecord>& links);

struct PublishRequest {
    std::filesystem::path video;      ///< the rendered review copy (H.264 + AAC in MP4)
    std::filesystem::path stagingFolder; ///< where the page is written before it is uploaded
    ReviewPageInput page;             ///< filled in by the caller except the video URL and the id
    std::string location;
    std::string title;
    std::int64_t expiresAt = 0;       ///< unix seconds, 0 = never (S3 is capped at 7 days)
    bool removeWhenExpired = true;
    std::uint64_t sequence = 0;
    std::int64_t startTicks = 0;
    std::int64_t durationTicks = 0;
    std::string contentHash;
    /// S3 only: a link for `path` valid `seconds`.
    std::function<StoreResult<std::string>(std::string_view path, int seconds)> presign;
};

struct Published {
    ReviewLinkRecord record;
    std::uint64_t bytes = 0;
};

/// Uploads the video and the page into `reviews/<token>/` of the store. Writes nothing before this call; fails without leaving
/// files behind when the video upload fails (the partial folder is removed).
StoreResult<Published> publishReviewLink(RemoteStore& store, const PublishRequest& request, const RandomBytes& random, const UnixClock& clock,
                                         const Progress& progress = {}, std::stop_token stop = {});
/// Deletes the link's folder (the page, the video, the comments reviewers left there). Returns how many files.
StoreResult<std::size_t> revokeReviewLink(RemoteStore& store, const ReviewLinkRecord& record, std::stop_token stop = {});

struct CommentFile {
    std::string name;
    std::string text;
};
struct CollectedComments {
    std::vector<CommentFile> files;
    std::size_t skipped = 0; ///< entries that are not comment files (other names, too large)
};
/// The comment files reviewers left in the link's `comments/` folder (a shared folder they can write into).
StoreResult<CollectedComments> collectComments(RemoteStore& store, const ReviewLinkRecord& record, std::stop_token stop = {});

} // namespace wabcut::cloud
