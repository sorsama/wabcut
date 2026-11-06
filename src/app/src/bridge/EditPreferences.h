#pragma once

// Editing preferences the C++ bridges read straight from the settings store (Preferences.qml keeps them in the same
// keys, so the Settings window and these stay in step). Header-only.
//
//   * 0.7 (card 182) Settings > Timeline & editing > "Add tracks when editing clips in" (editing/addTracks, default on,
//     Premiere 26.3's Timeline preference): while on, an edit that needs a track the sequence does not have makes it (a
//     recording's second audio stream gets a track of its own, a paste that spans more tracks than exist, an Overlay with
//     no free video track above). Off, such an edit uses the tracks that are there and says so when nothing fits; the
//     explicit ways to add tracks (Add Track, Add Tracks..., a drag held below the last track) are unaffected.

#include <QLatin1String>
#include <QSettings>

namespace wabcut::app::editprefs {

inline constexpr const char* kAddTracksKey = "editing/addTracks";

/// Whether an edit may add tracks the sequence lacks (Settings > Timeline & editing).
inline bool addTracksOnEdit() {
    return QSettings().value(QLatin1String(kAddTracksKey), true).toBool();
}

} // namespace wabcut::app::editprefs
