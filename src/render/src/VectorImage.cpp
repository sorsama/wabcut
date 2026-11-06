#include <wabcut/render/VectorImage.h>

#include "VectorPdf.h"

#include <wabcut/model/MediaLocator.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QPainter>
#include <QProcess>
#include <QString>
#include <QRectF>
#include <QRegularExpression>
#include <QSizeF>
#include <QStringList>
#include <QSvgRenderer>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

#include <zlib.h>

namespace wabcut::render {

namespace {

using Fs = std::filesystem::path;

constexpr std::uintmax_t kMaxSvgBytes = std::uintmax_t{64} * 1024 * 1024; // an SVG is text
constexpr int kProcessTimeoutMs = 120'000;                                 // one Ghostscript run
constexpr qint64 kMaxProcessOutput = qint64{1} << 30;                      // 1 GiB of PNG at the most
constexpr std::size_t kMaxCachedPictures = 24;
constexpr std::int64_t kMaxCachedBytes = std::int64_t{128} * 1024 * 1024;
constexpr std::size_t kMaxCachedInfos = 64;

std::mutex g_searchMutex;
std::optional<std::string> g_searchPath;

std::string lowered(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

Error invalid(std::string message) { return Error(ErrorCode::InvalidArgument, std::move(message)); }
Error unsupported(std::string message) { return Error(ErrorCode::Unsupported, std::move(message)); }

/// The first `count` bytes of a file (fewer when it is shorter), or the last `count` with `fromEnd`.
std::string bytesOf(const Fs& file, std::size_t count, bool fromEnd = false) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    if (fromEnd) {
        in.seekg(0, std::ios::end);
        const auto size = static_cast<std::streamoff>(in.tellg());
        in.seekg(std::max<std::streamoff>(0, size - static_cast<std::streamoff>(count)), std::ios::beg);
    }
    std::string out(count, '\0');
    in.read(out.data(), static_cast<std::streamsize>(count));
    out.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
    return out;
}

bool hasPdfHeader(const std::string& head) {
    // "%PDF-" within the first kilobyte (a few producers put a banner before it).
    return head.substr(0, 1024).find("%PDF-") != std::string::npos;
}

bool hasPostScriptHeader(const std::string& head) {
    // "%!PS-Adobe" (EPS and PostScript-based Illustrator files), or a DOS EPS binary header C5 D0 D3 C6.
    return head.starts_with("%!") || (head.size() >= 4 && static_cast<unsigned char>(head[0]) == 0xC5 &&
                                      static_cast<unsigned char>(head[1]) == 0xD0 && static_cast<unsigned char>(head[2]) == 0xD3 &&
                                      static_cast<unsigned char>(head[3]) == 0xC6);
}

/// Keeps the nominal size inside [1, kMaxVectorSide] with its shape.
VectorInfo clampedInfo(VectorFormat format, double width, double height) {
    VectorInfo info{format, width, height};
    const double longest = std::max(info.width, info.height);
    if (longest > kMaxVectorSide) {
        info.width *= kMaxVectorSide / longest;
        info.height *= kMaxVectorSide / longest;
    }
    info.width = std::max(1.0, info.width);
    info.height = std::max(1.0, info.height);
    return info;
}

// ---- Ghostscript ----------------------------------------------------------------------------------------------------

std::vector<Fs> directoriesOf(const std::string& list) {
#ifdef _WIN32
    constexpr char kSeparator = ';';
#else
    constexpr char kSeparator = ':';
#endif
    std::vector<Fs> out;
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t end = list.find(kSeparator, start);
        const std::string part = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty()) {
            out.push_back(pathFromUtf8(part));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

std::vector<std::string> ghostscriptNames() {
#ifdef _WIN32
    return {"gswin64c.exe", "gswin32c.exe", "gs.exe"};
#else
    return {"gs"};
#endif
}

std::optional<Fs> inDirectory(const Fs& directory, const std::vector<std::string>& names) {
    std::error_code ec;
    for (const std::string& name : names) {
        const Fs candidate = directory / pathFromUtf8(name);
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return std::nullopt;
}

/// The usual install folders, newest version first.
std::vector<Fs> installDirectories() {
    std::vector<Fs> out;
#ifdef _WIN32
    std::error_code ec;
    for (const char* variable : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"}) {
        const char* root = std::getenv(variable);
        if (!root || !*root) {
            continue;
        }
        std::vector<Fs> versions;
        for (const auto& entry : std::filesystem::directory_iterator(Fs(root) / "gs", ec)) {
            if (entry.is_directory(ec)) {
                versions.push_back(entry.path() / "bin");
            }
        }
        std::ranges::sort(versions, [](const Fs& a, const Fs& b) { return a.parent_path().filename() > b.parent_path().filename(); });
        out.insert(out.end(), versions.begin(), versions.end());
    }
#elif defined(__APPLE__)
    out = {"/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin"};
#else
    out = {"/usr/bin", "/usr/local/bin"};
#endif
    return out;
}

std::optional<std::string> configuredSearchPath() {
    std::lock_guard lock(g_searchMutex);
    return g_searchPath;
}

/// Runs Ghostscript, returning its standard output (the picture), or why it did not draw one. The process is killed after
/// kProcessTimeoutMs. Reads untrusted PostScript: -dSAFER keeps it from the file system and other processes.
std::expected<QByteArray, Error> runGhostscript(const Fs& gs, const QStringList& arguments) {
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setProgram(QString::fromStdString(pathToUtf8(gs)));
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(15'000)) {
        return std::unexpected(Error(ErrorCode::IoError, "Ghostscript did not start: " + process.errorString().toStdString()));
    }
    QByteArray output;
    QByteArray errors;
    QElapsedTimer clock;
    clock.start();
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(200);
        output += process.readAllStandardOutput();
        errors += process.readAllStandardError();
        if (errors.size() > (1 << 20)) {
            errors = errors.right(1 << 20);
        }
        if (output.size() > kMaxProcessOutput || clock.elapsed() > kProcessTimeoutMs) {
            process.kill();
            process.waitForFinished(5'000);
            return std::unexpected(Error(ErrorCode::Cancelled, output.size() > kMaxProcessOutput
                                                                   ? "Ghostscript's picture was too large"
                                                                   : "Ghostscript took too long to draw the file"));
        }
    }
    output += process.readAllStandardOutput();
    errors += process.readAllStandardError();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || output.isEmpty()) {
        std::string why = QString::fromUtf8(errors.trimmed().right(400)).toStdString();
        return std::unexpected(Error(ErrorCode::DecoderError, "Ghostscript could not draw the file" + (why.empty() ? "" : ": " + why)));
    }
    return output;
}

/// "%%HiResBoundingBox: x0 y0 x1 y1" or "%%BoundingBox: x0 y0 x1 y1" in PostScript comment text (EPS DSC); nullopt without
/// numbers (e.g. "(atend)").
std::optional<std::pair<double, double>> boundingBoxIn(std::string_view text) {
    for (const std::string_view key : {std::string_view("%%HiResBoundingBox:"), std::string_view("%%BoundingBox:")}) {
        std::size_t at = 0;
        while ((at = text.find(key, at)) != std::string_view::npos) {
            at += key.size();
            const std::size_t end = text.find_first_of("\r\n", at);
            std::string line(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
            std::array<double, 4> v{};
            const char* p = line.c_str();
            bool ok = true;
            for (double& number : v) {
                char* next = nullptr;
                number = std::strtod(p, &next);
                if (next == p) {
                    ok = false;
                    break;
                }
                p = next;
            }
            if (ok && std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::isfinite(v[3]) && v[2] > v[0] &&
                v[3] > v[1]) {
                return std::pair{v[2] - v[0], v[3] - v[1]};
            }
        }
    }
    return std::nullopt;
}

/// The nominal size of a PostScript / EPS file from its DSC header (or trailer: "(atend)").
std::expected<std::pair<double, double>, Error> postScriptSize(const Fs& file) {
    std::string head = bytesOf(file, 256 * 1024);
    if (head.size() >= 12 && static_cast<unsigned char>(head[0]) == 0xC5) { // DOS EPS: the PostScript starts at head[4..7]
        const auto le32 = [&](std::size_t at) {
            return static_cast<std::uint32_t>(static_cast<unsigned char>(head[at])) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(head[at + 1])) << 8) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(head[at + 2])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(head[at + 3])) << 24);
        };
        const std::uint32_t offset = le32(4);
        if (offset < head.size()) {
            head.erase(0, offset);
        }
    }
    if (const auto box = boundingBoxIn(head)) {
        return *box;
    }
    if (const auto box = boundingBoxIn(bytesOf(file, 256 * 1024, true))) {
        return *box;
    }
    return std::unexpected(invalid("the file has no %%BoundingBox, so Wabcut cannot tell how big the artwork is"));
}

