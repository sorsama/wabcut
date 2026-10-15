# Pure-C++ dependencies of the core (model, command, keymap, interchange, ai and their tests) when they do not come from
# vcpkg. Included by the root CMakeLists.txt when WABCUT_FETCH_DEPENDENCIES is ON (the `core` preset).
#
# Each dependency is looked up with find_package() first (system packages on Linux/macOS CI); if that fails it
# is downloaded at the pinned version below, verified by SHA-256 and built from source as part of the project.
# FetchContent then redirects the modules' own find_package() calls to the fetched targets, so module
# CMakeLists.txt files are the same for vcpkg and core builds. Versions match the vcpkg baseline in vcpkg.json.

include_guard(GLOBAL)
include(FetchContent)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    EXCLUDE_FROM_ALL SYSTEM
    FIND_PACKAGE_ARGS CONFIG)

FetchContent_Declare(pugixml
    URL https://github.com/zeux/pugixml/releases/download/v1.16/pugixml-1.16.tar.gz
    URL_HASH SHA256=4cee1ca4aad395170f4c7a07824f3bdd41f28316c6e1e1090a1425b278ec0b4b
    EXCLUDE_FROM_ALL SYSTEM
    FIND_PACKAGE_ARGS CONFIG)

FetchContent_Declare(ZLIB
    URL https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
    URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
    EXCLUDE_FROM_ALL SYSTEM
    FIND_PACKAGE_ARGS)

# Smart search (src/ai/search): hnswlib (header-only) and SQLite with FTS5. SQLite is always the pinned amalgamation
# here, compiled with FTS5, so every core build has the same full-text engine (a system SQLite may lack FTS5 or the
# trigram tokenizer, 3.34+); OVERRIDE_FIND_PACKAGE makes find_package(SQLite3) resolve to it.
FetchContent_Declare(hnswlib
    URL https://github.com/nmslib/hnswlib/archive/refs/tags/v0.9.0.tar.gz
    URL_HASH SHA256=65dfb6639cb7d1acbdaeec1429b978fb657a9bf368ebb8353109167394537823
    EXCLUDE_FROM_ALL SYSTEM
    FIND_PACKAGE_ARGS CONFIG)

FetchContent_Declare(SQLite3
    URL https://sqlite.org/2026/sqlite-amalgamation-3530400.zip
    URL_HASH SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
    EXCLUDE_FROM_ALL SYSTEM
    OVERRIDE_FIND_PACKAGE)

FetchContent_Declare(Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.0.tar.gz
    URL_HASH SHA256=0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34
    EXCLUDE_FROM_ALL SYSTEM
    FIND_PACKAGE_ARGS 3 CONFIG)

# Options of the fetched projects: static libraries, no tests, no install rules.
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
set(PUGIXML_INSTALL OFF CACHE INTERNAL "")
set(ZLIB_BUILD_SHARED OFF CACHE INTERNAL "")
set(ZLIB_BUILD_TESTING OFF CACHE INTERNAL "")
set(ZLIB_INSTALL OFF CACHE INTERNAL "")
set(CATCH_INSTALL_DOCS OFF CACHE INTERNAL "")
set(CATCH_INSTALL_EXTRAS OFF CACHE INTERNAL "")

set(HNSWLIB_EXAMPLES OFF CACHE INTERNAL "")

set(wabcut_fetched nlohmann_json pugixml ZLIB hnswlib SQLite3)
if(WABCUT_BUILD_TESTS)
    list(APPEND wabcut_fetched Catch2)
endif()
FetchContent_MakeAvailable(${wabcut_fetched})

# The amalgamation has no CMake build: SQLite3::SQLite3, like CMake's FindSQLite3 and vcpkg's sqlite3[fts5].
if(sqlite3_SOURCE_DIR AND NOT TARGET SQLite3::SQLite3)
    add_library(wabcut_sqlite3 STATIC "${sqlite3_SOURCE_DIR}/sqlite3.c")
    target_include_directories(wabcut_sqlite3 SYSTEM PUBLIC "${sqlite3_SOURCE_DIR}")
    target_compile_definitions(wabcut_sqlite3 PRIVATE SQLITE_ENABLE_FTS5 SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
    if(MSVC)
        target_compile_options(wabcut_sqlite3 PRIVATE /W0)
    else()
        target_compile_options(wabcut_sqlite3 PRIVATE -w)
        find_package(Threads REQUIRED)
        target_link_libraries(wabcut_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
    endif()
    add_library(SQLite3::SQLite3 ALIAS wabcut_sqlite3)
endif()

# zlib's own CMake names its static library ZLIB::ZLIBSTATIC; the modules link ZLIB::ZLIB.
if(TARGET zlibstatic AND NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()
# catch_discover_tests() lives in Catch2's extras directory.
if(catch2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
