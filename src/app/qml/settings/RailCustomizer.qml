pragma ComponentBehavior: Bound
import QtQuick

// The left rail's editor (0.1.3, Settings > Interface; the rail's "Customize rail…"): every panel the rail offers, in
// its order, with a grip to drag it elsewhere, Up/Down buttons for the keyboard, and an eye that shows or hides it.
// Changes apply at once (Preferences.railLayout); Reset gives the default rail back. Help and Settings are not listed:
// they always stay at the rail's foot.
Column {
    id: root
    objectName: "railCustomizer"

    /// The rail's catalog (LeftRail.panels).
    property var panels: []
    readonly property var entries: Preferences.railEntries(root.panels)
    // A drag in progress: the row picked up and the place it would land.
    property int dragFrom: -1
    property int dragTo: -1

    function keys(): var {
        return root.entries.map(e => e.key)
    }
    function hiddenKeys(): var {
        return root.entries.filter(e => e.hidden).map(e => e.key)
    }
    /// Moves the entry at `from` to `to` (indexes in `entries`).
    function moveEntry(from: int, to: int) {
        const order = root.keys()
        if (from < 0 || from >= order.length || to < 0 || to >= order.length || from === to)
            return
        const key = order.splice(from, 1)[0]
        order.splice(to, 0, key)
        Preferences.setRailLayout(order, root.hiddenKeys())
    }
    function setHidden(key: string, hidden: bool) {
        const set = root.hiddenKeys().filter(k => k !== key)
        if (hidden)
            set.push(key)
        Preferences.setRailLayout(root.keys(), set)
    }
    function reset() {
        Preferences.setRailLayout([], [])
    }

    width: parent ? parent.width : implicitWidth
    spacing: 0

    Repeater {
        model: root.entries

        delegate: Item {
            id: row
            required property var modelData
            required property int index

            objectName: "railEntry_" + modelData.key
            width: root.width
            height: Theme.rowHeight + Theme.space(2)
            opacity: root.dragFrom === index ? 0.45 : 1

            // Where a dragged row would land (above or below this one).
            Rectangle {
                visible: root.dragFrom >= 0 && root.dragTo === row.index && root.dragTo !== root.dragFrom
                y: root.dragTo < root.dragFrom ? 0 : parent.height - height
                width: parent.width
                height: Theme.focusRingWidth
                radius: height / 2
                color: Theme.accentStroke
            }

            Icon {
                id: grip
                x: 0
                anchors.verticalCenter: parent.verticalCenter
                name: "grip-vertical"
                size: Theme.iconSm
                color: gripDrag.active ? Theme.textPrimary : Theme.textTertiary

                HoverHandler {
                    cursorShape: gripDrag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                }
                DragHandler {
                    id: gripDrag
                    target: null
                    xAxis.enabled: false
                    onActiveChanged: {
                        if (active) {
                            root.dragFrom = row.index
                            root.dragTo = row.index
                        } else {
                            const from = root.dragFrom
                            const to = root.dragTo
                            root.dragFrom = -1
                            root.dragTo = -1
                            root.moveEntry(from, to)
                        }
                    }
                    onActiveTranslationChanged: {
                        if (active)
                            root.dragTo = Math.max(0, Math.min(root.entries.length - 1,
                                                               row.index + Math.round(activeTranslation.y / row.height)))
                    }
                }
            }
            Icon {
                id: panelIcon
                anchors.left: grip.right
                anchors.leftMargin: Theme.space(3)
                anchors.verticalCenter: parent.verticalCenter
                name: row.modelData.icon
                size: Theme.iconSm
                color: row.modelData.hidden ? Theme.textDisabled : Theme.textSecondary
            }
            Text {
                anchors.left: panelIcon.right
                anchors.leftMargin: Theme.space(2)
                anchors.right: buttons.left
                anchors.rightMargin: Theme.space(2)
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.hidden ? qsTr("%1 (hidden)").arg(row.modelData.label) : row.modelData.label
                elide: Text.ElideRight
                color: row.modelData.hidden ? Theme.textTertiary : Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            Row {
                id: buttons
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space(0.5)

                IconButton {
                    objectName: "railUp_" + row.modelData.key
                    compact: true
                    iconName: "chevron-up"
                    enabled: row.index > 0
                    tooltip: qsTr("Move %1 up").arg(row.modelData.label)
                    onClicked: root.moveEntry(row.index, row.index - 1)
                }
                IconButton {
                    objectName: "railDown_" + row.modelData.key
                    compact: true
                    iconName: "chevron-down"
                    enabled: row.index < root.entries.length - 1
                    tooltip: qsTr("Move %1 down").arg(row.modelData.label)
                    onClicked: root.moveEntry(row.index, row.index + 1)
                }
                IconButton {
                    objectName: "railVisible_" + row.modelData.key
                    compact: true
                    iconName: row.modelData.hidden ? "eye-off" : "eye"
                    checkable: true
                    checked: !row.modelData.hidden
                    checkedColor: Theme.hover
                    checkedIconColor: Theme.textPrimary
                    tooltip: row.modelData.hidden ? qsTr("Show %1 in the rail").arg(row.modelData.label)
                                                  : qsTr("Hide %1 from the rail").arg(row.modelData.label)
                    onClicked: {
                        checked = Qt.binding(() => !row.modelData.hidden)
                        root.setHidden(row.modelData.key, !row.modelData.hidden)
                    }
                }
            }
        }
    }

    Item {
        width: root.width
        height: Theme.controlHeight + Theme.space(3)

        PillButton {
            objectName: "railReset"
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            compact: true
            variant: "ghost"
            iconName: "rotate-ccw"
            text: qsTr("Reset rail")
            enabled: Preferences.railLayout !== ""
            onClicked: root.reset()
        }
    }
}