/// Ghostscript's own measure of a PDF or PostScript file (-sDEVICE=bbox prints the boxes to standard error).
std::expected<std::pair<double, double>, Error> ghostscriptSize(const Fs& gs, const Fs& file) {
    QStringList arguments{QStringLiteral("-dSAFER"), QStringLiteral("-dBATCH"), QStringLiteral("-dNOPAUSE"),
                          QStringLiteral("-dQUIET"), QStringLiteral("-sDEVICE=bbox"), QStringLiteral("-dFirstPage=1"),
                          QStringLiteral("-dLastPage=1"), QStringLiteral("-dUseCropBox"),
                          QString::fromStdString(pathToUtf8(file))};
    QProcess process;
    process.setProgram(QString::fromStdString(pathToUtf8(gs)));
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::MergedChannels); // the boxes come on standard error; both are read
    process.start();
    if (!process.waitForStarted(15'000) || !process.waitForFinished(kProcessTimeoutMs)) {
        process.kill();
        return std::unexpected(Error(ErrorCode::IoError, "Ghostscript could not measure the file"));
    }
    const QByteArray text = process.readAll();
    if (const auto box = boundingBoxIn(std::string_view(text.constData(), static_cast<std::size_t>(text.size())))) {
        return *box;
    }
    return std::unexpected(Error(ErrorCode::DecoderError, "Ghostscript could not measure the file"));
}

