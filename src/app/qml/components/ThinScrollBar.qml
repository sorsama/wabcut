import QtQuick
import QtQuick.Controls.Basic

// Minimal vertical scroll bar for panel content (design language v3): a thin pill in `well` that only shows while it
// is needed.
ScrollBar {
    id: control

    policy: ScrollBar.AsNeeded
    padding: Theme.hairline
    minimumSize: 0.1

    contentItem: Rectangle {
        implicitWidth: Theme.space(1)
        implicitHeight: Theme.space(1)
        radius: width / 2
        color: control.pressed ? Theme.textSecondary : control.hovered ? Theme.textTertiary : Theme.well
        opacity: control.size < 1 ? 1 : 0

        Behavior on color { ColorAnimation { duration: Theme.durFast } }
    }

    background: null
}
