#pragma once

// The display modes of a screen and switching one for the life of the process (0.7, card 110: Match Refresh Rate for the video
// output). Windows only (EnumDisplaySettingsEx / ChangeDisplaySettingsEx); elsewhere a screen lists no modes and nothing is
// switched, so the output only reports how the sequence's rate sits on the display (VideoOutput.h analyzeCadence).

#include "VideoOutput.h"

#include <string>
#include <vector>

namespace wabcut::app::platform {

/// The modes (size and refresh rate) the display named by its system device name (QScreen::name(), "\\.\DISPLAY2") can run
/// at, 32-bit colour, each once. Windows lists 59 Hz for 59.94, 23 for 23.976 and so on: those come back as the broadcast
/// rate. Empty when the platform cannot say.
std::vector<videoout::DisplayMode> displayModes(const std::string& deviceName);

/// Switches the display to `mode` (CDS_FULLSCREEN: the system takes the mode back when the process ends, and
/// restoreDisplayMode() does it at once). False when the platform cannot or the system refuses.
bool setDisplayMode(const std::string& deviceName, const videoout::DisplayMode& mode);

/// Puts the display back to the mode the user saved for it.
void restoreDisplayMode(const std::string& deviceName);

} // namespace wabcut::app::platform