/// Draws a PDF-based or PostScript file with Ghostscript: `info` gives the nominal size, `pixels` the picture.
std::expected<QImage, Error> renderWithGhostscript(const Fs& file, const VectorInfo& info, QSize pixels, bool pdf) {
    const auto gs = findGhostscript();
    if (!gs) {
        return std::unexpected(unsupported(ghostscriptMissingMessage(info.format)));
    }
    // The resolution that makes the picture `pixels` wide. -dEPSCrop draws the file's real %%BoundingBox, which clampedInfo
    // may have cut down to kMaxVectorSide, so it is read again here: a box of 200000 pt drawn at the clamped size's
    // resolution would be many times the pixels asked for.
    double widthPoints = info.width;
    if (!pdf) {
        if (const auto box = postScriptSize(file)) {
            widthPoints = std::max(box->first, 1.0);
        }
    }
    const double dpi = 72.0 * static_cast<double>(pixels.width()) / widthPoints;
    QStringList arguments{QStringLiteral("-dSAFER"),          QStringLiteral("-dBATCH"),
                          QStringLiteral("-dNOPAUSE"),        QStringLiteral("-dQUIET"),
                          QStringLiteral("-sstdout=%stderr"), QStringLiteral("-sDEVICE=pngalpha"),
                          QStringLiteral("-dTextAlphaBits=4"), QStringLiteral("-dGraphicsAlphaBits=4"),
                          QStringLiteral("-r%1").arg(dpi, 0, 'f', 3)};
    if (pdf) {
        arguments << QStringLiteral("-dFirstPage=1") << QStringLiteral("-dLastPage=1") << QStringLiteral("-dUseCropBox");
    } else {
        arguments << QStringLiteral("-dEPSCrop");
    }
    arguments << QStringLiteral("-sOutputFile=-") << QString::fromStdString(pathToUtf8(file));
    auto png = runGhostscript(*gs, arguments);
    if (!png) {
        return std::unexpected(png.error());
    }
    QImage image;
    if (!image.loadFromData(*png, "PNG") || image.isNull()) {
        return std::unexpected(Error(ErrorCode::DecoderError, "Ghostscript's picture could not be read"));
    }
    return image;
}

// ---- Formats ----------------------------------------------------------------------------------------------------------

std::expected<std::vector<std::uint8_t>, Error> readWhole(const Fs& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(Error(ErrorCode::IoError, "the file could not be opened"));
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return bytes;
}

