// OutputMode::Native (zero-copy, docs/zero-copy.md): d3d11va decoders on a shared D3D11 device deliver their
// surfaces as D3D11Texture payloads. Frame accuracy is checked by copying a surface slice to a staging texture and
// reading the burnt-in frame number; the CPU path stays the default and the fallback.

#include "TestMedia.h"

#include <wabcut/media/DecoderPool.h>
#include <wabcut/media/VideoDecoder.h>

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)
#include <d3d11.h>
#endif

#include <memory>
#include <vector>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;

#if defined(_WIN32)

namespace {

constexpr const char* kClip = "h264_1080p60_aac.mp4";

struct SharedDevice {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    DeviceToken token;

    ~SharedDevice() {
        (void)setSharedD3D11Device(nullptr, nullptr); // later tests get media's own device again
        if (context) {
            context->Release();
        }
        if (device) {
            device->Release();
        }
    }
};

/// A device like the renderer's (video support), installed as the shared device. Null when unavailable.
std::unique_ptr<SharedDevice> shareNewDevice() {
    auto d = std::make_unique<SharedDevice>();
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                 D3D11_SDK_VERSION, &d->device, nullptr, &d->context))) {
        return nullptr;
    }
    auto token = setSharedD3D11Device(d->device, d->context);
    if (!token) {
        return nullptr;
    }
    d->token = *token;
    return d;
}

/// The frame number burnt into an NV12 surface slice (copied to a staging texture and mapped).
std::int64_t surfaceBarcode(ID3D11Device* device, ID3D11DeviceContext* context, const D3D11Texture& native) {
    auto* texture = static_cast<ID3D11Texture2D*>(native.texture);
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.ArraySize = 1;
    stagingDesc.MipLevels = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
        return -1;
    }
    context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture,
                                   D3D11CalcSubresource(0, native.arraySlice, desc.MipLevels), nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    std::int64_t index = -1;
    if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        index = 0;
        const auto* luma = static_cast<const std::uint8_t*>(mapped.pData) + 16 * mapped.RowPitch;
        for (int bit = 0; bit < 16; ++bit) {
            if (luma[bit * 32 + 16] > 128) {
                index |= std::int64_t{1} << bit;
            }
        }
        context->Unmap(staging, 0);
    }
    staging->Release();
    return index;
}

Ticks frameTime(const VideoDecoder& decoder, std::int64_t frame) {
    return decoder.firstFrameTime() + Ticks{frame * (kTicksPerSecond / 60)};
}

} // namespace

TEST_CASE("Native output delivers frame-exact D3D11 surfaces on the shared device", "[media][native][gpu]") {
    if (!gpuExpected() || !haveMedia(kClip)) {
        SKIP("no GPU expected or test media missing");
    }
    auto shared = shareNewDevice();
    REQUIRE(shared);
    CHECK_FALSE(shared->token.isCpu());
    CHECK(sharedD3D11Device() == shared->token);

    DecoderOptions options;
    options.outputMode = OutputMode::Native;
    auto decoder = VideoDecoder::open(mediaPath(kClip), -1, options);
    REQUIRE(decoder.has_value());
    CHECK((*decoder)->backendName() == "d3d11va"); // preferred for zero-copy over the readback policy

    std::vector<FrameHandle> held; // retention: consumers hold frames while decoding goes on
    for (std::int64_t n = 37; n < 37 + 60; ++n) {
        auto frame = (*decoder)->frameAt(frameTime(**decoder, n));
        REQUIRE(frame.has_value());
        REQUIRE(frame->payloadKind() == PayloadKind::D3D11Texture);
        const auto& native = std::get<D3D11Texture>(frame->payload);
        CHECK(native.device == shared->token);
        CHECK(frame->device() == shared->token);
        CHECK(frame->format == PixelFormat::NV12);
        if (n == 37 || n == 50 || n == 96) {
            auto* texture = static_cast<ID3D11Texture2D*>(native.texture);
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            CHECK(desc.Format == DXGI_FORMAT_NV12);
            CHECK((desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0);
            CHECK(native.arraySlice < desc.ArraySize);
            ID3D11Device* owner = nullptr;
            texture->GetDevice(&owner);
            CHECK(owner == shared->device);
            owner->Release();
            CHECK(surfaceBarcode(shared->device, shared->context, native) == n);
        }
        held.push_back(*frame);
        if (held.size() > 16) {
            held.erase(held.begin()); // hold the last 16 (DecoderOptions::nativeSurfaces default)
        }
    }
    // A random access lands on the right frame too.
    auto back = (*decoder)->frameAt(frameTime(**decoder, 12));
    REQUIRE(back.has_value());
    REQUIRE(back->payloadKind() == PayloadKind::D3D11Texture);
    CHECK(surfaceBarcode(shared->device, shared->context, std::get<D3D11Texture>(back->payload)) == 12);
}

TEST_CASE("Native output falls back to CPU planes without a shared device", "[media][native][gpu]") {
    if (!gpuExpected() || !haveMedia(kClip)) {
        SKIP("no GPU expected or test media missing");
    }
    (void)setSharedD3D11Device(nullptr, nullptr);
    CHECK(sharedD3D11Device().isCpu());
    DecoderOptions options;
    options.outputMode = OutputMode::Native;
    auto decoder = VideoDecoder::open(mediaPath(kClip), -1, options);
    REQUIRE(decoder.has_value());
    auto frame = (*decoder)->frameAt(frameTime(**decoder, 10));
    REQUIRE(frame.has_value());
    CHECK(frame->payloadKind() == PayloadKind::CpuPlanes);
    CHECK(frameIndexOf(*frame) == 10);
}

TEST_CASE("Cpu output is unchanged while a device is shared", "[media][native][gpu]") {
    if (!gpuExpected() || !haveMedia(kClip)) {
        SKIP("no GPU expected or test media missing");
    }
    auto shared = shareNewDevice();
    REQUIRE(shared);
    auto decoder = VideoDecoder::open(mediaPath(kClip), -1, DecoderOptions{});
    REQUIRE(decoder.has_value());
    auto frame = (*decoder)->frameAt(frameTime(**decoder, 20));
    REQUIRE(frame.has_value());
    CHECK(frame->payloadKind() == PayloadKind::CpuPlanes);
    CHECK(frameIndexOf(*frame) == 20);
}

TEST_CASE("DecoderPool hands native frames through", "[media][native][gpu]") {
    if (!gpuExpected() || !haveMedia(kClip)) {
        SKIP("no GPU expected or test media missing");
    }
    auto shared = shareNewDevice();
    REQUIRE(shared);
    DecoderPoolConfig config;
    config.decoderOptions.outputMode = OutputMode::Native;
    DecoderPool pool(config);
    FrameRequest request;
    request.path = mediaPath(kClip);
    request.time = Ticks{25 * (kTicksPerSecond / 60)};
    auto frame = pool.requestFrame(request).wait();
    REQUIRE(frame.has_value());
    REQUIRE(frame->payloadKind() == PayloadKind::D3D11Texture);
    CHECK(surfaceBarcode(shared->device, shared->context, std::get<D3D11Texture>(frame->payload)) == 25);
}

#endif // _WIN32
