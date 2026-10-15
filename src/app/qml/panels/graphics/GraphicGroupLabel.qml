import QtQuick
import QtQuick.Layouts

// A group's label in the Essential Graphics panel (the caption style panel's look: secondary, semibold, tracked).
Text {
    Layout.fillWidth: true
    Layout.topMargin: Theme.space(1)
    elide: Text.ElideRight
    color: Theme.textSecondary
    font.family: Theme.fontSans
    font.pixelSize: Theme.textSm
    font.weight: Font.DemiBold
    font.letterSpacing: Theme.labelTracking
    Accessible.role: Accessible.Heading
    Accessible.name: text
}
