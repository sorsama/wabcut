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
    // r07 (developers.deepgram.com/docs/tts-models): a voice is a model per language; the voice speaks that language.
    if (api == "deepgram-speak") {
        return {{"aura-2-thalia-en", "Thalia", "English, clear and energetic"},
                {"aura-2-apollo-en", "Apollo", "English, confident and casual"},
                {"aura-2-athena-en", "Athena", "English, calm and professional"},
                {"aura-2-orion-en", "Orion", "English, approachable and calm"},
                {"aura-2-draco-en", "Draco", "English (British), baritone"},
                {"aura-2-pandora-en", "Pandora", "English (British), calm"},
                {"aura-2-zeus-en", "Zeus", "English, deep and trustworthy"},
                {"aura-2-luna-en", "Luna", "English, friendly and natural"},
                {"aura-2-celeste-es", "Celeste", "Spanish (Colombian)"},
                {"aura-2-nestor-es", "Nestor", "Spanish (Peninsular)"},
                {"aura-2-julius-de", "Julius", "German"},
                {"aura-2-viktoria-de", "Viktoria", "German"},
                {"aura-2-agathe-fr", "Agathe", "French"},
                {"aura-2-hector-fr", "Hector", "French"},
                {"aura-2-izanami-ja", "Izanami", "Japanese"},
                {"aura-2-fujin-ja", "Fujin", "Japanese"}};
    }
    // r07 (docs.cartesia.ai/build-with-cartesia/tts-models/latest): stock voices; any voice id of the account works too.
    if (api == "cartesia-tts") {
        return {{"db6b0ed5-d5d3-463d-ae85-518a07d3c2b4", "Skylar", "US English, female"},
                {"47c38ca4-5f35-497b-b1a3-415245fb35e1", "Daniel", "US English, male"},
                {"9626c31c-bec5-4cca-baa8-f8ba9e84c8bc", "Jacqueline", "US English, female"},
                {"62ae83ad-4f6a-430b-af41-a9bede9286ca", "Gemma", "British English, female"},
                {"ef191366-f52f-447a-a398-ed8c0f2943a1", "Archie", "British English, male"}};
    }
    // r07 (docs.fish.audio/features/text-to-speech/drama-3): public voices; any voice model id works too.
    if (api == "fish-tts") {
        return {{"bf322df2096a46f18c579d0baa36f41d", "Adrian", "US English, male"},
                {"c70cfb98867a4bfb9ae20d2401257b92", "Delia", "US English, female"},
                {"f3a2b90078d54a65af4b95f63bc7798e", "Sheila", "US English, female"}};
    }
    return {};
}

std::string defaultVoice(std::string_view api) {
    const std::vector<VoiceInfo> voices = builtinVoices(api);
    return voices.empty() ? std::string() : voices.front().id;
}

namespace {

/// A model of OpenAI's own speech API rather than a server that copies it.
bool openAiServer(std::string_view api, std::string_view provider) {
    return api == "openai-speech" && !provider.empty() && provider != "openai";
}

} // namespace

bool acceptsAnyVoice(std::string_view api, std::string_view provider) {
    return api == "elevenlabs-tts" || api == "cartesia-tts" || api == "fish-tts" || openAiServer(api, provider);
}

std::vector<VoiceInfo> voicesFor(std::string_view api, std::string_view provider) {
    if (openAiServer(api, provider)) {
        std::vector<VoiceInfo> voices;
        for (const char* id : {"alloy", "echo", "fable", "onyx", "nova", "shimmer"}) {
            std::string name = id;
            name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
            voices.push_back({id, name, "OpenAI name; the server may use its own"});
        }
        return voices;
    }
    return builtinVoices(api);
}

std::string defaultVoice(std::string_view api, std::string_view provider) {
    const std::vector<VoiceInfo> voices = voicesFor(api, provider);
    return voices.empty() ? std::string() : voices.front().id;
}

bool isValidVoice(std::string_view api, std::string_view voice, std::string_view provider) {
    if (openAiServer(api, provider)) { // the server's own voice names: one line of printable text
        return !voice.empty() && voice.size() <= 128 && voice.find_first_not_of(' ') != std::string_view::npos &&
               std::all_of(voice.begin(), voice.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x20 && c != 0x7f; });
    }
    return isValidVoice(api, voice);
}

bool isValidVoice(std::string_view api, std::string_view voice) {
    if (voice.empty()) {
        return false;
    }
    if (api == "elevenlabs-tts") {
        return voice.size() <= 64 && std::all_of(voice.begin(), voice.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
    }
    if (api == "cartesia-tts" || api == "fish-tts") { // r07: any voice of the user's account (a UUID / 32 hex digits)
        return voice.size() <= 64 && std::all_of(voice.begin(), voice.end(), [](char c) {
                   return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-';
               });
    }
    const std::vector<VoiceInfo> voices = builtinVoices(api);
    return std::any_of(voices.begin(), voices.end(), [&](const VoiceInfo& v) { return v.id == voice; });
}

} // namespace wabcut::ai::dub
