#pragma once

// The wire between the editor and wabcut-ofxhost, the helper process that runs OpenFX video effect plug-ins (0.7,
// docs/plugins.md, "OpenFX video plug-ins"). It follows the audio plug-in host's wire (src/plughost): two channels.
//
//   * control: JSON lines over the helper's stdin/stdout (Ipc.h of wabcut_plughost_common). Every request carries an "id"
//     the reply repeats. Requests: hello, create, attach, render, destroy, quit. Replies: {"id": n, "ok": true, ...} or
//     {"id": n, "ok": false, "error": "..."}.
//   * pictures: one shared-memory block per instance (FrameHeader below), holding the picture the editor wants processed
//     and the picture the plug-in made, both RGBA float32, premultiplied, rows top-down, tightly packed (`width` pixels
//     per row). The editor writes the input, sends "render", waits for the reply and reads the output: the control
//     channel is the synchronisation, so there are no events and no atomics in the block. When a picture is larger than
//     the block's capacity the editor makes a new block and sends "attach".
//
// What the helper does with a picture is the OpenFX host's business (OfxHost.cpp): it converts to the pixel depth and
// components the plug-in asked for, flips the rows (OpenFX images start at the bottom), and converts back.
//
// One helper process per plug-in binary (a .ofx bundle), as the audio host does: a crash takes down that binary's
// instances only.

#include <cstddef>
#include <cstdint>
#include <string>

namespace wabcut::ofxhost {

inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::uint32_t kFrameMagic = 0x3158464F; // "OFX1"

/// Largest picture the host processes: more pixels than this (a 4096 x 4096 frame) is not offered to plug-ins (a float
/// RGBA block of that size is 256 MiB, twice for input and output).
inline constexpr std::uint64_t kMaxFramePixels = 4096ull * 4096ull;
/// Largest side of a picture, in pixels.
inline constexpr int kMaxFrameSide = 16384;
/// Bytes of one pixel of a shared picture: RGBA float32.
inline constexpr std::size_t kPixelBytes = 16;

struct FrameHeader {
    std::uint32_t magic = kFrameMagic;
    std::uint32_t version = kProtocolVersion;
    std::uint64_t capacityPixels = 0; ///< the pixels each of the two pictures can hold
    std::uint32_t reserved[12]{};
};
static_assert(sizeof(FrameHeader) == 64, "the picture block's header is 64 bytes");

/// Offsets of the input and output pictures in a block of `capacityPixels` pixels per picture.
constexpr std::size_t inputOffset() noexcept { return sizeof(FrameHeader); }
constexpr std::size_t outputOffset(std::uint64_t capacityPixels) noexcept {
    return sizeof(FrameHeader) + static_cast<std::size_t>(capacityPixels) * kPixelBytes;
}
constexpr std::size_t blockBytes(std::uint64_t capacityPixels) noexcept {
    return sizeof(FrameHeader) + 2u * static_cast<std::size_t>(capacityPixels) * kPixelBytes;
}

/// The name of the shared block of instance `instance` (generation `generation`: a larger block replaces a smaller one)
/// of host process `hostPid`.
std::string blockName(std::uint32_t hostPid, std::uint64_t instance, std::uint32_t generation);

/// What the plug-in said about the picture it made (clip preferences).
enum class OutputAlpha : std::uint8_t { Opaque, Premultiplied, Unpremultiplied };

/// Limits on what a scan or a helper reply may contain (a plug-in's own descriptions are untrusted input).
inline constexpr std::size_t kMaxParams = 256;
inline constexpr std::size_t kMaxOptions = 512;
inline constexpr std::size_t kMaxNameBytes = 256;
inline constexpr std::size_t kMaxTextBytes = 4096;

} // namespace wabcut::ofxhost
