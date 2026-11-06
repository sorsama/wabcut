#pragma once

// Video output to another display (0.7, card 110; Premiere's Mercury Transmit to a display): the decisions behind the
// output window, as plain functions over plain data so they are tested without a screen. MonitorBridge feeds them the
// displays Qt reports and the saved settings, and ProgramOutputWindow.qml shows what they decide.
//
//   * Which display: the choice is saved as a stable key (manufacturer, model, serial or the system name, never the
//     display's number in the list: plugging another display in does not move the output to it), found again when the
//     display comes back, and the output is off while it is gone.
//   * Where the picture goes in the display: Fit (letterbox / pillarbox on black), Fill (cropped to cover it) or 1:1
//     (the frame's own pixels, centred), for the sequence's display shape (frame size and pixel aspect).
//   * Frame rate: a display runs at its own refresh rate. How the sequence's rate sits on it (the same, a whole multiple,
//     the 3:2 pulldown of 24 on 60, or an uneven one), and which mode the display could be switched to so it sits well
//     (Windows: Match Refresh Rate; the other platforms only report the cadence).
//   * Sync offset: how late this display shows a picture, in ms, so the program is played that much ahead (the mirror's
//     display latency) and the picture meets the sound.
//
// Hardware video cards (Blackmagic DeckLink, AJA, NDI) need their vendor's SDK: not here (docs/monitors.md).

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wabcut::app::videoout {

/// A display as the system reports it.
struct DisplayInfo {
    std::string key;     ///< stable across plugging and unplugging: "manufacturer|model|serial", else the system name
    std::string name;    ///< the system's name for it ("\\.\DISPLAY2"): what display modes are asked of
    std::string label;   ///< what the user sees ("DELL U2720Q"; the system name when it has no model)
    int width = 0;       ///< pixels
    int height = 0;
    double refreshHz = 0; ///< 0: unknown
    bool primary = false;
};

/// A mode a display can run at (Windows lists them; elsewhere the list is empty and nothing is switched).
struct DisplayMode {
    int width = 0;
    int height = 0;
    double refreshHz = 0;
    bool operator==(const DisplayMode&) const = default;
};

enum class Scaling { Fit, Fill, Native };

/// "fit", "fill", "native" (the saved and QML form); unknown text is Fit.
std::string toString(Scaling scaling);
Scaling scalingFromString(const std::string& text);

/// The output's settings (MonitorBridge saves them).
struct Settings {
    bool enabled = false;               ///< Enable Transmit
    std::string displayKey;             ///< the chosen display; "" none
    Scaling scaling = Scaling::Fit;
    bool safeMargins = false;           ///< title and action safe lines over the output
    int syncOffsetMs = 0;               ///< -kMaxSyncOffsetMs..kMaxSyncOffsetMs; positive: the display shows pictures late
    bool matchRefreshRate = false;      ///< switch the display to a mode that suits the sequence's rate (Windows)
};

inline constexpr int kMaxSyncOffsetMs = 500;

/// The offset clamped to its range.
int clampSyncOffset(int ms);

/// The sequence the output plays.
struct Timing {
    std::int32_t rateNum = 30;          ///< frames per second as a fraction (30000 / 1001 for 29.97)
    std::int32_t rateDen = 1;
    std::int32_t width = 1920;          ///< the frame in pixels
    std::int32_t height = 1080;
    double pixelAspect = 1.0;           ///< the shape of a pixel (the sequence's pixel aspect ratio)

    double fps() const { return rateDen > 0 ? static_cast<double>(rateNum) / rateDen : 0.0; }
    /// Width over height as the picture is shown (frame size and pixel aspect).
    double displayAspect() const { return height > 0 ? width * pixelAspect / height : 0.0; }
};

struct Rect {
    double x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Rect&) const = default;
};

/// Where the sequence's picture goes in a display of `displayWidth` x `displayHeight` pixels: Fit is the biggest rect of the
/// picture's shape inside it, Fill the smallest that covers it (it reaches past the display, cropped there), Native the
/// frame's own pixels (width x pixel aspect by height) centred, cut off where the display is smaller.
Rect placePicture(const Timing& timing, int displayWidth, int displayHeight, Scaling scaling);

/// How a sequence's frame rate sits on a display's refresh rate.
enum class Cadence {
    Unknown,   ///< the display's rate (or the sequence's) is not known
    Even,      ///< a whole number of refreshes per frame (24 on 48 / 72 / 120, 25 on 50, 30 on 60): every frame lasts the same
    NearEven,  ///< a whole number within a tenth of a percent (23.976 on 24 Hz, 59.94 on 60): an occasional repeated frame
    Pulldown,  ///< two and a half refreshes per frame (24 on 60, 23.976 on 59.94): frames alternate 3 and 2 refreshes
    Uneven,    ///< any other ratio (25 on 60): frames last unequal numbers of refreshes, judder
};

struct CadenceInfo {
    Cadence kind = Cadence::Unknown;
    double ratio = 0;   ///< refreshes per frame
    int multiple = 0;   ///< Even / NearEven: the whole number of refreshes per frame
};

CadenceInfo analyzeCadence(double fps, double refreshHz);

/// The mode that suits `fps` best among `modes` of the display's current size: the lowest refresh rate that is a whole
/// multiple of the sequence's rate within 0.1 % (24 -> 48, 72, 120; 29.97 -> 59.94). nullopt when the display already runs
/// at one, or none of its modes does.
std::optional<DisplayMode> bestModeFor(double fps, const DisplayMode& current, std::span<const DisplayMode> modes);

/// What the output does now.
enum class State {
    Off,             ///< Enable Transmit is off
    NoDisplay,       ///< on, but no display is chosen
    DisplayMissing,  ///< on, but the chosen display is not connected (it comes back when the display does)
    Active,
};

struct Plan {
    State state = State::Off;
    int displayIndex = -1;                 ///< into the displays the plan was made from (Active)
    DisplayInfo display;                   ///< the chosen display (Active), else its key and what is known of it
    Rect picture;                          ///< placePicture for the display (Active)
    CadenceInfo cadence;                   ///< the sequence's rate on the display's refresh rate
    std::optional<DisplayMode> switchTo;   ///< the mode Match Refresh Rate would set (empty when none or not asked)
    int syncOffsetMs = 0;                  ///< clamped
};

/// The index of the display with this key, or -1.
int findDisplay(std::span<const DisplayInfo> displays, const std::string& key);

/// Decides the output: which display, where the picture goes, the cadence and the mode to switch to. `modes` are the chosen
/// display's modes (only looked at when `settings.matchRefreshRate`).
Plan planOutput(std::span<const DisplayInfo> displays, const Settings& settings, const Timing& timing,
                std::span<const DisplayMode> modes = {});

} // namespace wabcut::app::videoout
