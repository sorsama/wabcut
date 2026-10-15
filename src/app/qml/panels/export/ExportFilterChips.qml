pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// The Export panel's destination filter (design language v3, as the Project panel's "All · Footage · Audio" chips):
// pills that wrap onto more lines when the panel is narrow, each with its destination's hue dot; the current one is
// inverted (the text colour as its fill). `model`: [{ id, name, hue }]. Keyboard: one Tab stop (the current chip); Left /
// Right (and Home / End) pick the previous or next filter.
Flow {
    id: root
    objectName: "exportFilterChips"

    property var model: []
    property string current: "all"
    signal activated(string id)

    readonly property var ids: model.map(m => m.id)
    property var chips: ({})

    function pick(index) {
        const id = root.ids[Math.max(0, Math.min(root.ids.length - 1, index))]
        if (id === undefined)
            return
        if (id !== root.current)
            root.activated(id)
        root.chips[id]?.forceActiveFocus(Qt.TabFocusReason)
    }

    spacing: Theme.space(1.5)
    Accessible.role: Accessible.PageTabList
    Accessible.name: qsTr("Show presets for")

    Repeater {
        model: root.model

        delegate: AbstractButton {
            id: chip
            required property var modelData
            required property int index
            readonly property bool current: modelData.id === root.current

            objectName: "exportFilter_" + modelData.id
            height: Theme.controlHeightSmall
            width: chipRow.implicitWidth + Theme.space(6)
            hoverEnabled: true
            focusPolicy: current ? Qt.StrongFocus : Qt.ClickFocus
            Accessible.role: Accessible.PageTab
            Accessible.name: modelData.name
            Accessible.selected: current
            onClicked: root.activated(modelData.id)

            Keys.onShortcutOverride: event => event.accepted = event.modifiers === Qt.NoModifier
                                                           && [Qt.Key_Left, Qt.Key_Right, Qt.Key_Home, Qt.Key_End]
                                                              .indexOf(event.key) >= 0
            Keys.onPressed: event => {
                if (event.modifiers !== Qt.NoModifier)
                    return
                if (event.key === Qt.Key_Left)
                    root.pick(chip.index - 1)
                else if (event.key === Qt.Key_Right)
                    root.pick(chip.index + 1)
                else if (event.key === Qt.Key_Home)
                    root.pick(0)
                else if (event.key === Qt.Key_End)
                    root.pick(root.ids.length - 1)
                else
                    return
                event.accepted = true
            }
            Component.onCompleted: root.chips[modelData.id] = chip
            Component.onDestruction: {
                if (root.chips[modelData.id] === chip)
                    delete root.chips[modelData.id]
            }

            contentItem: Item {
                Row {
                    id: chipRow
                    anchors.centerIn: parent
                    spacing: Theme.space(1.5)

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: chip.modelData.hue !== undefined
                        width: Theme.dotSize
                        height: Theme.dotSize
                        radius: width / 2
                        color: chip.modelData.hue ?? "transparent"
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: chip.modelData.name
                        color: chip.current ? Theme.canvas : chip.hovered ? Theme.textPrimary : Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        font.weight: Theme.weightLabel

                        Behavior on color { ColorAnimation { duration: Theme.durFast } }
                    }
                }
            }
            background: Rectangle {
                radius: height / 2
                color: chip.current ? Theme.textPrimary : chip.hovered ? Theme.hover : Theme.section
                Behavior on color { ColorAnimation { duration: Theme.durFast } }

                FocusRing {
                    targetRadius: chip.height / 2
                    visible: chip.visualFocus
                }
            }
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
        }
    }
}
