find_package(nlohmann_json CONFIG REQUIRED)

wabcut_add_test_suite(keymap
    SOURCES
        test_default_tables.cpp
        test_key_chord.cpp
        test_keymap_manager.cpp
        test_kys_export.cpp
        test_kys_import.cpp
        test_registry.cpp
        test_transport_gesture.cpp
    LIBS wabcut_keymap nlohmann_json::nlohmann_json)
target_compile_definitions(wabcut_keymap_tests PRIVATE
    WABCUT_KEYMAP_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    WABCUT_KEYMAP_TABLE_DIR="${CMAKE_SOURCE_DIR}/src/keymap/keymaps")

# The keymap module is Qt-free; this test checks its key constants against Qt's own headers.
if(TARGET Qt6::Core)
    target_sources(wabcut_keymap_tests PRIVATE test_qt_compat.cpp)
    target_link_libraries(wabcut_keymap_tests PRIVATE Qt6::Core)
endif()
