pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// P1 round (#39): Premiere's Properties (File > Get Properties for Selection, Ctrl/Cmd+Shift+H, and Properties… in the
// Library's item menu): what the project knows of a Library item - its file's format, length, codec and profile, frame
// rate (variable or not), pixel format and bit depth, chroma, fields, alpha, rotation, every audio stream with its title,
// file size and data rate, proxy, how many clips use it and where it lives (Library.mediaProperties, nothing probed
// again; a sequence shows its settings, a bin what it holds). Copy all puts it on the clipboard as text; Reveal shows
// the file in the file manager.
WabcutDialog {
    id: dialog
    objectName: "propertiesDialog"

    property string key: ""
    property var info: ({})
    property var lines: []
    property string note: ""

    /// Shows the properties of `itemKey` ("m7", "s3", "b4").
    function openFor(itemKey: string) {
        dialog.key = itemKey
        dialog.info = Library.mediaProperties(itemKey)
        dialog.lines = Library.propertyLines(dialog.info)
        dialog.note = ""
        dialog.open()
    }

    //: Title of the Properties dialog, e.g. "Properties: Match 3.mp4"
    title: qsTr("Properties: %1").arg(dialog.info.name ?? "")

    Text {
        Layout.fillWidth: true
        visible: dialog.lines.length === 0
        text: qsTr("This item is no longer in the project.")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    ListView {
        id: rows
        objectName: "propertiesList"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight,
                                         Math.max(Theme.space(30), (dialog.parent ? dialog.parent.height : 0) - Theme.space(70)))
        visible: count > 0
        clip: true
        model: dialog.lines
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}
        Accessible.role: Accessible.List
        Accessible.name: dialog.title

        delegate: Item {
            id: line
            required property var modelData
            required property int index
            width: ListView.view.width
            height: Math.max(label.implicitHeight, value.implicitHeight) + Theme.space(2)
            Accessible.role: Accessible.StaticText
            Accessible.name: line.modelData.label + ": " + line.modelData.value

            Rectangle {
                anchors.fill: parent
                radius: Theme.radiusField
                color: line.index % 2 === 0 ? Theme.sunken : "transparent"
            }
            Text {
                id: label
                x: Theme.space(3)
                width: Theme.space(32)
                anchors.verticalCenter: parent.verticalCenter
                text: line.modelData.label
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                elide: Text.ElideRight
            }
            Text {
                id: value
                objectName: "propertiesValue_" + line.index
                anchors.left: label.right
                anchors.right: parent.right
                anchors.leftMargin: Theme.space(2)
                anchors.rightMargin: Theme.space(3)
                anchors.verticalCenter: parent.verticalCenter
                text: line.modelData.value
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            }
        }
    }

    Text {
        objectName: "propertiesNote"
        Layout.fillWidth: true
        visible: dialog.note !== ""
        text: dialog.note
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            objectName: "propertiesReveal"
            visible: (dialog.info.path ?? "") !== "" && dialog.info.offline !== true
            text: Qt.platform.os === "osx" ? qsTr("Reveal in Finder") : qsTr("Reveal in File Explorer")
            iconName: "folder-open"
            onClicked: Library.reveal(dialog.key)
        },
        PillButton {
            objectName: "propertiesCopy"
            visible: dialog.lines.length > 0
            text: qsTr("Copy all")
            iconName: "copy"
            onClicked: dialog.note = Library.copyProperties(dialog.key) ? qsTr("Copied to the clipboard.")
                                                                         : qsTr("Nothing was copied.")
        },
        PillButton {
            objectName: "propertiesClose"
            text: qsTr("Close")
            variant: "primary"
            onClicked: dialog.close()
        }
    ]
}
