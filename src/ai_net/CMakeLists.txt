# AI network layer (owner: AI platform engineer; contract docs/ai.md "Provider contract").
#
# wabcut_ai_adapters: the cloud wire adapters (Anthropic Messages, OpenAI Responses, Chat Completions, Gemini
#   Interactions, Deepgram, ElevenLabs Scribe, AssemblyAI). Written against IHttpTransport only: no Qt, so the
#   recorded-fixture tests drive them with a fake transport.
# wabcut_ai_net: QtHttpTransport (QNetworkAccessManager on its own thread) and the NetworkStack the app uses.
add_library(wabcut_ai_adapters STATIC
    src/AdapterUtil.cpp
    src/AdapterUtil.h
    src/AnthropicChat.cpp
    src/ChatCompletionsChat.cpp
    src/GeminiInteractionsChat.cpp
    src/OpenAiResponsesChat.cpp
    src/MediaGenerators.cpp
    src/SoundEffects.cpp
    src/SpeechSynths.cpp # AI-12: ElevenLabs, OpenAI and Gemini text-to-speech
    src/Transcribers.cpp)
target_include_directories(wabcut_ai_adapters PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_adapters PUBLIC wabcut_ai)
wabcut_target_defaults(wabcut_ai_adapters)

if(NOT WABCUT_CORE_ONLY)
find_package(Qt6 REQUIRED COMPONENTS Network)
add_library(wabcut_ai_net STATIC src/QtHttpTransport.cpp)
target_link_libraries(wabcut_ai_net PUBLIC wabcut_ai_adapters PRIVATE Qt6::Network)
wabcut_target_defaults(wabcut_ai_net)

endif()
