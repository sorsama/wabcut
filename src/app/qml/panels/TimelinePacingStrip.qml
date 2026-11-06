pragma ComponentBehavior: Bound
import QtQuick

// 0.7 (card 180): the pacing heat strip, a thin band between the ruler and the tracks that colours the stretches of the edit
// where a viewer may get bored (Pacing: PacingBridge, graded from the cuts, how much the picture moves, the silences and the
// speed of the speech, all measured on this machine). Amber = easing off, strong amber = slow, red = likely to drag; the
// calm stretches draw nothing. Hover a stretch for why, in numbers, and what to try; click it to put the In and Out marks
// around it (and the playhead on its start); Shift-click selects the clips that play in it; the right-click menu adds a
// marker that remembers the reasons. The band is a view: it changes nothing in the project but what those actions do.
Item {
    id: root
    objectName: "timelinePacingStrip"

    required property TimelineView view

    /// The stretch under the pointer (-1: none); it follows the strip when the analysis changes under a resting pointer.
    readonly property int hovered: Pacing.stretches.length >= 0 ? Pacing.stretchAt(hoverArea.hoverSeconds) : -1

    Accessible.role: Accessible.Indicator
    Accessible.name: qsTr("Pacing strip")
    Accessible.description: Pacing.summary
    clip: true

    /// The band itself: a quiet track so the strip reads as a strip even where nothing drags.
    Rectangle {
        anchors.fill: parent
        radius: Theme.space(0.5)
        color: Theme.sunken
    }

    Repeater {
        model: Pacing.stretches

        delegate: Rectangle {
            id: block

            required property var modelData

            readonly property real x0: modelData.startSec * root.view.pixelsPerSecond - root.view.scrollX
            readonly property real x1: modelData.endSec * root.view.pixelsPerSecond - root.view.scrollX

            objectName: "pacingStretch_" + modelData.index
            x: Math.round(Math.max(0, x0))
            width: Math.max(Theme.space(0.5), Math.round(Math.min(root.width, x1)) - x)
            height: root.height
            visible: x1 > 0 && x0 < root.width
            radius: Theme.space(0.5)
            color: modelData.level >= 3 ? Theme.danger : Qt.alpha(Theme.warning, modelData.level === 2 ? 0.95 : 0.5)
            border.width: root.hovered === modelData.index ? Theme.selectionWidth : 0
            border.color: Theme.textPrimary
        }
    }

    // While files are measured, a thin line crawls along the foot of the band: more is still to come.
    Rectangle {
        objectName: "pacingProgress"
        visible: Pacing.busy
        anchors.bottom: parent.bottom
        height: Theme.hairline * 2
        width: root.width * Pacing.progress
        color: Theme.accentStroke
    }

    MouseArea {
        id: hoverArea
        objectName: "pacingStripArea"
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: root.hovered >= 0 ? Qt.PointingHandCursor : Qt.ArrowCursor

        /// The time under the pointer, in seconds (the strip is aligned with the lanes).
        property real hoverSeconds: -1
        onPositionChanged: mouse => hoverSeconds = (mouse.x + root.view.scrollX) / root.view.pixelsPerSecond
        onExited: hoverSeconds = -1
        onClicked: mouse => {
            const at = (mouse.x + root.view.scrollX) / root.view.pixelsPerSecond
            const index = Pacing.stretchAt(at)
            if (index < 0)
                return
            if (mouse.button === Qt.RightButton) {
                stripMenu.index = index
                stripMenu.popup(hoverArea, mouse.x, mouse.y)
            } else if (mouse.modifiers & Qt.ShiftModifier) {
                Pacing.selectStretch(index)
            } else {
                Pacing.markStretch(index)
            }
        }

        Tooltip {
            objectName: "pacingTooltip"
            parent: hoverArea
            visible: hoverArea.containsMouse && !stripMenu.opened && (root.hovered >= 0 || Pacing.status !== "")
            x: Math.max(0, Math.min(hoverArea.width - implicitWidth, hoverArea.mouseX + Theme.space(2)))
            y: hoverArea.height + Theme.space(1)
            text: root.hovered >= 0 ? Pacing.stretch(root.hovered).text ?? "" : Pacing.status
        }
    }

    ContextMenu {
        id: stripMenu
        objectName: "pacingStripMenu"

        property int index: -1

        ContextMenuItem {
            objectName: "pacingMenuMark"
            text: qsTr("Mark In and Out Around This Stretch")
            iconName: "mark-in"
            onTriggered: Pacing.markStretch(stripMenu.index)
        }
        ContextMenuItem {
            objectName: "pacingMenuSelect"
            text: qsTr("Select the Clips in This Stretch")
            iconName: "crosshair"
            onTriggered: Pacing.selectStretch(stripMenu.index)
        }
        ContextMenuItem {
            objectName: "pacingMenuMarker"
            text: qsTr("Add a Marker for This Stretch")
            iconName: "bookmark"
            onTriggered: Pacing.addMarker(stripMenu.index)
        }
    }
}
