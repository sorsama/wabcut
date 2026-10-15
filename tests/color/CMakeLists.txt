# Colour grade core (I.5, wabcut_color): .cube parsing (with malformed and fuzzed corpora), tetrahedral and trilinear
# interpolation, curve baking, the grade's CPU reference stage by stage, ASC CDL, scope binning and statistics, Auto and
# Color Match on synthetic pictures, and the colour commands through the CommandEngine with a fake renderer. Pure C++:
# runs in the core preset too. GPU vs CPU comparisons live in tests/render (test_grade.cpp).
wabcut_add_test_suite(color
    SOURCES
        ../model/TimelineFixture.cpp
        ../model/TimelineFixture.h
        test_lut.cpp
        test_grade_math.cpp
        test_cdl.cpp
        test_scopes.cpp
        test_color_tools.cpp
        test_color_commands.cpp
        test_hdr.cpp # I.9 (gaming): PQ / HLG, the BT.2390 tone map of HDR captures
    LIBS wabcut_color wabcut_ai_edit
    PROPERTIES LABELS color)
target_include_directories(wabcut_color_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/command)
