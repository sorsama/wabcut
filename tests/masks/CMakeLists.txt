# Masks without pixels (I.6, wabcut_masks): the region tracker on synthetic moving textures with exact sub-pixel truth
# (accuracy, loss, backward = inverse of forward, RANSAC, speed), and the mask / track / redaction commands through the
# CommandEngine with fake pictures. Pure C++: runs in the core preset too. GPU masks are tested in tests/render
# (test_masks.cpp), redactions in evaluation in tests/playback/video (test_redaction.cpp), their sound in
# tests/playback (test_redaction_audio.cpp) and in export in tests/export (test_rendered.cpp).
wabcut_add_test_suite(masks
    SOURCES
        ../model/TimelineFixture.cpp
        ../model/TimelineFixture.h
        SyntheticMotion.h
        test_tracker.cpp
        test_mask_commands.cpp
    LIBS wabcut_masks wabcut_ai_edit
    PROPERTIES LABELS masks)
target_include_directories(wabcut_masks_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model ${CMAKE_SOURCE_DIR}/tests/command)
target_compile_definitions(wabcut_masks_tests PRIVATE WABCUT_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test-output")
