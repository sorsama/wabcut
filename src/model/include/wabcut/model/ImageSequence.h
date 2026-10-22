#pragma once

// Image sequences (0.2, Premiere's File > Import with "Image Sequence" ticked): numbered stills (shot_0001.png,
// shot_0002.png, ...) imported as one video clip that plays one file per frame at a chosen frame rate.
//
// A sequence is an ordinary media item whose locator path names the whole run instead of one file:
//
//     <directory>/<prefix>%0<digits>d<suffix>?start=<first number>&rate=<num>:<den>
//
// The part before '?' is FFmpeg's image2 pattern ('%' in the folder or the names doubled), so the decoders open it
// with the image2 demuxer (media::detail::openInput); '?' cannot occur in a Windows file name, which keeps the path
// apart from any real file. The first frame's file stands for the sequence wherever a file is checked: existence,
// fingerprint and relinking (MediaLocator.cpp, editor::checkMediaOnDisk). Paths read from a project are untrusted:
// parseImageSequencePath bounds every field and returns nullopt for anything else.

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace wabcut {

struct ImageSequenceSpec {
    std::filesystem::path directory;
    std::string prefix;      ///< UTF-8 file name text before the frame number
    std::string suffix;      ///< UTF-8 file name text after it, extension included (".png")
    int digits = 1;          ///< the number's width: shorter numbers are zero-padded (0001); longer ones are not
    std::int64_t first = 0;  ///< the first frame's number
    std::int64_t count = 0;  ///< frames, numbered contiguously from `first` (0 when only parsed from a path)
    Rate frameRate{30000, 1001};

    /// The file of frame `number`.
    std::filesystem::path framePath(std::int64_t number) const;
    std::filesystem::path firstFramePath() const { return framePath(first); }
};

/// Premiere's Preferences > Media > Indeterminate Media Timebase default: the rate image sequences import at unless
/// another is chosen.
inline constexpr Rate kDefaultImageSequenceRate{30000, 1001};
/// Bounds (untrusted input): frames in one sequence, the frame number's width and value, the rate, the path's length
/// (FFmpeg's image2 formats each frame's name into a 1024-byte buffer).
inline constexpr std::int64_t kMaxImageSequenceFrames = 1'000'000;
inline constexpr int kMaxImageSequenceDigits = 9;
inline constexpr std::int64_t kMaxImageSequenceNumber = 999'999'999;
inline constexpr std::int32_t kMaxImageSequenceRate = 1000;
inline constexpr std::size_t kMaxImageSequencePathBytes = 1000;

/// True for the still formats Premiere imports as sequences: PNG, JPEG, TIFF, DPX, Targa, OpenEXR and BMP, and CinemaDNG
/// frames (DNG, 0.2) (`extension` with its dot, any case).
bool isImageSequenceExtension(std::string_view extension);

/// The numbered sequence that starts at `frame` (its file name's last run of digits is the frame number): `frame` and
/// the files numbered after it without a gap, at most kMaxImageSequenceFrames. InvalidArgument when `frame` has no
/// number, is not a still format or `frameRate` is out of range; NotFound when `frame` does not exist.
std::expected<ImageSequenceSpec, Error> findImageSequence(const std::filesystem::path& frame, Rate frameRate);

/// FFmpeg's image2 pattern of the sequence: "<directory>/<prefix>%0<digits>d<suffix>" with '%' doubled elsewhere.
std::string imageSequencePattern(const ImageSequenceSpec& spec);
/// The media path of the sequence (see the file comment).
std::string imageSequencePath(const ImageSequenceSpec& spec);
/// The sequence a media path names, or nullopt for any other path (a real file, or a malformed or out-of-range
/// sequence path). `count` is 0: the path does not hold it.
std::optional<ImageSequenceSpec> parseImageSequencePath(std::string_view utf8);

/// The file that stands for `path` in file checks: an image sequence's first frame, a spanned clip's first part
/// (SpannedClip.h), a layered file's PSD (LayeredFile.h), else `path` itself.
std::filesystem::path mediaFileToCheck(const std::filesystem::path& path);
/// is_regular_file of mediaFileToCheck(path).
bool mediaFileExists(const std::filesystem::path& path);
/// The media path an item recorded as `recorded` has once relinked to `file` (the file the user or a search found for
/// mediaFileToCheck(recorded)): a sequence, spanned clip or layered-file item keeps what its path says beyond the file
/// (the numbering and rate, the other parts next to the new file, the layers); any other item is `file` itself.
std::string relinkedMediaPath(std::string_view recorded, const std::filesystem::path& file);

} // namespace wabcut
