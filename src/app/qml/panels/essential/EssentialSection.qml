pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// A section of the Essential Sound panel (M5m): a sunken card with the section's title, a one-line hint and its switch
// in the header, its controls below while it is on. The switch turns the section's marked effects on or off (they
// keep their settings, as Premiere's checkbox); a section with nothing built yet asks the owner to build it.
Rectangle {
    id: root

    property string title: ""
    property string hint: ""
    /// The section's state from EssentialSound.sections: "on", "off" (its effects are bypassed) or "" (none built).
    property string sectionState: ""
    /// Whether the body shows (default: while the section is on).
    property bool expanded: sectionState === "on"
    property bool switchable: true
    default property alias content: body.data

    /// The switch was flipped by the user.
    signal switched(bool on)

    Layout.fillWidth: true
    implicitHeight: column.implicitHeight + Theme.space(5)
    radius: Theme.radiusField
    color: Theme.sunken
    border.width: Theme.hairline
    border.color: root.sectionState === "off" ? Theme.borderStrong : Theme.border

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: Theme.space(3)
        spacing: Theme.space(2)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    text: root.title
                    elide: Text.ElideRight
                    color: root.sectionState === "off" ? Theme.textTertiary : Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: root.sectionState === "off" ? qsTr("Bypassed: its effects are off") : root.hint
                    wrapMode: Text.Wrap
                    color: root.sectionState === "off" ? Theme.warning : Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                }
            }
            Toggle {
                visible: root.switchable
                tooltip: root.title
                checked: root.sectionState === "on"
                onToggled: {
                    root.switched(checked)
                    checked = Qt.binding(() => root.sectionState === "on") // the model decides
                }
                Accessible.name: root.title
            }
        }
        ColumnLayout {
            id: body
            Layout.fillWidth: true
            visible: root.expanded
            spacing: Theme.space(2)
        }
    }
}
