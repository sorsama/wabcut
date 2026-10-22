# Motion estimation (P.2, wabcut_motion): dense optical flow and flow interpolation on synthetic moving textures with
# exact sub-pixel truth (accuracy, both directions, determinism, PSNR of Optical Flow against Frame Blending and Frame
# Sampling). Pure C++: runs in the core preset too. The GPU flow mix is tested against the CPU reference in tests/render
# (test_flowmix.cpp), evaluation and export in tests/playback and tests/export.
wabcut_add_test_suite(motion
    SOURCES
        MovingTexture.h
        test_flow.cpp
    LIBS wabcut_motion
    PROPERTIES LABELS motion)
