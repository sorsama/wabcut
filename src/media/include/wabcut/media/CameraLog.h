#pragma once

// Camera log auto-detection (0.2, colour management's "Color Manage Auto Detected Log and Raw Media"; docs/color.md).
// Log footage is never described by the stream's colour tags (FFmpeg reads BT.709 or "unknown"), so media::probe reads
// what the cameras write instead and stores it in VideoStreamInfo::logColorSpace (an InputColorSpace id,
// model/ColorManagement.h):
//
//   * Sony's NonRealTimeMeta XML (XAVC S / HS / XAVC in MP4, and the clip's "<name>M01.XML" sidecar next to it):
//     <Item name="CaptureGammaEquation" value="s-log3-cine"/> with <Item name="CaptureColorPrimaries" .../>.
//   * Key / value metadata a demuxer reports (QuickTime "mdta" keys, Matroska tags): a key about the gamma, transfer or
//     log whose value names a log curve ("LOG-C", "LogC4", "Canon Log 3", "V-Log", "Apple Log", "Log3G10", "S-Log3";
//     0.6: "D-Log", "F-Log", "F-Log2", "N-Log", "S-Log2", "C-Log" / "Canon Log", "L-Log", "REDlogFilm").
//
// Every read is bounded (files are untrusted): at most kMaxMetadataBytes of a file's metadata boxes and of a sidecar.

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace wabcut::media {

/// The most bytes read from a file's metadata boxes or a sidecar while looking for its camera log.
inline constexpr std::size_t kMaxMetadataBytes = 4u << 20;

/// The log a curve name means ("S-Log3.Cine", "slog3", "LOG-C", "LogC4", "C-Log3", "V-Log", "Apple Log", "Log3G10",
/// "D-Log", "F-Log2", "N-Log", "S-Log2", "Canon Log", "L-Log", "REDlogFilm"), as an InputColorSpace id, or empty.
/// `gamut` refines S-Log3 and S-Log2 (S-Gamut / S-Gamut3 or S-Gamut3.Cine). Curves without a space here are empty:
/// DJI's D-Log M (no published equation) and Fujifilm's F-Log2 C (F-Gamut C).
std::string cameraLogFromName(std::string_view curve, std::string_view gamut = {});
/// The log Sony's NonRealTimeMeta XML names (CaptureGammaEquation, CaptureColorPrimaries), or empty.
std::string cameraLogFromXml(std::string_view xml);
/// The log a metadata entry names: a key about the gamma, transfer or log ("com.arri.camera.ColorGammaSxS",
/// "gamma", "TransferCharacteristic", ...) whose value is a log curve; empty otherwise.
std::string cameraLogFromTag(std::string_view key, std::string_view value);
/// The log in a file's own metadata (the MP4 / QuickTime boxes other than the media data, read up to
/// kMaxMetadataBytes) or in its Sony sidecar ("<stem>M01.XML" beside it). Empty when neither names one.
std::string cameraLogFromFile(const std::filesystem::path& file);
/// 0.7 (stabilizing with the camera's gyro): the camera model Sony's NonRealTimeMeta XML names in the file or its sidecar
/// ("ILCE-7M4"), read the same bounded way; empty when there is none.
std::string cameraModelFromFile(const std::filesystem::path& file);

} // namespace wabcut::media
