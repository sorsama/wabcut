pragma ComponentBehavior: Bound
import QtQuick

// The shortcuts keyboard: a US keyboard (Shortcuts.keyboard, positions in key units) scaled to fit, each key filled
// by what it does with `modifiers` held (Shortcuts.keyAssignments): application commands (Theme.keyApplication),
// panel commands (Theme.keyPanel), both (split), or nothing. Clicking a modifier key toggles it (modifierToggled);
// clicking another key selects it (`selectedId`, keySelected). A command dragged from the list onto a key asks for that chord
// (commandDropped). Keys are pointer targets only: the command list is the keyboard path to every shortcut.
Item {
    id: root
    objectName: "shortcutsKeyboard"

    property int modifiers: 0
    property string selectedId: ""
    readonly property var assignments: Shortcuts.revision >= 0 ? Shortcuts.keyAssignments(modifiers) : ({})
    readonly property real unit: Math.min(Theme.keyUnit, width / 22.75)
    readonly property real keyGap: Math.max(2, Math.round(unit * 0.08))

    signal keySelected(var key)
    signal modifierToggled(int bit)
    signal commandDropped(string commandId, var key)

    implicitWidth: Theme.keyUnit * 22.75
    implicitHeight: Math.ceil(unit * 6.25)

    Repeater {
        model: Shortcuts.keyboard
        delegate: Item {
            id: cap
            required property var modelData
            readonly property var info: root.assignments[modelData.id]
            readonly property string kind: modelData.modifier !== 0 ? "modifier" : info ? info.kind : ""
            readonly property bool held: modelData.modifier !== 0 && (root.modifiers & modelData.modifier) !== 0
            readonly property bool selected: root.selectedId === modelData.id && modelData.modifier === 0
            readonly property color fill: kind === "application" || kind === "both" ? Theme.keyApplication
                                        : kind === "panel" ? Theme.keyPanel
                                        : held ? Theme.accentFill
                                        : kind === "modifier" ? Theme.sunken : Theme.raised
            readonly property color ink: kind === "application" || kind === "panel" || kind === "both" ? Theme.clipInk
                                       : held ? Theme.accentOnFill : Theme.textSecondary

            objectName: "key_" + modelData.id + (modelData.modifier !== 0 ? "_" + modelData.x : "")
            x: Math.round(modelData.x * root.unit)
            y: Math.round(modelData.y * root.unit)
            width: Math.round(modelData.w * root.unit) - root.keyGap
            height: Math.round(modelData.h * root.unit) - root.keyGap

            Rectangle {
                id: face
                anchors.fill: parent
                radius: Theme.radiusBadge + 1
                color: cap.fill
                border.width: cap.selected || keyDrop.containsDrag ? Theme.focusRingWidth : Theme.hairline
                border.color: cap.selected || keyDrop.containsDrag ? Theme.accentStroke
                            : cap.kind === "" || cap.kind === "modifier" ? Theme.border : Qt.alpha(Theme.clipInk, 0.25)
                Behavior on color { ColorAnimation { duration: Theme.durFast } }

                // Both kinds: the right half in the panel colour.
                Rectangle {
                    visible: cap.kind === "both"
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.margins: face.border.width
                    width: (parent.width - face.border.width * 2) / 2
                    radius: Theme.radiusBadge
                    color: Theme.keyPanel
                }
            }

            Text {
                id: label
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: Math.round(root.unit * 0.14)
                anchors.rightMargin: Math.round(root.unit * 0.1)
                anchors.topMargin: Math.round(root.unit * 0.08)
                text: cap.modelData.label
                elide: Text.ElideRight
                color: cap.ink
                font.family: Theme.fontSans
                font.pixelSize: root.unit >= 32 ? Theme.textSm : Theme.textXs
                font.weight: Font.DemiBold
            }
            // The command the key runs (the first one), small, under the label.
            Text {
                anchors.left: label.left
                anchors.right: label.right
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Math.round(root.unit * 0.06)
                visible: cap.info !== undefined && root.unit >= 30 && cap.height >= root.unit * 0.8
                text: cap.info ? cap.info.names[0] : ""
                elide: Text.ElideRight
                color: Qt.alpha(Theme.clipInk, 0.8)
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }

            MouseArea {
                id: area
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (cap.modelData.modifier !== 0) {
                        root.modifierToggled(cap.modelData.modifier)
                        return
                    }
                    root.selectedId = cap.modelData.id
                    root.keySelected(cap.modelData)
                }
            }

            DropArea {
                id: keyDrop
                anchors.fill: parent
                enabled: cap.modelData.modifier === 0
                keys: ["application/x-wabcut-command"]
                onDropped: drop => {
                    root.commandDropped(drop.getDataAsString("application/x-wabcut-command"), cap.modelData)
                    drop.accept(Qt.CopyAction)
                }
            }

            Loader {
                active: area.containsMouse && cap.info !== undefined
                sourceComponent: Tooltip {
                    parent: cap
                    visible: true
                    text: cap.info ? cap.info.names.join(" · ") : ""
                }
            }
        }
    }
}
