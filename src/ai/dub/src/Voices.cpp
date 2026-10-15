#include <wabcut/ai/dub/Voices.h>

#include <algorithm>
#include <cctype>

namespace wabcut::ai::dub {

std::vector<VoiceInfo> builtinVoices(std::string_view api) {
    if (api == "openai-speech") {
        // OpenAI describes none of them; it recommends marin and cedar for the best quality.
        std::vector<VoiceInfo> voices;
        for (const char* id : {"marin", "cedar", "alloy", "ash", "ballad", "coral", "echo", "fable", "onyx", "nova", "sage", "shimmer", "verse"}) {
            std::string name = id;
            name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            voices.push_back({id, name, std::string_view(id) == "marin" || std::string_view(id) == "cedar" ? "recommended" : ""});
        }
        return voices;
    }
    if (api == "gemini-tts") {
        return {{"Kore", "Kore", "firm"},           {"Puck", "Puck", "upbeat"},         {"Charon", "Charon", "informative"},
                {"Zephyr", "Zephyr", "bright"},     {"Fenrir", "Fenrir", "excitable"},  {"Leda", "Leda", "youthful"},
                {"Orus", "Orus", "firm"},           {"Aoede", "Aoede", "breezy"},       {"Callirrhoe", "Callirrhoe", "easy-going"},
                {"Autonoe", "Autonoe", "bright"},   {"Enceladus", "Enceladus", "breathy"}, {"Iapetus", "Iapetus", "clear"},
                {"Umbriel", "Umbriel", "easy-going"}, {"Algieba", "Algieba", "smooth"}, {"Despina", "Despina", "smooth"},
                {"Erinome", "Erinome", "clear"},    {"Algenib", "Algenib", "gravelly"}, {"Rasalgethi", "Rasalgethi", "informative"},
                {"Laomedeia", "Laomedeia", "upbeat"}, {"Achernar", "Achernar", "soft"}, {"Alnilam", "Alnilam", "firm"},
                {"Schedar", "Schedar", "even"},     {"Gacrux", "Gacrux", "mature"},     {"Pulcherrima", "Pulcherrima", "forward"},
                {"Achird", "Achird", "friendly"},   {"Zubenelgenubi", "Zubenelgenubi", "casual"},
                {"Vindemiatrix", "Vindemiatrix", "gentle"}, {"Sadachbia", "Sadachbia", "lively"},
                {"Sadaltager", "Sadaltager", "knowledgeable"}, {"Sulafat", "Sulafat", "warm"}};
    }
    if (api == "elevenlabs-tts") {
        return {{"JBFqnCBsd6RMkjVDRZzb", "George", "warm narrator (premade)"}};
    }
    return {};
}

std::string defaultVoice(std::string_view api) {
    const std::vector<VoiceInfo> voices = builtinVoices(api);
    return voices.empty() ? std::string() : voices.front().id;
}

bool isValidVoice(std::string_view api, std::string_view voice) {
    if (voice.empty()) {
        return false;
    }
    if (api == "elevenlabs-tts") {
        return voice.size() <= 64 && std::all_of(voice.begin(), voice.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
    }
    const std::vector<VoiceInfo> voices = builtinVoices(api);
    return std::any_of(voices.begin(), voices.end(), [&](const VoiceInfo& v) { return v.id == voice; });
}

} // namespace wabcut::ai::dub
