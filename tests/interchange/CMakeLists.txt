# Interchange tests: format fixtures (tests/interchange/fixtures), robustness (truncated/mutated/garbage
# input), relinking and the import report. The fixtures folder is passed as WABCUT_INTERCHANGE_FIXTURES.
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED) # test_mogrt deflates its own .mogrt archives

wabcut_add_test_suite(interchange
    SOURCES
        ../model/ScaleProject.cpp
        ../model/ScaleProject.h
        ../model/TimelineFixture.cpp
        ../model/TimelineFixture.h
        AudioPostTestSupport.h
        InterchangeTestSupport.cpp
        InterchangeTestSupport.h
        test_aaf.cpp
        test_audiopost_import.cpp # 0.6: AAF / OMF import
        test_captions.cpp
        test_detect.cpp
        test_docx.cpp
        test_edl.cpp
        test_export.cpp
        test_fcp7.cpp
        test_fcp7_filters.cpp # 0.6: colour grades and audio EQ / dynamics (card #66)
        test_fcpxml.cpp
        test_interchange_effects.cpp # 0.6: generators and FCPXML / OTIO effects
        test_auditions_interchange.cpp # 0.6 (card #74): auditions
        test_file_url.cpp
        test_lottie.cpp
        test_omf.cpp
        test_mogrt.cpp
        test_otio.cpp
        test_prproj.cpp
        test_relinker.cpp
        test_robustness.cpp
        test_summary.cpp
        test_time_math.cpp
        test_xmp.cpp
    LIBS wabcut_interchange nlohmann_json::nlohmann_json ZLIB::ZLIB)
# White-box tests of the private time math and parsers' helpers.
target_include_directories(wabcut_interchange_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/interchange/src
    ${CMAKE_SOURCE_DIR}/tests/model)
target_compile_definitions(wabcut_interchange_tests PRIVATE
    WABCUT_INTERCHANGE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
