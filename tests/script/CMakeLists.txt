# Extensions (C.12). Manifests, permissions, storage, events, the protocol, the runsModel classification, the editor's
# side against a misbehaving helper (wabcut_fake_scripthost) and the manager are pure C++ (core preset too); the
# sandbox and the samples need the real wabcut-scripthost (Qt Qml), `ext run` needs wabcut-cli (full builds).
find_package(nlohmann_json CONFIG REQUIRED)
add_executable(wabcut_fake_scripthost fake_scripthost.cpp)
target_link_libraries(wabcut_fake_scripthost PRIVATE nlohmann_json::nlohmann_json)
wabcut_target_defaults(wabcut_fake_scripthost)

set(wabcut_script_test_sources
    ../model/TimelineFixture.cpp
    ../model/TimelineFixture.h
    ScriptTestUtil.h
    test_docs.cpp
    test_host_robustness.cpp
    test_manager.cpp
    test_manifest.cpp
    test_permissions.cpp
    test_protocol.cpp
    test_runs_model.cpp
    test_storage_events.cpp
    test_wave5_extensions.cpp)
if(TARGET wabcut-scripthost)
    list(APPEND wabcut_script_test_sources ../ai/EditTestUtil.h test_samples.cpp test_sandbox.cpp)
endif()
if(TARGET wabcut-cli)
    list(APPEND wabcut_script_test_sources test_cli_ext.cpp)
endif()

wabcut_add_test_suite(script
    SOURCES ${wabcut_script_test_sources}
    LIBS wabcut_script wabcut_ai_edit wabcut_ai)
target_include_directories(wabcut_script_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/ai)
target_compile_definitions(wabcut_script_tests PRIVATE WABCUT_SCRIPT_SAMPLES_DIR="${CMAKE_SOURCE_DIR}/src/script/samples"
    WABCUT_DOCS_DIR="${CMAKE_SOURCE_DIR}/docs")
# The host tests start the helpers from next to the test executable.
add_dependencies(wabcut_script_tests wabcut_fake_scripthost)
if(TARGET wabcut-scripthost)
    add_dependencies(wabcut_script_tests wabcut-scripthost)
endif()
# No build dependency on wabcut-cli (it links most of the editor): the full build makes it, and the test skips when it
# is missing.
if(TARGET wabcut-cli)
    target_compile_definitions(wabcut_script_tests PRIVATE WABCUT_CLI_PATH="$<TARGET_FILE:wabcut-cli>")
endif()
