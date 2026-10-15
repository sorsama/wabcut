# wabcut_ai_search — smart search over footage and transcripts (AI-8, docs/ai.md "Smart search"): the SigLIP 2 tokenizer,
# the wabcut-embed protocol and HelperEmbedder (the model runs out of process), shot-aware keyframes, the search index
# (SQLite FTS5 trigram for transcript phrases, hnswlib for keyframe vectors), hybrid ranking, FootageSearch and the
# search.* commands. Pure C++ (model + command + ai + ai_analysis), so it builds and is tested in the core preset.
# Owner: AI platform engineer.
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(hnswlib CONFIG REQUIRED)

# CMake 4.3's FindSQLite3 names the target SQLite3::SQLite3 (SQLite::SQLite3 is deprecated); older ones only the latter.
if(TARGET SQLite3::SQLite3)
    set(wabcut_sqlite_target SQLite3::SQLite3)
else()
    set(wabcut_sqlite_target SQLite::SQLite3)
endif()

add_library(wabcut_ai_search STATIC
    src/Embedding.cpp
    src/EmbedHelper.cpp
    src/EmbedProtocol.cpp
    src/FootageSearch.cpp
    src/HelperEmbedder.cpp
    src/Keyframes.cpp
    src/Ranking.cpp
    src/SearchCommands.cpp
    src/SearchIndex.cpp
    src/TextQuery.cpp
    src/Tokenizer.cpp)
target_include_directories(wabcut_ai_search PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(wabcut_ai_search PUBLIC wabcut_ai wabcut_ai_analysis wabcut_command wabcut_model
    PRIVATE nlohmann_json::nlohmann_json ${wabcut_sqlite_target} hnswlib::hnswlib)
wabcut_target_defaults(wabcut_ai_search)

# wabcut_ai_search_media — MediaSearchAccess: fingerprints, shots, decoded keyframes and cached transcripts of the
# project's files. Full builds only; the core stays FFmpeg-free.
if(NOT WABCUT_CORE_ONLY)
    add_library(wabcut_ai_search_media STATIC src/MediaSearchAccess.cpp)
    target_link_libraries(wabcut_ai_search_media PUBLIC wabcut_ai_search wabcut_ai_analysis_media wabcut_ai_edit_media wabcut_media)
    wabcut_target_defaults(wabcut_ai_search_media)
endif()
