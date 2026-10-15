pragma ComponentBehavior: Bound
import QtQuick

// One built-in export preset ("Match Source", "ProRes 422 HQ", …) as a tile of the Export panel's grid: the same tile
// as the gaming presets (GamingPresetCard: hue glyph circle, name, what it makes, the estimate pill), always available.
// One Tab stop; Space/Enter select (radio semantics for accessibility).
GamingPresetCard {
    available: true
}
