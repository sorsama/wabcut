pragma ComponentBehavior: Bound
import QtQuick

// Sequence tabs above the timeline (M5i, SequenceTabs): Premiere's open sequences. Click a tab to show its sequence
// (each keeps its playhead, zoom, scroll and targets), × or a middle click closes it (not the last one), drag a tab
// sideways to reorder. Ctrl+Tab / Ctrl+Shift+Tab cycle them from the Timeline (TimelinePanel.runCommand).
// Design language v3: Chrome-style tabs on a dark strip; the current one is cut from the panel's surface (rounded
// top corners, flared feet that run into the panel below), the others are text with hairline separators.
// 0.2, Premiere 25.x: with Show Sequence Label Color (the timeline display settings) a swatch of the sequence's label
// colour sits before its name; a right click opens the Label menu (the same label as in the Project panel).
Item {
    id: root

    objectName: "sequenceTabs"
    implicitHeight: Theme.sequenceTabHeight
    Accessible.role: Accessible.PageTabList
    Accessible.name: qsTr("Open sequences")

    // The tab being dragged and where it would land.
    property int dragFrom: -1
    property int dragTo: -1

    // The strip the tabs sit in.
    Rectangle {
        anchors.fill: parent
        color: Theme.sunken
        topLeftRadius: Theme.radiusTab
        topRightRadius: Theme.radiusTab
    }

    // 0.2, the tab's right-click menu: Premiere's Label (the 16 label colours, Library.labelNames).
    ContextMenu {
        id: tabMenu
        objectName: "sequenceTabMenu"
        property string sequence: ""
        function openFor(sequenceId, target, point) {
            sequence = sequenceId
            popup(target, point.x, point.y)
        }
        ContextMenu {
            id: tabLabelMenu
            objectName: "sequenceTabLabelMenu"
            title: qsTr("Label")
            Instantiator {
                model: Library.labelNames
                delegate: ContextMenuItem {
                    required property string modelData
                    required property int index
                    objectName: "sequenceTabLabelItem_" + index
                    text: modelData
                    iconName: "square"
                    iconColor: Theme.clipColor(index)
                    onTriggered: SequenceTabs.setLabel(tabMenu.sequence, index)
                }
                onObjectAdded: (index, object) => tabLabelMenu.insertItem(index, object as ContextMenuItem)
                onObjectRemoved: (index, object) => tabLabelMenu.removeItem(object as ContextMenuItem)
            }
        }
    }

    ListView {
        id: list
        anchors.fill: parent
        // Room inside the clip for the first and last tabs' flares.
        leftMargin: Theme.radiusTab
        rightMargin: Theme.radiusTab
        orientation: ListView.Horizontal
        spacing: 0
        clip: true
        interactive: false
        boundsBehavior: Flickable.StopAtBounds
        model: SequenceTabs.tabs

        delegate: Item {
            id: tab

            required property var modelData
            required property int index
            readonly property bool current: modelData.current
            readonly property bool closable: SequenceTabs.count > 1

            objectName: "sequenceTab_" + modelData.id
            readonly property bool showLabel: Preferences.showSequenceLabelColor
            width: Math.min(Theme.sequenceTabMaxWidth, glyph.width + label.implicitWidth + Theme.space(6.5)
                                                       + (closable ? close.width : 0)
                                                       + (showLabel ? swatch.width + Theme.space(1.5) : 0))
            height: list.height
            z: current ? 1 : 0
            opacity: root.dragFrom === index ? 0.6 : 1
            Accessible.role: Accessible.PageTab
            Accessible.name: modelData.name
            Accessible.selected: current

            // The tab: the panel's surface for the current one, a hover tint for the others.
            Rectangle {
                id: face
                anchors.fill: parent
                anchors.topMargin: Theme.space(1)
                topLeftRadius: Theme.radiusTab
                topRightRadius: Theme.radiusTab
                color: tab.current ? Theme.panel : area.containsMouse ? Theme.hover : "transparent"
                Behavior on color { ColorAnimation { duration: Theme.durFast } }
            }
            // The current tab's feet: the panel's surface curving out into the strip on both sides (only the surface is
            // drawn, so a hovered neighbour shows through the curve).
            Repeater {
                model: tab.current ? [-1, 1] : []

                delegate: TabFlare {
                    required property int modelData
                    side: modelData
                    x: side < 0 ? -width : tab.width
                    y: tab.height - height
                    color: Theme.panel
                }
            }
            // A hairline between two tabs that are not the current one.
            Rectangle {
                readonly property var next: SequenceTabs.tabs[tab.index + 1] ?? null
                visible: !tab.current && root.dragTo < 0 && next !== null && !next.current
                anchors.right: parent.right
                anchors.verticalCenter: face.verticalCenter
                width: Theme.hairline
                height: face.height - Theme.space(3)
                color: Theme.borderStrong
            }
            // Where a dragged tab would drop.
            Rectangle {
                visible: root.dragTo === tab.index && root.dragFrom !== tab.index
                x: root.dragTo < root.dragFrom ? 0 : parent.width - width
                width: 2
                height: parent.height
                color: Theme.accentStroke
            }

            // The sequence glyph: stacked track lines, in the accent on the current tab.
            Icon {
                id: glyph
                anchors.left: parent.left
                anchors.leftMargin: Theme.space(2.5)
                anchors.verticalCenter: face.verticalCenter
                name: "align-left"
                size: Theme.iconXs
                color: tab.current ? Theme.accentStroke : Theme.textTertiary
            }
            // 0.2: the sequence's label colour (Show Sequence Label Color).
            Rectangle {
                id: swatch
                objectName: "sequenceTabLabel_" + tab.modelData.id
                visible: tab.showLabel
                anchors.left: glyph.right
                anchors.leftMargin: Theme.space(1.5)
                anchors.verticalCenter: face.verticalCenter
                width: Theme.space(2.5)
                height: width
                radius: Theme.radiusBadge / 2
                color: Theme.clipColor(tab.modelData.label ?? 0)
                border.width: Theme.hairline
                border.color: Theme.clipBorder
            }
            Text {
                id: label
                anchors.left: tab.showLabel ? swatch.right : glyph.right
                anchors.leftMargin: Theme.space(1.5)
                anchors.right: tab.closable ? close.left : parent.right
                anchors.verticalCenter: face.verticalCenter
                text: tab.modelData.name
                elide: Text.ElideRight
                color: tab.current ? Theme.textPrimary : Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: tab.current ? Font.Bold : Font.Medium
            }

            MouseArea {
                id: area
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
                cursorShape: Qt.PointingHandCursor
                property real pressX: 0
                onPressed: mouse => {
                    pressX = mouse.x
                    if (mouse.button === Qt.LeftButton)
                        SequenceTabs.activate(tab.modelData.id)
                    else if (mouse.button === Qt.RightButton)
                        tabMenu.openFor(tab.modelData.id, area, Qt.point(mouse.x, mouse.y)) // 0.2: Label
                }
                onPositionChanged: mouse => {
                    if (!(mouse.buttons & Qt.LeftButton))
                        return
                    if (root.dragFrom < 0 && Math.abs(mouse.x - pressX) > Theme.space(2))
                        root.dragFrom = tab.index
                    if (root.dragFrom >= 0) {
                        const p = mapToItem(list.contentItem, mouse.x, height / 2)
                        const over = list.indexAt(p.x, p.y)
                        root.dragTo = over >= 0 ? over : (p.x < 0 ? 0 : list.count - 1)
                    }
                }
                onReleased: mouse => {
                    if (root.dragFrom >= 0 && root.dragTo >= 0)
                        SequenceTabs.move(root.dragFrom, root.dragTo)
                    root.dragFrom = -1
                    root.dragTo = -1
                }
                onClicked: mouse => {
                    if (mouse.button === Qt.MiddleButton && tab.closable)
                        SequenceTabs.close(tab.modelData.id)
                }
            }

            IconButton {
                id: close
                objectName: "sequenceTabClose_" + tab.modelData.id
                visible: tab.closable
                anchors.right: parent.right
                anchors.rightMargin: Theme.space(0.5)
                anchors.verticalCenter: face.verticalCenter
                compact: true
                iconName: "x"
                iconSize: Theme.iconXs
                tooltip: qsTr("Close %1").arg(tab.modelData.name)
                // On the current tab and under the pointer, as browsers do.
                opacity: tab.current || area.containsMouse || hovered || activeFocus ? 1 : 0
                onClicked: SequenceTabs.close(tab.modelData.id)
            }
        }
    }
}