/// The text of an SVG file: a gzip-compressed one (SVGZ) is inflated, at most kMaxSvgBytes.
std::expected<QByteArray, Error> svgText(const Fs& file) {
    auto bytes = readWhole(file);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    if (bytes->size() > kMaxSvgBytes) {
        return std::unexpected(invalid("SVG files over 64 MB are not imported"));
    }
    if (bytes->size() < 2 || (*bytes)[0] != 0x1F || (*bytes)[1] != 0x8B) {
        return QByteArray(reinterpret_cast<const char*>(bytes->data()), static_cast<qsizetype>(bytes->size()));
    }
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 16) != Z_OK) { // 16: gzip only
        return std::unexpected(Error(ErrorCode::ParseError, "the file is not a valid SVG"));
    }
    stream.next_in = bytes->data();
    stream.avail_in = static_cast<uInt>(bytes->size());
    QByteArray out;
    std::array<unsigned char, 64 * 1024> chunk;
    int status = Z_OK;
    while (status == Z_OK) {
        stream.next_out = chunk.data();
        stream.avail_out = static_cast<uInt>(chunk.size());
        status = inflate(&stream, Z_NO_FLUSH);
        out.append(reinterpret_cast<const char*>(chunk.data()), static_cast<qsizetype>(chunk.size() - stream.avail_out));
        if (static_cast<std::uintmax_t>(out.size()) > kMaxSvgBytes) {
            inflateEnd(&stream);
            return std::unexpected(invalid("the SVG is over 64 MB once unpacked, so it is not imported"));
        }
    }
    inflateEnd(&stream);
    if (status != Z_STREAM_END) {
        return std::unexpected(Error(ErrorCode::ParseError, "the file is not a valid SVG"));
    }
    return out;
}

