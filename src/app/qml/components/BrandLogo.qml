import QtQuick

// The Wabcut logo (design language v3): the echo mark (brand violet and lime, independent of the accent; one ink in
// light and high-contrast themes) and the wordmark in Manrope ExtraBold, optionally with the version. Title bar, Home,
// the welcome dialog.
Row {
    id: root

    property bool showVersion: true
    property bool showWordmark: true
    // The mark's size; the wordmark follows it.
    property int markSize: Theme.space(7)

    spacing: Math.round(root.markSize * 0.3)

    EchoMark {
        objectName: "brandMark"
        anchors.verticalCenter: parent.verticalCenter
        size: root.markSize
        mono: !Theme.dark || Theme.highContrast
    }

    Text {
        visible: root.showWordmark
        anchors.verticalCenter: parent.verticalCenter
        //: Brand wordmark in the title bar
        text: qsTr("Wabcut")
        color: Theme.textPrimary
        font.family: Theme.fontDisplay
        font.pixelSize: Math.max(Theme.textTitle, Math.round(root.markSize * 0.56))
        font.weight: Theme.weightTitle
        font.letterSpacing: -0.01 * Math.max(Theme.textTitle, Math.round(root.markSize * 0.56))
    }

    Text {
        visible: root.showVersion
        anchors.verticalCenter: parent.verticalCenter
        text: qsTr("v%1").arg(Qt.application.version)
        color: Theme.textTertiary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textXs
    }
}
