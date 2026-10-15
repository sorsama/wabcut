import QtQuick

// What a DockSlot moves into a window of its own and back (0.1.3 panel docking): it holds the slot's panels, so they
// keep their state wherever it goes, and names its slot for the panels inside (Panel.dockHost).
Item {
    /// The slot this holder belongs to.
    property Item dockSlot: null
}
