pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Where the Library is (M5k): the bins opened as tabs (pills, the current one in the accent fill with black text; the
// project root first, a bin tab closes with its × or a middle click) and the breadcrumb of the bin shown (Project ›
// Footage › Day 1; a crumb opens that bin). Crumbs and tabs take drops: items dragged onto them move into that bin.
// While Find lists the whole project the breadcrumb says so instead.
Column {
    id: root
    objectName: "libraryBinBar"
    spacing: Theme.space(1)

    // Bin tabs: only when a bin was opened as a tab.
    ListView {
        id: tabStrip
        objectName: "libraryBinTabs"
        width: parent.width
        height: visible ? Theme.controlHeightSmall : 0
        visible: Library.tabs.length > 1
        orientation: ListView.Horizontal
        spacing: Theme.space(1)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: Library.tabs
        Accessible.role: Accessible.PageTabList
        Accessible.name: qsTr("Bins opened as tabs")

        delegate: AbstractButton {
            id: tab
            required property var modelData
            objectName: "libraryBinTab_" + (modelData.key === "" ? "root" : modelData.key)
            height: tabStrip.height
            width: Math.min(Theme.space(40), tabRow.implicitWidth + Theme.space(4.5))
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            Accessible.role: Accessible.PageTab
            Accessible.name: modelData.name
            onClicked: Library.openBin(modelData.key)

            contentItem: Row {
                id: tabRow
                spacing: Theme.space(1)
                leftPadding: Theme.space(2.5)
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: tab.modelData.key === "" ? "library" : "folder"
                    size: Theme.iconXs
                    color: tab.modelData.current ? Theme.accentOnFill : Theme.textTertiary
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, Theme.space(26))
                    text: tab.modelData.name
                    elide: Text.ElideRight
                    color: tab.modelData.current ? Theme.accentOnFill : Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Theme.weightLabel
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: tab.modelData.key !== ""
                    compact: true
                    width: Theme.iconSm + Theme.space(1)
                    height: width
                    iconName: "x"
                    iconSize: Theme.iconXs
                    iconColor: tab.modelData.current ? Theme.accentOnFill : Theme.textSecondary
                    tooltip: qsTr("Close bin tab")
                    onClicked: Library.closeTab(tab.modelData.key)
                }
            }
            background: Rectangle {
                // A pill: the current tab in the accent fill (design language v3), the others light up on hover.
                radius: height / 2
                color: tab.modelData.current ? Theme.accentFill : tab.hovered ? Theme.hover : "transparent"

                Behavior on color { ColorAnimation { duration: Theme.durFast } }

                FocusRing {
                    targetRadius: tab.height / 2
                    visible: tab.visualFocus
                }
            }

            TapHandler {
                acceptedButtons: Qt.MiddleButton
                onTapped: Library.closeTab(tab.modelData.key)
            }
            HoverHandler {
                enabled: tab.enabled
                cursorShape: Qt.PointingHandCursor
            }
            DropArea {
                anchors.fill: parent
                keys: ["application/x-wabcut-library"]
                onDropped: drop => {
                    Library.moveItems(drop.getDataAsString("application/x-wabcut-library").split(","), tab.modelData.key)
                    drop.accept(Qt.MoveAction)
                }
            }
        }
    }

    // Breadcrumb.
    Row {
        id: crumbs
        objectName: "libraryBreadcrumb"
        width: parent.width
        height: Theme.controlHeightSmall
        spacing: 0
        clip: true
        Accessible.role: Accessible.ToolBar
        Accessible.name: qsTr("Bin path")

        IconButton {
            objectName: "libraryUpButton"
            anchors.verticalCenter: parent.verticalCenter
            compact: true
            iconName: "chevron-left"
            tooltip: qsTr("Up one bin")
            enabled: Library.currentBin !== "" && !Library.filtering
            onClicked: Library.goUp()
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: Library.filtering
            leftPadding: Theme.space(1)
            //: Library breadcrumb while Find lists matches from every bin, e.g. "Found 12 in the project"
            text: qsTr("Found %n in the project", "", Library.items.count)
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Repeater {
            model: Library.filtering ? [] : Library.breadcrumb
            delegate: Row {
                id: crumb
                required property var modelData
                required property int index
                anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                Icon {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: crumb.index > 0
                    name: "chevron-right"
                    size: Theme.iconXs
                    color: Theme.textTertiary
                }
                AbstractButton {
                    id: crumbButton
                    objectName: "libraryCrumb_" + (crumb.modelData.key === "" ? "root" : crumb.modelData.key)
                    readonly property bool last: crumb.index === Library.breadcrumb.length - 1
                    height: Theme.controlHeightSmall
                    width: crumbText.implicitWidth + Theme.space(3)
                    hoverEnabled: true
                    focusPolicy: Qt.StrongFocus
                    onClicked: Library.openBin(crumb.modelData.key)
                    Accessible.name: crumb.modelData.name
                    contentItem: Text {
                        id: crumbText
                        text: crumb.modelData.name
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: crumbButton.last ? Theme.textPrimary : Theme.textSecondary
                        font.family: crumbButton.last ? Theme.fontDisplay : Theme.fontSans
                        font.pixelSize: Theme.textSm
                        font.weight: crumbButton.last ? Font.DemiBold : Font.Normal
                    }
                    background: Rectangle {
                        radius: Theme.radiusField
                        color: crumbDrop.containsDrag ? Theme.accentWash : crumbButton.hovered ? Theme.hover : "transparent"
                        border.width: crumbDrop.containsDrag ? Theme.hairline : 0
                        border.color: Theme.accentStroke
                        FocusRing {
                            targetRadius: Theme.radiusField
                            visible: crumbButton.visualFocus
                        }
                    }
                    HoverHandler {
                        enabled: crumbButton.enabled
                        cursorShape: Qt.PointingHandCursor
                    }
                    DropArea {
                        id: crumbDrop
                        anchors.fill: parent
                        keys: ["application/x-wabcut-library"]
                        onDropped: drop => {
                            Library.moveItems(drop.getDataAsString("application/x-wabcut-library").split(","), crumb.modelData.key)
                            drop.accept(Qt.MoveAction)
                        }
                    }
                }
            }
        }
    }
}
