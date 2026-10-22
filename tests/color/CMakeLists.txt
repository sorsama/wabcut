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
    LIBS wabcut_color wabcut_ai_edit
    PROPERTIES LABELS color)
target_include_directories(wabcut_color_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/command)

# Colour management (I.9, wabcut_colorspaces, full builds): OpenColorIO's transforms against published reference values
# (ACEScct, SMPTE ST 2084 PQ, BT.2100 HLG, camera log curves), the baked LUTs against OCIO's own processor, the input
# and output plans, the tone-mapped SDR preview.
if(TARGET wabcut_colorspaces)
    wabcut_add_test_suite(colorspaces
        SOURCES test_color_spaces.cpp
        LIBS wabcut_colorspaces
        PROPERTIES LABELS color)
endif()
