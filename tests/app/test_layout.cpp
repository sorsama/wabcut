// Layout density (compact below 820 px) and the timeline ruler's adaptive label interval.

#include "ShellFixture.h"

#include <catch2/catch_test_macros.hpp>

#include <QQuickItem>

#include <algorithm>
#include <array>
#include <cmath>

using namespace wabcut::test;

TEST_CASE("Short windows switch to the compact density and keep the timeline tracks", "[app][layout]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    QQuickItem* timeline = shell.item("timelinePanel");
    REQUIRE(timeline != nullptr);
    const auto titleBar = [&] { return shell.theme->property("titleBarHeight").toInt(); };
    // The layout's glides are not what is tested here: without motion the sizes are final at once.
    shell.theme->setProperty("reduceMotion", true);

    // 1600×960: regular density, every track visible.
    REQUIRE(waitFor([&] { return timeline->property("visibleTrackCount").toInt() == 6; }));
    CHECK_FALSE(shell.theme->property("compact").toBool());
    const int regularTracks = timeline->property("visibleTrackCount").toInt();
    const int regularTitleBar = titleBar();
    CHECK(regularTracks == 6);
    CHECK(regularTitleBar == 52);

    // 1100×700: compact. The title bar (with the workspace tabs) gets lower; the timeline keeps at least four tracks.
    shell.resize(1100, 700);
    REQUIRE(waitFor([&] { return shell.theme->property("compact").toBool() && titleBar() == 44; }));
    const int compactTracks = timeline->property("visibleTrackCount").toInt();
    CHECK(compactTracks >= 4);

    // Just above the threshold the regular density returns.
    shell.resize(1100, 820);
    CHECK_FALSE(shell.theme->property("compact").toBool());

    writeOutputFile(QStringLiteral("layout.txt"),
                    QStringLiteral("Visible timeline tracks at 1600x960: %1 of 6 (title bar %2 px)\n"
                                   "Visible timeline tracks at 1100x700 (compact): %3 of 6 (title bar %4 px)\n")
                        .arg(regularTracks)
                        .arg(regularTitleBar)
                        .arg(compactTracks)
                        .arg(40));

    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Ruler labels use the smallest nice interval at least 110 px apart", "[app][layout]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    QQuickItem* view = shell.item("timelineView");
    REQUIRE(view != nullptr);

    // The demo sequence runs at 23.976 fps: labels count 24 frames per timecode second, and a timecode second
    // lasts 1.001 s of real time.
    constexpr std::array<int, 11> kIntervals{1, 2, 5, 10, 15, 30, 60, 300, 600, 1800, 3600}; // seconds
    constexpr int kBase = 24;
    constexpr double kRealSecondsPerLabelSecond = 1.001;
    for (int i = 0; i <= 20; ++i) {
        const double zoom = i / 20.0;
        view->setProperty("zoomLevel", zoom);
        const double pps = view->property("pixelsPerSecond").toDouble();
        const int stepFrames = view->property("rulerStepFrames").toInt();
        const double stepPx = view->property("rulerStepPx").toDouble();
        INFO("zoom " << zoom << ": " << pps << " px/s, step " << stepFrames << " frames = " << stepPx << " px");

        REQUIRE(stepFrames % kBase == 0);
        const int seconds = stepFrames / kBase;
        const auto it = std::find(kIntervals.begin(), kIntervals.end(), seconds);
        REQUIRE(it != kIntervals.end());
        CHECK(stepPx >= 110.0);
        CHECK(std::abs(stepPx - seconds * kRealSecondsPerLabelSecond * pps) < 0.01);
        // Minimal: the next smaller interval would crowd the labels.
        if (it != kIntervals.begin())
            CHECK(*(it - 1) * kRealSecondsPerLabelSecond * pps < 110.0);
    }

    const QStringList messages = takeMessages();
    INFO("Qt messages:\n" << join(messages));
    CHECK(messages.isEmpty());
}
