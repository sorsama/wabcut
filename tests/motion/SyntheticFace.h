#pragma once

// A synthetic talking head for the face-aware Morph Cut tests (0.7): an ellipse of nearly flat skin (a faint gradient - what
// makes real cheeks and foreheads hard for optical flow) with hair, brows, eyes, a nose and a mouth that opens, in front of a
// moving-texture background that does not move with the head. The pose (centre, size, turn in the picture's plane, mouth
// opening) moves the head and the landmarks together, so the tests have exact truth for the flow, the mid-point picture and the
// five landmarks YuNet would report.

#include "MovingTexture.h"

#include <wabcut/motion/FaceAlign.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace wabcut::motion::test {

struct HeadPose {
    double cx = 240.0;
    double cy = 135.0;
    double halfWidth = 48.0; ///< pixels: the head's ellipse is halfWidth wide and 1.25 halfWidth high (half sizes)
    double turnDegrees = 0.0;
    double mouthOpen = 0.0;  ///< 0 closed .. 1 wide open
};

inline HeadPose midPose(const HeadPose& a, const HeadPose& b, double t) {
    HeadPose p;
    p.cx = a.cx + (b.cx - a.cx) * t;
    p.cy = a.cy + (b.cy - a.cy) * t;
    p.halfWidth = a.halfWidth + (b.halfWidth - a.halfWidth) * t;
    p.turnDegrees = a.turnDegrees + (b.turnDegrees - a.turnDegrees) * t;
    p.mouthOpen = a.mouthOpen + (b.mouthOpen - a.mouthOpen) * t;
    return p;
}

class SyntheticFace {
public:
    explicit SyntheticFace(std::uint32_t seed = 21) : m_background(seed) {}

    /// The picture of the head at `pose` over the background (0..1 grey).
    Plane picture(int w, int h, const HeadPose& pose) const {
        Plane p(w, h);
        const double turn = pose.turnDegrees * std::numbers::pi / 180.0;
        const double c = std::cos(turn);
        const double s = std::sin(turn);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                // Into the head's own frame: unit = the head's half width, x right, y down, no turn.
                const double dx = (x - pose.cx) / pose.halfWidth;
                const double dy = (y - pose.cy) / pose.halfWidth;
                const double lx = c * dx + s * dy;
                const double ly = -s * dx + c * dy;
                p(x, y) = static_cast<float>(shade(x, y, lx, ly, pose.mouthOpen));
            }
        }
        return p;
    }

    /// YuNet's five landmarks and the box of the head at `pose`.
    static FaceFeatures features(const HeadPose& pose) {
        const double turn = pose.turnDegrees * std::numbers::pi / 180.0;
        const double c = std::cos(turn);
        const double s = std::sin(turn);
        const auto at = [&](double lx, double ly) {
            return FacePoint{static_cast<float>(pose.cx + pose.halfWidth * (c * lx - s * ly)),
                             static_cast<float>(pose.cy + pose.halfWidth * (s * lx + c * ly))};
        };
        FaceFeatures f;
        f.points = {at(-0.42, -0.25), at(0.42, -0.25), at(0.0, 0.15), at(-0.35, 0.62), at(0.35, 0.62)};
        // The box of the ellipse (half sizes 1 and 1.25) turned by `turn`.
        const double hw = std::sqrt(c * c + 1.5625 * s * s) * pose.halfWidth;
        const double hh = std::sqrt(s * s + 1.5625 * c * c) * pose.halfWidth;
        f.x = static_cast<float>(pose.cx - hw);
        f.y = static_cast<float>(pose.cy - hh);
        f.w = static_cast<float>(2.0 * hw);
        f.h = static_cast<float>(2.0 * hh);
        f.score = 0.95f;
        return f;
    }

private:
    static double soft(double edge, double value, double width) { // 1 where value < edge, 0 beyond edge + width
        return 1.0 - std::clamp((value - edge) / width, 0.0, 1.0);
    }

    double shade(int x, int y, double lx, double ly, double mouthOpen) const {
        double v = m_background.at(x, y) * 0.8 + 0.1;
        const double head = std::sqrt(lx * lx + (ly / 1.25) * (ly / 1.25));
        const double inHead = soft(0.97, head, 0.06);
        if (inHead <= 0.0) {
            return v;
        }
        // Skin: nearly flat, a faint gradient.
        double skin = 0.62 + 0.04 * lx - 0.03 * ly;
        // Hair: the cap above the forehead.
        skin = skin * (1.0 - 0.8 * soft(-0.62, ly, 0.05)) + 0.18 * soft(-0.62, ly, 0.05) * 0.8;
        // Eyes: a pale ellipse with a dark iris; brows above.
        for (const double ex : {-0.42, 0.42}) {
            const double ed = std::sqrt(((lx - ex) / 0.22) * ((lx - ex) / 0.22) + ((ly + 0.25) / 0.12) * ((ly + 0.25) / 0.12));
            skin = skin * (1.0 - soft(1.0, ed, 0.25)) + 0.93 * soft(1.0, ed, 0.25);
            const double id = std::hypot(lx - ex, ly + 0.25);
            skin = skin * (1.0 - soft(0.09, id, 0.03)) + 0.08 * soft(0.09, id, 0.03);
            const double brow = std::abs(ly + 0.52) < 0.045 && std::abs(lx - ex) < 0.26 ? 1.0 : 0.0;
            skin = skin * (1.0 - 0.7 * brow) + 0.2 * 0.7 * brow;
        }
        // Nose: a soft vertical shade and a dark tip.
        skin -= 0.06 * soft(0.0, std::abs(lx), 0.07) * soft(0.2, ly, 0.0001) * (ly > -0.2 ? 1.0 : 0.0);
        const double tip = std::hypot(lx, ly - 0.15);
        skin = skin * (1.0 - soft(0.07, tip, 0.03) * 0.8) + 0.25 * soft(0.07, tip, 0.03) * 0.8;
        // Mouth: a dark ellipse whose height follows the opening; teeth above when open.
        const double mh = 0.035 + 0.22 * mouthOpen;
        const double md = std::sqrt((lx / 0.36) * (lx / 0.36) + ((ly - 0.62) / mh) * ((ly - 0.62) / mh));
        skin = skin * (1.0 - soft(1.0, md, 0.18)) + 0.12 * soft(1.0, md, 0.18);
        return v * (1.0 - inHead) + skin * inHead;
    }

    MovingTexture m_background;
};

} // namespace wabcut::motion::test
