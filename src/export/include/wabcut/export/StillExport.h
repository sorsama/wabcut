#pragma once

// Export Frame (P.2 E6, Premiere's File > Export > Frame and the monitors' camera button; docs/monitors.md): one frame
// of a sequence rendered as an export renders it (the playback evaluator, the original media, render::OffscreenRenderer
// with the export output transform: the pixels an export of that frame holds) and written as a still image.
//
// Formats (Premiere's Export Frame list):
//   * DPX (10-bit RGB), JPEG (4:4:4, high quality), TIFF (8-bit RGB, LZW), Targa (24-bit), BMP (24-bit) and GIF
//     (256 colours, ordered 6x7x6 palette with error diffusion) through FFmpeg's image encoders;
//   * PNG (8-bit RGB) through Qt's writer: the FFmpeg build has no zlib, so no PNG encoder;
//   * OpenEXR through Wabcut's own writer (FFmpeg's exr encoder needs zlib too): an uncompressed scanline file of half
//     floats, R G B A, in linear light (the 8-bit Rec.709 values through the inverse BT.709 OETF, as the export output
//     transform encoded them).
//
// Threading: renderStill() runs on the calling thread (it needs a QGuiApplication in the process, like an export) and
// creates its renderer each time; writeStill() is pure CPU work.

#include <wabcut/command/FrameServices.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>

#include <QImage>

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::exporter {

enum class StillFormat { Dpx, Jpeg, Png, Tiff, Targa, Bmp, Gif, OpenExr };

/// "dpx", "jpeg", "png", "tiff", "targa", "bmp", "gif", "openexr".
std::string_view stillFormatName(StillFormat format);
std::optional<StillFormat> stillFormatFromName(std::string_view name);
/// ".dpx", ".jpg", ".png", ".tif", ".tga", ".bmp", ".gif", ".exr".
std::string_view stillExtension(StillFormat format);
/// Every format, in Premiere's menu order (DPX first).
std::vector<StillFormat> stillFormats();

/// The frame of `sequence` at `time` (snapped to its frame), at the sequence's size, RGBA8888 (opaque). Errors: NotFound
/// (no such sequence), DeviceError (no renderer), DecoderError / IoError (offline or unreadable media).
std::expected<QImage, Error> renderStill(const ProjectState& state, SequenceId sequence, Ticks time);

/// Writes `picture` (RGBA8888 or convertible) to `file` as `format`; the file is replaced. Errors: InvalidArgument (an
/// empty picture), Unsupported (no encoder), IoError.
std::expected<void, Error> writeStill(const QImage& picture, const std::filesystem::path& file, StillFormat format);

/// P1 (#23): a size, a JPEG quality and a size cap for a still (command::FrameExportServices::Options).
using StillOptions = command::FrameExportServices::Options;

/// `picture` scaled to `width` x `height` (smooth): `fill` crops its centre to fill the frame, else it fits whole with
/// black bars around it. 0 x 0, or the picture's own size: unchanged.
QImage scaleStill(const QImage& picture, int width, int height, bool fill);

/// P1 (#23): writeStill() with `options`: scaled first (scaleStill), JPEG at options.quality (1..100; 0 = high, 95),
/// and with options.maxBytes a JPEG is written again at lower qualities (down to 30) until it fits. Returns what was
/// written: its size in pixels and bytes, and the JPEG quality used (Written::bytes > maxBytes: it never fit). Same
/// errors as writeStill(), plus InvalidArgument for a bad size.
std::expected<command::FrameExportServices::Written, Error>
writeStill(const QImage& picture, const std::filesystem::path& file, StillFormat format, const StillOptions& options);

/// The JPEG quality (1..100) as the encoder's quantiser scale (2 = best .. 31).
int jpegQscale(int quality);

/// The command services behind sequence.exportFrame / source.exportFrame: renderStill + writeStill, and a describer
/// that turns the written file into a media item to import (the host's probe and locator rules; empty: no import).
std::shared_ptr<command::FrameExportServices> stillExportServices(command::FrameExportServices::Describer describer = {});

} // namespace wabcut::exporter