/// Whether an SVG links to anything but itself: an `href` / `xlink:href` that is neither "#id" nor a data: URL. Qt's SVG
/// reader opens an <image>'s link as a file, also another folder's and a network share's (a UNC path starts a login to
/// that server), so such an SVG is refused: embed the picture in the SVG (data:) instead. Over-eager on purpose (a link in
/// a comment counts too).
bool svgLinksElsewhere(const QByteArray& text) {
    static const QRegularExpression link(QStringLiteral(R"re(href\s*=\s*(?:"([^"]*)"|'([^']*)'))re"),
                                         QRegularExpression::CaseInsensitiveOption);
    const QString all = QString::fromUtf8(text);
    for (auto match = link.globalMatch(all); match.hasNext();) {
        const auto m = match.next();
        const QString target = (m.capturedLength(1) > 0 ? m.captured(1) : m.captured(2)).trimmed();
        if (target.isEmpty() || target.startsWith(QLatin1Char('#')) || target.startsWith(QLatin1String("data:"), Qt::CaseInsensitive)) {
            continue;
        }
        return true;
    }
    return false;
}

/// Loads `file` into `renderer` from its bytes (so the file's folder is not where a link would be looked for), after the
/// checks above.
std::expected<void, Error> loadSvg(QSvgRenderer& renderer, const Fs& file) {
    auto text = svgText(file);
    if (!text) {
        return std::unexpected(text.error());
    }
    if (svgLinksElsewhere(*text)) {
        return std::unexpected(invalid("the SVG links to another file or address; embed its pictures in the SVG (data: URLs) instead"));
    }
    if (!renderer.load(*text) || !renderer.isValid()) {
        return std::unexpected(Error(ErrorCode::ParseError, "the file is not a valid SVG"));
    }
    return {};
}

std::expected<VectorInfo, Error> probeSvg(const Fs& file, std::uintmax_t bytes) {
    if (bytes > kMaxSvgBytes) {
        return std::unexpected(invalid("SVG files over 64 MB are not imported"));
    }
    QSvgRenderer renderer;
    if (auto loaded = loadSvg(renderer, file); !loaded) {
        return std::unexpected(loaded.error());
    }
    QSizeF size = QSizeF(renderer.defaultSize()); // (whole pixels; a viewBox in fractions is the fallback)
    if (!(size.width() > 0.0 && size.height() > 0.0)) {
        size = renderer.viewBoxF().size();
    }
    if (!(size.width() > 0.0 && size.height() > 0.0) || !std::isfinite(size.width()) || !std::isfinite(size.height())) {
        return std::unexpected(invalid("the SVG has no size (no width and height, no viewBox)"));
    }
    return clampedInfo(VectorFormat::Svg, size.width(), size.height());
}

std::expected<VectorInfo, Error> probeIllustrator(const Fs& file, std::uintmax_t bytes) {
    const std::string head = bytesOf(file, 2048);
    if (hasPdfHeader(head)) {
#ifdef _WIN32
        auto content = readWhole(file);
        if (!content) {
            return std::unexpected(content.error());
        }
        const auto size = detail::pdfPageSizePoints(*content);
        if (!size) {
            return std::unexpected(Error(ErrorCode::DecoderError, "the Illustrator file could not be read: " + size.error()));
        }
        (void)bytes;
        return clampedInfo(VectorFormat::Illustrator, size->first, size->second);
#else
        (void)bytes;
        const auto gs = findGhostscript();
        if (!gs) {
            return std::unexpected(unsupported(ghostscriptMissingMessage(VectorFormat::Illustrator)));
        }
        const auto size = ghostscriptSize(*gs, file);
        if (!size) {
            return std::unexpected(size.error());
        }
        return clampedInfo(VectorFormat::Illustrator, size->first, size->second);
#endif
    }
    if (hasPostScriptHeader(head)) { // an Illustrator file before CS, or saved as PostScript: drawn by Ghostscript
        if (!findGhostscript()) {
            return std::unexpected(unsupported(ghostscriptMissingMessage(VectorFormat::Illustrator)));
        }
        const auto size = postScriptSize(file);
        if (!size) {
            return std::unexpected(size.error());
        }
        (void)bytes;
        return clampedInfo(VectorFormat::Illustrator, size->first, size->second);
    }
    return std::unexpected(unsupported("this Illustrator file has no PDF-compatible content. In Illustrator choose File > Save As, "
                                       "tick \"Create PDF Compatible File\" and import the copy, or export the artwork as SVG"));
}

std::expected<VectorInfo, Error> probeEps(const Fs& file) {
    const std::string head = bytesOf(file, 64);
    if (!hasPostScriptHeader(head)) {
        return std::unexpected(invalid("the file is not an Encapsulated PostScript file"));
    }
    if (!findGhostscript()) {
        return std::unexpected(unsupported(ghostscriptMissingMessage(VectorFormat::Eps)));
    }
    const auto size = postScriptSize(file);
    if (!size) {
        return std::unexpected(size.error());
    }
    return clampedInfo(VectorFormat::Eps, size->first, size->second);
}

std::expected<QImage, Error> renderSvg(const Fs& file, QSize pixels) {
    QSvgRenderer renderer;
    if (auto loaded = loadSvg(renderer, file); !loaded) {
        return std::unexpected(loaded.error());
    }
    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return std::unexpected(Error(ErrorCode::IoError, "the picture is too large to draw"));
    }
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(pixels)));
    return image;
}

std::expected<QImage, Error> renderIllustrator(const Fs& file, const VectorInfo& info, QSize pixels) {
    const std::string head = bytesOf(file, 2048);
    if (hasPdfHeader(head)) {
#ifdef _WIN32
        auto content = readWhole(file);
        if (!content) {
            return std::unexpected(content.error());
        }
        const auto png = detail::pdfRenderPng(*content, pixels.width(), pixels.height());
        if (!png) {
            return std::unexpected(Error(ErrorCode::DecoderError, "the Illustrator file could not be drawn: " + png.error()));
        }
        QImage image;
        if (!image.loadFromData(png->data(), static_cast<int>(png->size()), "PNG") || image.isNull()) {
            return std::unexpected(Error(ErrorCode::DecoderError, "Windows' picture of the Illustrator file could not be read"));
        }
        return image;
#else
        return renderWithGhostscript(file, info, pixels, true);
#endif
    }
    return renderWithGhostscript(file, info, pixels, false);
}

} // namespace

// ---- Public -------------------------------------------------------------------------------------------------------------

