#pragma once

// The real SpeechAudio (AI-12): the providers' MP3 / WAV decoded by FFmpeg (wabcut_media's AudioDecoder), lines sped up
// by the audio engine's WSOLA stretcher (pitch kept, the one clips at speed play through), mono 32-bit float WAV files
// probed and fingerprinted like any imported media. Full builds only (wabcut_ai_dub_media).

#include <wabcut/ai/dub/Pipeline.h>

#include <filesystem>
#include <memory>

namespace wabcut::ai::dub {

/// `scratch`: where the providers' audio is put for decoding (deleted right after).
std::shared_ptr<SpeechAudio> makeMediaSpeechAudio(std::filesystem::path scratch);

/// Offline WSOLA: `samples` (mono) played `speed` times as fast; round(size / speed) frames.
std::vector<float> stretchOffline(std::span<const float> samples, int rate, double speed);

} // namespace wabcut::ai::dub
