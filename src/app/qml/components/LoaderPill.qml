import QtQuick

// A loading state with its label (design language v3, the brand sheet): a 20 px EchoLoader and the job's name in a
// raised pill, e.g. "Analyzing audio". `variant` picks the loader (equalizer, chase, scan, build).
Rectangle {
    id: root

    property string variant: "chase"
    property string text: ""
    property bool running: visible

    implicitHeight: Theme.space(8.5)
    implicitWidth: Theme.space(2.5) + row.implicitWidth + (label.visible ? Theme.space(3.5) : Theme.space(2.5))
    radius: height / 2
    color: Theme.raised

    Accessible.role: Accessible.ProgressBar
    Accessible.name: root.text !== "" ? root.text : qsTr("Working")
    Accessible.description: qsTr("Working")

    Row {
        id: row
        x: Theme.space(2.5)
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space(2.5)

        EchoLoader {
            objectName: "loaderPillLoader"
            anchors.verticalCenter: parent.verticalCenter
            size: Theme.space(5)
            variant: root.variant
            running: root.running
        }
        Text {
            id: label
            visible: root.text !== ""
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, Math.max(0, root.width - Theme.space(13.5)))
            text: root.text
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Theme.weightLabel
        }
    }
}
