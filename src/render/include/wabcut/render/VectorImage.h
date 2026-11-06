#pragma once

// Vector artwork as pictures that stay sharp at the size they are shown (0.7, card #86).
//
// SVG, Adobe Illustrator (.ai) and EPS files import as stills whose picture is drawn again at the size it is shown:
//   * SVG (.svg, .svgz): Qt Svg (QSvgRenderer).
//   * Illustrator (.ai): a file saved "PDF compatible" is a PDF. On Windows the page is drawn by the Windows.Data.Pdf
//     API (WinRT); anywhere else, and for the older PostScript-based files, by an installed Ghostscript.
//   * EPS (.eps): an installed Ghostscript, found when it is needed (the PATH, then the usual install folders). Without
//     one the file is refused with a message that says what to install - nothing is bundled.
// The nominal size of the artwork (probeVector) is what the Library shows and what Scale to Frame Size fits; renderVector
// draws it at any pixel size, and VectorPictures keeps the drawings (a few sizes of each file) so playback and export
// re-draw only when the size shown has changed.
//
// Everything here reads untrusted files: sizes and file lengths are bounded, Ghostscript runs with -dSAFER (no file or
// process access from the PostScript), with a time limit, and nothing is ever run on the calling thread's event loop.
// Safe on any thread; it needs a QGuiApplication for text in SVG (fonts).

#include <wabcut/model/Error.h>

#include <QImage>
#include <QSize>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace wabcut::render {

enum class VectorFormat : std::uint8_t { Svg, Illustrator, Eps };

/// "SVG", "Illustrator", "EPS".
std::string_view toString(VectorFormat format);

/// The format a file name claims (case-insensitive .svg, .svgz, .ai, .eps); nullopt for anything else.
std::optional<VectorFormat> vectorFormatOf(const std::filesystem::path& file);

/// The largest nominal side kept (a hostile or mistaken viewBox is scaled down to it), and the largest side ever drawn.
inline constexpr double kMaxVectorSide = 16384.0;
inline constexpr int kMaxVectorRenderSide = 8192;
/// Larger files are refused (an SVG is text; an Illustrator file with embedded images can be big).
inline constexpr std::uintmax_t kMaxVectorFileBytes = std::uintmax_t{256} * 1024 * 1024;

/// The artwork's nominal size: SVG user units (CSS pixels), points for PDF-based and PostScript files (1 pt = 1 px).
struct VectorInfo {
    VectorFormat format = VectorFormat::Svg;
    double width = 0.0;
    double height = 0.0;

    bool operator==(const VectorInfo&) const = default;
};

/// Looks for Ghostscript: `searchPath` (PATH syntax: ';' on Windows, ':' elsewhere) or, without one, the PATH and the usual
/// install folders (Windows: Program Files\gs\gs*\bin, newest first; macOS: Homebrew and MacPorts). The console build on
/// Windows (gswin64c, gswin32c, then gs). Cheap: file-system checks only.
std::optional<std::filesystem::path> findGhostscript(const std::optional<std::string>& searchPath = std::nullopt);

/// For tests (and a future setting): look for Ghostscript in these directories only. nullopt = back to the PATH and the usual
/// folders. Thread-safe.
void setGhostscriptSearchPath(std::optional<std::string> searchPath);

/// The message that says what to install when Ghostscript is needed and not found.
std::string ghostscriptMissingMessage(VectorFormat format);

/// Reads the file's header and size: the nominal size, or why it cannot be imported (not a vector file of its kind, too big,
/// a PDF-less Illustrator file, Ghostscript missing, ...). Draws nothing.
std::expected<VectorInfo, Error> probeVector(const std::filesystem::path& file);

/// Draws the artwork into an image of exactly `pixels` (premultiplied ARGB, transparent where nothing is drawn). Both sides
/// at most kMaxVectorRenderSide. `info` is probeVector's answer for the file (the caller usually has it).
std::expected<QImage, Error> renderVector(const std::filesystem::path& file, const VectorInfo& info, QSize pixels);

/// The size to draw artwork of `info` so that a box of `boxWidth` x `boxHeight` pixels is covered on both axes (the larger
/// of the two scales), with the artwork's shape, rounded up to a quarter-octave step so that a slowly growing box does not
/// draw it afresh every frame, and never more than kMaxVectorRenderSide on a side. At least 1 x 1.
QSize vectorRenderSize(const VectorInfo& info, double boxWidth, double boxHeight);

/// Drawings of vector files, kept by path, modification time, file size and pixel size (an LRU of a few pictures, at most
/// ~128 MiB). Thread-safe; renders happen outside the lock.
class VectorPictures {
public:
    static VectorPictures& shared();

    /// The file's nominal size (kept while the file is unchanged), or why not.
    std::expected<VectorInfo, Error> info(const std::filesystem::path& file);
    /// The artwork drawn to cover a box of `boxWidth` x `boxHeight` pixels (vectorRenderSize); null when it cannot be drawn.
    QImage picture(const std::filesystem::path& file, double boxWidth, double boxHeight);

    /// Drawings made so far (a size reused does not count).
    std::uint64_t renders() const;
    void clear();

private:
    struct Entry {
        std::string path;
        std::int64_t size = -1;
        std::int64_t mtime = 0;
        QSize pixels;
        QImage image;
    };
    struct InfoEntry {
        std::string path;
        std::int64_t size = -1;
        std::int64_t mtime = 0;
        std::expected<VectorInfo, Error> info;
    };
    mutable std::mutex m_mutex;
    std::list<Entry> m_images;      // most recent first
    std::list<InfoEntry> m_infos;   // most recent first, at most 64
    std::uint64_t m_renders = 0;
};

} // namespace wabcut::render
