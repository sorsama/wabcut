import QtQuick

// Keyboard focus indicator (design language v3): a 2 px ring in Theme.focus (white on dark, black on light) drawn just
// outside its parent. Show it for `visualFocus` (focus that came from the keyboard), not for mouse focus.
// `targetRadius` is the parent's corner radius (height / 2 for a pill).
Rectangle {
    id: ring

    property real targetRadius: 0

    anchors.fill: parent
    anchors.margins: -(Theme.focusRingGap + Theme.focusRingWidth)
    radius: targetRadius > 0 ? targetRadius + Theme.focusRingGap + Theme.focusRingWidth : 0
    color: "transparent"
    border.width: Theme.focusRingWidth
    border.color: Theme.focus
    visible: false
}
