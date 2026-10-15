#include <wabcut/media/VideoDecoder.h>
#include <wabcut/render/ZeroCopy.h>

#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <atomic>

namespace wabcut::render {

namespace {
/// The QRhi whose device was shared last (identity only; never dereferenced).
std::atomic<const QRhi*> g_shared{nullptr};
} // namespace

std::expected<media::DeviceToken, Error> shareDeviceWithDecoders(QRhi* rhi) {
#if defined(_WIN32)
    if (rhi && rhi->backend() == QRhi::D3D11) {
        const auto* handles = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
        if (!handles || !handles->dev || !handles->context) {
            return std::unexpected(Error{ErrorCode::DeviceError, "The QRhi exposes no D3D11 device"});
        }
        auto token = media::setSharedD3D11Device(handles->dev, handles->context);
        if (token) {
            g_shared.store(rhi);
        }
        return token;
    }
#endif
    (void)rhi;
    return std::unexpected(Error{ErrorCode::Unsupported, "Zero-copy needs the D3D11 backend"});
}

bool decodersShareDevice(QRhi* rhi) {
    return rhi && g_shared.load() == rhi && !media::sharedD3D11Device().isCpu();
}

} // namespace wabcut::render
