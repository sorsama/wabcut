#pragma once

// The voices the Dub and Narration dialogs offer per voice API (AI-12), as documented on 2026-10-12:
//   openai-speech   the 13 built-in voices of gpt-4o-mini-tts (platform.openai.com/docs/guides/text-to-speech)
//   gemini-tts      the prebuilt voices of the Gemini TTS models (ai.google.dev/gemini-api/docs/speech-generation);
//                   the 3.8 models list 150+ more at /v1beta/voices
//   elevenlabs-tts  a default premade voice; any voice id of the user's own library can be typed in
// Every voice speaks every language its model supports (the language follows the text).

#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::dub {

struct VoiceInfo {
    std::string id;
    std::string name;
    std::string description;
};

std::vector<VoiceInfo> builtinVoices(std::string_view api);
/// The voice a run uses when none is chosen; empty for an unknown api.
std::string defaultVoice(std::string_view api);
/// Whether `voice` can be sent to `api` (a known voice, or for ElevenLabs any 1..64 letters and digits).
bool isValidVoice(std::string_view api, std::string_view voice);

} // namespace wabcut::ai::dub
