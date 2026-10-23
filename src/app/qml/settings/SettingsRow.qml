import QtQuick

// One setting (0.1.3 Settings window): its label and an optional description on the left, the control(s) on the
// right. Rows after the first in a section draw a hairline above themselves.
// UX rework: `rowKey` names the row for the Settings search (AppSettings.index's `row`); SettingsPage.reveal(rowKey)
// scrolls to it and calls flash(), a short accent wash that fades (it only blinks off with reduce motion).
Item {
    id: row

    property string label: ""
    property string description: ""
    /// The row's key for the Settings search and AppSettings.openRequested (SettingsPage.reveal).
    property string rowKey: ""
    default property alias controls: slot.data

    /// Draws the eye to the row (the Settings search jumped here).
    function flash() {
        highlight.opacity = 1
        flashFade.restart()
    }

    width: parent ? parent.width : implicitWidth
    implicitHeight: Math.max(texts.implicitHeight, slot.implicitHeight) + Theme.space(5)

    Rectangle {
        visible: row.y > 0
        width: parent.width
        height: Theme.hairline
        color: Theme.border
    }

    // The flash: the accent wash over the whole row, a little wider than it (the section card's padding).
    Rectangle {
        id: highlight
        objectName: "settingsRowFlash"
        anchors.fill: parent
        anchors.leftMargin: -Theme.space(2)
        anchors.rightMargin: -Theme.space(2)
        anchors.topMargin: Theme.space(0.5)
        anchors.bottomMargin: Theme.space(0.5)
        radius: Theme.radiusField
        color: Theme.accentWash
        border.width: Theme.hairline
        border.color: Theme.accentStroke
        opacity: 0
        visible: opacity > 0
    }
    SequentialAnimation {
        id: flashFade
        PauseAnimation { duration: 1200 }
        NumberAnimation { target: highlight; property: "opacity"; to: 0; duration: Theme.durCollapse * 2; easing.type: Easing.OutCubic }
    }

    Column {
        id: texts
        anchors.left: parent.left
        anchors.right: slot.left
        anchors.rightMargin: Theme.space(4)
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space(0.5)

        Text {
            width: parent.width
            text: row.label
            wrapMode: Text.WordWrap
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        Text {
            width: parent.width
            visible: row.description !== ""
            text: row.description
            wrapMode: Text.WordWrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    Row {
        id: slot
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space(2)
    }
}