std::string_view toString(VectorFormat format) {
    switch (format) {
    case VectorFormat::Svg:
        return "SVG";
    case VectorFormat::Illustrator:
        return "Illustrator";
    case VectorFormat::Eps:
        return "EPS";
    }
    return "vector";
}

std::optional<VectorFormat> vectorFormatOf(const Fs& file) {
    const std::string extension = lowered(pathToUtf8(file.extension()));
    if (extension == ".svg" || extension == ".svgz") {
        return VectorFormat::Svg;
    }
    if (extension == ".ai") {
        return VectorFormat::Illustrator;
    }
    if (extension == ".eps") {
        return VectorFormat::Eps;
    }
    return std::nullopt;
}

std::optional<Fs> findGhostscript(const std::optional<std::string>& searchPath) {
    const std::vector<std::string> names = ghostscriptNames();
    const std::optional<std::string> given = searchPath ? searchPath : configuredSearchPath();
    if (given) {
        for (const Fs& directory : directoriesOf(*given)) {
            if (const auto found = inDirectory(directory, names)) {
                return found;
            }
        }
        return std::nullopt;
    }
    if (const char* path = std::getenv("PATH")) {
        for (const Fs& directory : directoriesOf(path)) {
            if (const auto found = inDirectory(directory, names)) {
                return found;
            }
        }
    }
    for (const Fs& directory : installDirectories()) {
        if (const auto found = inDirectory(directory, names)) {
            return found;
        }
    }
    return std::nullopt;
}

void setGhostscriptSearchPath(std::optional<std::string> searchPath) {
    std::lock_guard lock(g_searchMutex);
    g_searchPath = std::move(searchPath);
}

std::string ghostscriptMissingMessage(VectorFormat format) {
    const std::string kind = format == VectorFormat::Eps ? "EPS files" : "this Illustrator file";
    return "Importing " + kind + " needs Ghostscript, and Wabcut found none on this computer. Install it (free, from "
           "https://ghostscript.com/releases/gsdnld.html - on macOS \"brew install ghostscript\", on Linux your package "
           "manager's \"ghostscript\") and import the file again - or export the artwork from Illustrator as SVG, which needs "
           "nothing extra.";
}

std::expected<VectorInfo, Error> probeVector(const Fs& file) {
    const auto format = vectorFormatOf(file);
    if (!format) {
        return std::unexpected(unsupported("not an SVG, Illustrator or EPS file"));
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return std::unexpected(Error(ErrorCode::NotFound, "the file does not exist"));
    }
    const std::uintmax_t bytes = std::filesystem::file_size(file, ec);
    if (ec || bytes == 0) {
        return std::unexpected(invalid("the file is empty"));
    }
    if (bytes > kMaxVectorFileBytes) {
        return std::unexpected(invalid("files over 256 MB are not imported"));
    }
    switch (*format) {
    case VectorFormat::Svg:
        return probeSvg(file, bytes);
    case VectorFormat::Illustrator:
        return probeIllustrator(file, bytes);
    case VectorFormat::Eps:
        return probeEps(file);
    }
    return std::unexpected(unsupported("not an SVG, Illustrator or EPS file"));
}

