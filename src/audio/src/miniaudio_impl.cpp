// The single translation unit that compiles miniaudio's implementation (the vcpkg port is header-only).
// miniaudio.h comes from a SYSTEM include directory, so its code does not trip /W4 /WX. The MA_NO_* options are
// set for the whole wabcut_audio target in CMakeLists.txt so every TU sees the same declarations.
//
// Also home to two helpers that need miniaudio's internal backend wrappers, which are only visible here.

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

/// Frames the device keeps queued ahead of a freshly written one, in the device's internal sample rate.
extern "C" ma_uint32 wabcut_ma_buffer_frames(const ma_device* pDevice) {
#if defined(MA_HAS_WASAPI)
    // WASAPI: the real endpoint buffer (internalPeriods * internalPeriodSizeInFrames is only the request).
    if (pDevice->pContext->backend == ma_backend_wasapi && pDevice->wasapi.actualBufferSizeInFramesPlayback != 0) {
        return pDevice->wasapi.actualBufferSizeInFramesPlayback;
    }
#endif
    return pDevice->playback.internalPeriodSizeInFrames * pDevice->playback.internalPeriods;
}

/// Latency the OS adds after our buffer, in device frames (WASAPI only; 0 elsewhere). Must run on a thread where
/// miniaudio has initialised COM (its device thread, e.g. inside the "started" notification).
///
/// IAudioClient::GetStreamLatency is used when the driver reports it. Many shared-mode endpoints report 0
/// (observed on Windows 11 for Realtek, NVIDIA HDMI and VB-Audio endpoints), yet the Windows audio engine still
/// reads our buffer one engine period at a time and mixes it into the endpoint buffer, so at least one device
/// period passes before a sample leaves the engine. In that case the default device period is the estimate
/// (a lower bound; DAC/driver latency beyond it is what AudioDeviceConfig::latencyOffset calibrates).
///
/// `*measured` is set to 1 when the driver reported the value, 0 when it is the engine-period estimate (or 0).
extern "C" ma_uint32 wabcut_ma_stream_latency_frames(ma_device* pDevice, int* measured) {
    *measured = 0;
#if defined(MA_HAS_WASAPI)
    if (pDevice->pContext->backend == ma_backend_wasapi && pDevice->wasapi.pAudioClientPlayback != nullptr) {
        auto* client = static_cast<ma_IAudioClient*>(pDevice->wasapi.pAudioClientPlayback);
        MA_REFERENCE_TIME latency = 0; // 100-ns units
        if (SUCCEEDED(ma_IAudioClient_GetStreamLatency(client, &latency)) && latency > 0) {
            *measured = 1;
        } else {
            MA_REFERENCE_TIME minimumPeriod = 0;
            if (FAILED(ma_IAudioClient_GetDevicePeriod(client, &latency, &minimumPeriod))) {
                latency = 0;
            }
        }
        if (latency > 0) {
            return static_cast<ma_uint32>((latency * static_cast<MA_REFERENCE_TIME>(pDevice->playback.internalSampleRate)) /
                                          10'000'000);
        }
    }
#else
    (void)pDevice;
#endif
    return 0;
}
