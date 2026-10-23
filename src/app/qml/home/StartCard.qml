pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Shapes

// Start (the Home page, design language v3): three big actions - New project, Open project..., Import... - each with
// its key, then a compact drop well (files dropped anywhere on Home land here: `dragActive` lights it; a click chooses
// files), Import folder... and a link to the guide for timelines from Premiere, Resolve or Final Cut.
HomeCard {
    id: card
    objectName: "homeStartCard"

    property bool dragActive: false
    /// The keys' text for this platform ("Ctrl+N"), from HomeScreen's shortcuts.
    property string newKeys: ""
    property string openKeys: ""
    property string importKeys: ""
    /// How much shorter than its implicit height the card can be (the drop well at its smallest: short windows).
    readonly property real compressible: Math.max(0, zone.implicitHeight - zone.Layout.minimumHeight)

    signal newProjectRequested()
    signal openRequested()
    signal importRequested()
    signal importFolderRequested()

    /// New project takes the keyboard focus.
    function focusFirst() {
        newAction.forceActiveFocus(Qt.TabFocusReason)
    }

    title: qsTr("Start")
    hue: "lime"

    // A big action: a round tinted icon, the label, its key.
    component ActionRow: AbstractButton {
        id: action
        property string iconName: ""
        property string keys: ""
        property string hint: ""
        Layout.fillWidth: true
        implicitHeight: Theme.controlHeightLarge
        leftPadding: Theme.space(1)
        rightPadding: Theme.space(3)
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Accessible.role: Accessible.Button
        Accessible.name: text
        Accessible.description: action.hint
        Keys.onReturnPressed: action.clicked()
        Keys.onEnterPressed: action.clicked()

        background: Rectangle {
            radius: height / 2
            color: action.down ? Theme.hover : action.hovered ? Theme.hover : Theme.raised
            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }
        contentItem: RowLayout {
            spacing: Theme.space(3)
            Rectangle {
                implicitWidth: Theme.controlHeight
                implicitHeight: Theme.controlHeight
                radius: height / 2
                color: Theme.accentWash
                Icon {
                    anchors.centerIn: parent
                    name: action.iconName
                    size: Theme.iconSm
                    color: Theme.accentStroke
                }
            }
            Text {
                Layout.fillWidth: true
                text: action.text
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Theme.weightLabel
            }
            Rectangle {
                visible: action.keys !== ""
                implicitWidth: keysText.implicitWidth + Theme.space(3)
                implicitHeight: keysText.implicitHeight + Theme.space(1.5)
                radius: Theme.radiusBadge
                color: Theme.sunken
                Text {
                    id: keysText
                    anchors.centerIn: parent
                    text: action.keys
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            }
        }
        FocusRing {
            targetRadius: action.height / 2
            visible: action.visualFocus
        }
        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: false
        spacing: Theme.space(1)

        ActionRow {
            id: newAction
            objectName: "homeStartNew"
            iconName: "plus"
            text: qsTr("New project")
            keys: card.newKeys
            hint: qsTr("Choose a format and a name")
            onClicked: card.newProjectRequested()
        }
        ActionRow {
            objectName: "homeStartOpen"
            iconName: "folder-open"
            text: qsTr("Open project…")
            keys: card.openKeys
            hint: qsTr("Open a Wabcut project file")
            onClicked: card.openRequested()
        }
        ActionRow {
            objectName: "homeStartImport"
            iconName: "import"
            text: qsTr("Import…")
            keys: card.importKeys
            hint: qsTr("Media, game recordings, timelines or projects")
            onClicked: card.importRequested()
        }
    }

    // The drop well: files dropped anywhere on Home light it; a click chooses files.
    AbstractButton {
        id: zone
        objectName: "homeDropZone"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: Theme.space(14)
        implicitHeight: Theme.space(20)
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Drop files here, or choose files to import")
        Keys.onReturnPressed: zone.clicked()
        Keys.onEnterPressed: zone.clicked()
        onClicked: card.importRequested()

        background: Rectangle {
            radius: Theme.radiusCard
            color: card.dragActive ? Theme.accentWash : zone.hovered ? Theme.section : Theme.sunken
            Behavior on color { ColorAnimation { duration: Theme.durFast } }

            Shape {
                anchors.fill: parent
                ShapePath {
                    strokeColor: card.dragActive ? Theme.accentStroke : Theme.borderStrong
                    strokeWidth: card.dragActive ? Theme.focusRingWidth : Theme.hairline * 1.5
                    strokeStyle: ShapePath.DashLine
                    dashPattern: [4, 3]
                    fillColor: "transparent"
                    PathRectangle {
                        x: Theme.hairline
                        y: Theme.hairline
                        width: zone.width - Theme.hairline * 2
                        height: zone.height - Theme.hairline * 2
                        radius: Theme.radiusCard
                    }
                }
            }
        }
        contentItem: ColumnLayout {
            spacing: Theme.space(1)
            Item { Layout.fillHeight: true }
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: Theme.space(2)
                Icon {
                    name: "download"
                    size: Theme.iconMd
                    color: card.dragActive ? Theme.accentStroke : Theme.textSecondary
                }
                Text {
                    text: card.dragActive ? qsTr("Drop to import") : qsTr("Drop files here")
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textLg
                    font.weight: Theme.weightTitle
                }
            }
            Text {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.space(3)
                Layout.rightMargin: Theme.space(3)
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: qsTr("Media, game recordings, timelines or projects")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Item { Layout.fillHeight: true }
        }
        FocusRing {
            targetRadius: Theme.radiusCard
            visible: zone.visualFocus
        }
        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.space(3)
        HomeLink {
            objectName: "homeImportFolder"
            iconName: "folders"
            text: qsTr("Import folder…")
            onClicked: card.importFolderRequested()
        }
        HomeLink {
            objectName: "homeExchangeLink"
            iconName: "external-link"
            text: qsTr("From Premiere, Resolve or Final Cut?")
            onClicked: Qt.openUrlExternally(Home.guideUrl("exchange-timelines"))
        }
    }
}
