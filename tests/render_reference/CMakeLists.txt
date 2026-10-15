# CPU reference renders (C.15, wabcut_render_reference): the blend modes against hand-computed values and goldens, Dissolve,
# the Ultra Key on a synthetic green-screen card, masks, Border, Drop Shadow, Gaussian Blur and the vertical blur fill,
# the synthetic media and the golden-file format. Pure C++: runs in the core preset too. Goldens are in goldens/ (WRI1
# text); a mismatch writes the new picture to <build>/test-output/render_reference/ and fails, and WABCUT_UPDATE_GOLDEN=1
# rewrites the goldens.
wabcut_add_test_suite(render_reference
    SOURCES
        ReferenceTestSupport.h
        ReferenceTestSupport.cpp
        test_blend.cpp
        test_golden.cpp
        test_layout.cpp
        test_synthetic.cpp
        test_ultra_key.cpp
    LIBS wabcut_render_reference
    PROPERTIES LABELS render_reference)
target_compile_definitions(wabcut_render_reference_tests PRIVATE
    WABCUT_RENDER_REFERENCE_GOLDENS="${CMAKE_CURRENT_SOURCE_DIR}/goldens")
