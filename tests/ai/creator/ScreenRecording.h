#pragma once

// A synthetic screen recording for the AI-11 auto zoom tests: 20 s of a 1920x1080 desktop with an app window, a cursor
// that moves to a button and clicks it (the button lights up, a menu opens), then moves to a text field, clicks it and
// types; then nothing happens. Frames are drawn on demand (RGB, 3 bytes a pixel).

#include <wabcut/ai/autozoom/Activity.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace wabcut::ai::autozoom::test {

/// A 20 s 1920x1080 screen recording: an app window, a cursor (drawn at 200 %) that moves to a button at 2-3 s, clicks
/// it at 3.2 s (the button lights up, a menu opens), moves to a text field at 8-9 s, clicks it at 9.2 s and types from
/// 9.5 s to 12 s; then nothing happens.
struct ScreenRecording {
    static constexpr int kWidth = 1920;
    static constexpr int kHeight = 1080;
    static constexpr double kButtonX = 1500, kButtonY = 200;
    static constexpr double kFieldX = 420, kFieldY = 800;

    static std::pair<double, double> cursor(double t) {
        const auto lerp = [](double a, double b, double f) { return a + (b - a) * std::clamp(f, 0.0, 1.0); };
        if (t < 2.0) return {300, 300};
        if (t < 3.0) return {lerp(300, kButtonX, t - 2.0), lerp(300, kButtonY, t - 2.0)};
        if (t < 8.0) return {kButtonX, kButtonY};
        if (t < 9.0) return {lerp(kButtonX, kFieldX, t - 8.0), lerp(kButtonY, kFieldY, t - 8.0)};
        return {kFieldX, kFieldY};
    }

    std::vector<std::uint8_t> frame(double t) const {
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(kWidth) * kHeight * 3);
        const auto fill = [&](int x0, int y0, int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            for (int y = std::max(0, y0); y < std::min(kHeight, y1); ++y) {
                std::uint8_t* p = rgb.data() + (static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(std::max(0, x0))) * 3;
                for (int x = std::max(0, x0); x < std::min(kWidth, x1); ++x, p += 3) {
                    p[0] = r; p[1] = g; p[2] = b;
                }
            }
        };
        fill(0, 0, kWidth, kHeight, 236, 236, 240);       // desktop
        fill(100, 60, 1820, 1020, 250, 250, 252);         // the app window
        fill(100, 60, 300, 1020, 226, 226, 232);          // its sidebar
        for (int line = 0; line < 12; ++line) {           // static text
            fill(340, 120 + line * 40, 340 + 400 + (line * 37) % 300, 132 + line * 40, 90, 90, 100);
        }
        fill(1440, 180, 1560, 220, t >= 3.2 ? 56 : 200, t >= 3.2 ? 0 : 200, t >= 3.2 ? 186 : 210); // the button
        if (t >= 3.3) {
            fill(1440, 222, 1700, 500, 255, 255, 255);    // its menu
            for (int item = 0; item < 6; ++item) {
                fill(1460, 240 + item * 42, 1640, 252 + item * 42, 70, 70, 80);
            }
        }
        const std::uint8_t field = t >= 9.2 ? 30 : 180;   // the text field's border lights up when clicked
        fill(300, 780, 800, 782, field, field, field + 40);
        fill(300, 818, 800, 820, field, field, field + 40);
        fill(300, 780, 302, 820, field, field, field + 40);
        fill(798, 780, 800, 820, field, field, field + 40);
        if (t >= 9.5) {                                   // typed characters, one every 0.2 s
            const int typed = std::min(12, static_cast<int>((t - 9.5) / 0.2) + 1);
            for (int k = 0; k < typed; ++k) {
                fill(312 + k * 18, 790, 326 + k * 18, 810, 20, 20, 30);
            }
        }
        // The arrow, tip at the cursor position, white with a black outline.
        const auto [cx, cy] = cursor(t);
        for (int r = 0; r < 36; ++r) {
            const int width = r * 2 / 3;
            for (int c = 0; c <= width; ++c) {
                const bool edge = c <= 1 || c >= width - 1 || r >= 34;
                fill(static_cast<int>(cx) + c, static_cast<int>(cy) + r, static_cast<int>(cx) + c + 1, static_cast<int>(cy) + r + 1,
                     edge ? 0 : 255, edge ? 0 : 255, edge ? 0 : 255);
            }
        }
        return rgb;
    }

    FrameReader reader() const {
        return [this](Ticks time, int width) -> std::expected<GreyFrame, Error> {
            const auto rgb = frame(time.toSeconds());
            return toAnalysisFrame(rgb.data(), kWidth, kHeight, width, time);
        };
    }
};

} // namespace wabcut::ai::autozoom::test