std::expected<QImage, Error> renderVector(const Fs& file, const VectorInfo& info, QSize pixels) {
    if (pixels.width() < 1 || pixels.height() < 1 || pixels.width() > kMaxVectorRenderSide ||
        pixels.height() > kMaxVectorRenderSide) {
        return std::unexpected(invalid("the picture size is out of range"));
    }
    std::expected<QImage, Error> drawn = std::unexpected(unsupported("not an SVG, Illustrator or EPS file"));
    switch (info.format) {
    case VectorFormat::Svg:
        drawn = renderSvg(file, pixels);
        break;
    case VectorFormat::Illustrator:
        drawn = renderIllustrator(file, info, pixels);
        break;
    case VectorFormat::Eps:
        drawn = renderWithGhostscript(file, info, pixels, false);
        break;
    }
    if (!drawn) {
        return drawn;
    }
    QImage image = drawn->convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (image.size() != pixels) {
        // The interpreter rounds its page to whole pixels, a pixel either way.
        image = image.scaled(pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}

QSize vectorRenderSize(const VectorInfo& info, double boxWidth, double boxHeight) {
    const double w = std::max(info.width, 1.0);
    const double h = std::max(info.height, 1.0);
    double scale = std::max(boxWidth / w, boxHeight / h);
    if (!(scale > 0.0) || !std::isfinite(scale)) {
        scale = 1.0;
    }
    // Quarter-octave steps (about 19 %): a box that grows slowly is drawn a handful of times, not every frame.
    scale = std::exp2(std::ceil(std::log2(scale) * 4.0) / 4.0);
    double width = w * scale;
    double height = h * scale;
    const double longest = std::max(width, height);
    if (longest > kMaxVectorRenderSide) {
        width *= kMaxVectorRenderSide / longest;
        height *= kMaxVectorRenderSide / longest;
    }
    return QSize(std::max(1, static_cast<int>(std::lround(width))), std::max(1, static_cast<int>(std::lround(height))));
}

// ---- VectorPictures -------------------------------------------------------------------------------------------------------

VectorPictures& VectorPictures::shared() {
    static VectorPictures pictures;
    return pictures;
}

namespace {

struct FileStamp {
    std::int64_t size = -1;
    std::int64_t mtime = 0;
};

FileStamp stampOf(const Fs& file) {
    std::error_code ec;
    FileStamp stamp;
    if (std::filesystem::is_regular_file(file, ec)) {
        stamp.size = static_cast<std::int64_t>(std::filesystem::file_size(file, ec));
        stamp.mtime = static_cast<std::int64_t>(std::filesystem::last_write_time(file, ec).time_since_epoch().count());
    }
    return stamp;
}

} // namespace

std::expected<VectorInfo, Error> VectorPictures::info(const Fs& file) {
    const std::string path = pathToUtf8(file);
    const FileStamp stamp = stampOf(file);
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_infos.begin(); it != m_infos.end(); ++it) {
            if (it->path == path && it->size == stamp.size && it->mtime == stamp.mtime) {
                m_infos.splice(m_infos.begin(), m_infos, it);
                return m_infos.front().info;
            }
        }
    }
    auto probed = probeVector(file);
    std::lock_guard lock(m_mutex);
    std::erase_if(m_infos, [&](const InfoEntry& e) { return e.path == path; });
    m_infos.push_front(InfoEntry{path, stamp.size, stamp.mtime, probed});
    while (m_infos.size() > kMaxCachedInfos) {
        m_infos.pop_back();
    }
    return probed;
}

QImage VectorPictures::picture(const Fs& file, double boxWidth, double boxHeight) {
    const auto probed = info(file);
    if (!probed) {
        return {};
    }
    const QSize pixels = vectorRenderSize(*probed, boxWidth, boxHeight);
    const std::string path = pathToUtf8(file);
    const FileStamp stamp = stampOf(file);
    {
        std::lock_guard lock(m_mutex);
        for (auto it = m_images.begin(); it != m_images.end(); ++it) {
            if (it->path == path && it->size == stamp.size && it->mtime == stamp.mtime && it->pixels == pixels) {
                m_images.splice(m_images.begin(), m_images, it);
                return m_images.front().image;
            }
        }
    }
    auto drawn = renderVector(file, *probed, pixels);
    QImage image = drawn ? *drawn : QImage();
    std::lock_guard lock(m_mutex);
    ++m_renders;
    // Older sizes of the file go: the one just drawn is the size shown.
    std::erase_if(m_images, [&](const Entry& e) { return e.path == path && (e.size != stamp.size || e.mtime != stamp.mtime); });
    m_images.push_front(Entry{path, stamp.size, stamp.mtime, pixels, image});
    std::int64_t bytes = 0;
    std::size_t kept = 0;
    for (auto it = m_images.begin(); it != m_images.end();) {
        bytes += static_cast<std::int64_t>(it->image.sizeInBytes());
        if (kept >= 1 && (kept >= kMaxCachedPictures || bytes > kMaxCachedBytes)) {
            it = m_images.erase(it);
        } else {
            ++it;
        }
        ++kept;
    }
    return image;
}

std::uint64_t VectorPictures::renders() const {
    std::lock_guard lock(m_mutex);
    return m_renders;
}

void VectorPictures::clear() {
    std::lock_guard lock(m_mutex);
    m_images.clear();
    m_infos.clear();
}

} // namespace wabcut::render
