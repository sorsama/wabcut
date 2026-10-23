import QtQuick

// A section of a Settings page (0.1.3): the slice-mark header, an optional note, and its rows (SettingsRow, or any
// item) on one card (radius 8, hairline edge); rows draw a hairline between them.
// UX rework: a section that is one setting as a whole (Appearance's Theme and Custom colours) carries a `rowKey` for
// the Settings search like a SettingsRow, and flashes its card the same way.
Column {
    id: section

    property string title: ""
    property string description: ""
    /// The Settings search key when the whole section is one setting (SettingsPage.reveal).
    property string rowKey: ""
    default property alias content: body.data
    /// Without the card (a section whose content draws its own surfaces, e.g. an embedded AI page).
    property bool flat: false

    /// Draws the eye to the section's card (the Settings search jumped here).
    function flash() {
        highlight.opacity = 1
        flashFade.restart()
    }

    width: parent ? parent.width : implicitWidth
    spacing: Theme.space(2)

    SectionHeader {
        visible: section.title !== ""
        text: section.title
    }
    Text {
        width: parent.width
        visible: section.description !== ""
        text: section.description
        wrapMode: Text.WordWrap
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    Rectangle {
        width: parent.width
        height: body.implicitHeight + (section.flat ? 0 : Theme.space(2))
        radius: Theme.radiusCard
        color: section.flat ? "transparent" : Theme.raised
        border.width: section.flat ? 0 : Theme.hairline
        border.color: Theme.border

        Column {
            id: body
            x: section.flat ? 0 : Theme.space(4)
            y: section.flat ? 0 : Theme.space(1)
            width: parent.width - 2 * x
        }

        Rectangle {
            id: highlight
            objectName: "settingsSectionFlash"
            anchors.fill: parent
            radius: parent.radius
            color: Theme.accentWash
            border.width: Theme.hairline
            border.color: Theme.accentStroke
            opacity: 0
            visible: opacity > 0
        }
    }
    SequentialAnimation {
        id: flashFade
        PauseAnimation { duration: 1200 }
        NumberAnimation { target: highlight; property: "opacity"; to: 0; duration: Theme.durCollapse * 2; easing.type: Easing.OutCubic }
    }
}
