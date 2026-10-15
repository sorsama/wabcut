# wabcut_ai_translate — caption translation (AI-12, docs/ai.md "Going global"): languages and their reading speeds,
# re-segmentation of translated cues, "[bleep]"-safe translation through any ITranslator (the user's chat model via
# LlmTranslator, or a local model), the corrections layer before translation, and captions.translate. Pure C++ (model +
# command + ai_edit), so it builds and is tested in the core preset. Owner: AI platform engineer.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(wabcut_ai_translate STATIC
    src/CaptionTranslation.cpp
    src/Segmentation.cpp
    src/TranslateCommands.cpp)
target_include_directories(wabcut_ai_translate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_translate PUBLIC wabcut_ai_mt wabcut_ai_edit wabcut_command wabcut_model wabcut_ai
    PRIVATE nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(wabcut_ai_translate PRIVATE usp10) # Uniscribe: Thai line breaks without Qt
endif()
wabcut_target_defaults(wabcut_ai_translate)

# wabcut_ai_mt — local caption translation (AI-12): the languages (Languages.h), OPUS-MT's tokenizer (MarianTokenizer,
# also used by the wabcut-embed helper), the language pairs and their pinned model files, and LocalTranslator, the
# ITranslator that runs a model in the helper (EmbedProtocol.h "translate_text"). Pure C++ (ai + ai_search), core preset.
add_library(wabcut_ai_mt STATIC
    src/Languages.cpp
    src/LocalTranslation.cpp
    src/MarianTokenizer.cpp)
target_include_directories(wabcut_ai_mt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_mt PUBLIC wabcut_ai_search wabcut_ai wabcut_model PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_ai_mt)
