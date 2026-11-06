pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Filter chips of the local model library (design language v3, as the Project panel's "All · Footage · Audio" chips):
// pills that wrap onto more lines when the page is narrow; the current one is inverted (the text colour as its fill).
// `model`: [{ id, name }]. Keyboard: one Tab stop (the current chip); Left / Right (and Home / End) pick another.
Flow {
    id: root

    property var model: []
    property string current: ""
    property string namePrefix: "localChip"
    property string accessibleName: ""
    signal activated(string id)

    readonly property var ids: root.model.map(m => m.id)
    property var chips: ({})

    function pick(index: int) {
        const id = root.ids[Math.max(0, Math.min(root.ids.length - 1, index))]
        if (id === undefined)
            return
        if (id !== root.current)
            root.activated(id)
        root.chips[id]?.forceActiveFocus(Qt.TabFocusReason)
    }

    spacing: Theme.space(1.5)
    Accessible.role: Accessible.PageTabList
    Accessible.name: root.accessibleName

    Repeater {
        model: root.model

        delegate: AbstractButton {
            id: chip
            required property var modelData
            required property int index
            readonly property bool current: modelData.id === root.current

            objectName: root.namePrefix + "_" + (modelData.id === "" ? "all" : modelData.id)
            height: Theme.controlHeightSmall
            width: chipText.implicitWidth + Theme.space(6)
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

            contentItem: Text {
                id: chipText
                text: chip.modelData.name
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                color: chip.current ? Theme.panel : Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.DemiBold
            }

            background: Rectangle {
                radius: height / 2
                color: chip.current ? Theme.textPrimary : chip.hovered ? Theme.hover : Theme.raised
                border.width: chip.current ? 0 : Theme.hairline
                border.color: Theme.border
                Behavior on color {
                    enabled: !Theme.reduceMotion
                    ColorAnimation { duration: Theme.durFast }
                }
                FocusRing {
                    targetRadius: height / 2
                    visible: chip.visualFocus
                }
            }
        }
    }
}
