#pragma once

// Zero-copy decode -> render (docs/zero-copy.md): hardware decoders opened after shareDeviceWithDecoders() decode on
// the renderer's own GPU device, and with media::OutputMode::Native their frames reach the Compositor as decoder
// surfaces it samples in place (FrameImporter's D3D11 path). Frames from any other device keep using the CPU path.

#include <wabcut/media/FrameHandle.h>
#include <wabcut/model/Error.h>

#include <expected>

class QRhi;

namespace wabcut::render {

/// Shares `rhi`'s device with media's hardware decoders. D3D11 only today (Unsupported elsewhere); the device must
/// support video decoding. Returns the token native frames from that device carry.
std::expected<media::DeviceToken, Error> shareDeviceWithDecoders(QRhi* rhi);

/// True when `rhi` is the device decoders currently share (its native frames import without copies).
bool decodersShareDevice(QRhi* rhi);

} // namespace wabcut::render
