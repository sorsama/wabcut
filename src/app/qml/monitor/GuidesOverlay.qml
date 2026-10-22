pragma ComponentBehavior: Bound
import QtQuick

// P.2 (E6): the Program Monitor's guides (Premiere's View > Show Guides; bridge Monitors; docs/monitors.md), over the
// frame (fractions of the frame are fractions of this item). Unless the guides are locked, drag a guide to move it, drag
// it out of the frame to remove it, double-click it to edit it.
Item {
    id: root
    objectName: "programGuidesOverlay"

    visible: Monitors.showGuides && Monitors.guides.length > 0

    // By count, so a drag's own updates keep the pressed guide alive.
    Repeater {
        model: Monitors.guides.length
        delegate: Item {
            id: guide
            required property int index
            readonly property var modelData: index < Monitors.guides.length ? Monitors.guides[index]
                                                                            : ({ index: -1, orientation: "", position: -1, color: "transparent" })
            readonly property bool vertical: modelData.orientation === "vertical"
            objectName: "programGuide_" + modelData.index
            x: vertical ? modelData.position * root.width - width / 2 : 0
            y: vertical ? 0 : modelData.position * root.height - height / 2
            width: vertical ? 7 : root.width
            height: vertical ? root.height : 7

            Rectangle {
                anchors.centerIn: parent
                // Dragged past the frame's edges: about to go (Premiere: drag it out of the Program Monitor).
                visible: guide.modelData.position >= 0 && guide.modelData.position <= 1
                width: guide.vertical ? 1 : parent.width
                height: guide.vertical ? parent.height : 1
                color: guide.modelData.color
            }
            MouseArea {
                anchors.fill: parent
                // Locked guides take no pointer (hidden: a disabled item would still own the cursor).
                visible: !Monitors.lockGuides
                cursorShape: guide.vertical ? Qt.SplitHCursor : Qt.SplitVCursor
                preventStealing: true
                onPositionChanged: mouse => {
                    if (!pressed)
                        return
                    const p = mapToItem(root, mouse.x, mouse.y)
                    Monitors.moveGuide(guide.modelData.index, guide.vertical ? p.x / root.width : p.y / root.height)
                }
                onReleased: mouse => {
                    const p = mapToItem(root, mouse.x, mouse.y)
                    const f = guide.vertical ? p.x / root.width : p.y / root.height
                    if (f < 0 || f > 1)
                        Monitors.removeGuide(guide.modelData.index)
                }
                onDoubleClicked: Monitors.editGuide(guide.modelData.index)
            }
        }
    }
}
