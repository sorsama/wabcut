pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Show/Hide Tracks (0.7, card 032; Premiere's Audio Track Mixer menu): the mixer's strips as a checklist - the track's
// colour, its short name and name, and a switch (on: the strip shows). Show All brings every strip back. A hidden strip
// stays in the mix and keeps its mute, solo and fader: only the mixer's room changes (the choice lasts for the session,
// per sequence). The list scrolls inside the card when the sequence has many tracks.
Popover {
    id: root
    objectName: "showHideTracksPopover"

    title: qsTr("Show/Hide Tracks")
    transformOrigin: Item.TopRight

    /// The tracks, then the submixes, as the mixer lists them.
    readonly property var rows: AudioMixer.strips.concat(AudioMixer.submixes)

    Item {
        id: frame
        implicitWidth: Theme.space(64)
        implicitHeight: column.implicitHeight

        ColumnLayout {
            id: column
            width: frame.width
            spacing: Theme.space(2)

            Text {
                Layout.fillWidth: true
                text: qsTr("A hidden track still plays: only the mixer's room changes.")
                wrapMode: Text.Wrap
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }

            Flickable {
                id: scroller
                objectName: "showHideTracksScroll"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(list.implicitHeight, Theme.space(72))
                contentWidth: width
                contentHeight: list.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                interactive: contentHeight > height
                ScrollBar.vertical: ThinScrollBar {}

                Column {
                    id: list
                    width: scroller.width - (scroller.interactive ? Theme.space(3) : 0)
                    spacing: Theme.space(0.5)

                    Repeater {
                        model: root.rows
                        delegate: RowLayout {
                            id: row
                            required property var modelData
                            width: list.width
                            spacing: Theme.space(2)

                            Rectangle {
                                Layout.alignment: Qt.AlignVCenter
                                implicitWidth: Theme.dotSize
                                implicitHeight: Theme.dotSize
                                radius: Theme.dotSize / 2
                                color: Theme.clipColor(row.modelData.colorIndex ?? 0)
                            }
                            Text {
                                Layout.preferredWidth: Theme.space(9)
                                text: row.modelData.shortName ?? ""
                                color: Theme.textSecondary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textSm
                            }
                            Toggle {
                                id: toggle
                                objectName: "showHideTrack_" + row.modelData.key
                                Layout.fillWidth: true
                                text: row.modelData.name ?? ""
                                tooltip: qsTr("Show %1 in the mixer").arg(row.modelData.name ?? "")
                                checked: row.modelData.hidden !== true
                                onToggled: {
                                    AudioMixer.setTrackHidden(row.modelData.key, !toggle.checked)
                                    toggle.checked = Qt.binding(() => row.modelData.hidden !== true)
                                }
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: AudioMixer.hiddenCount > 0 ? qsTr("%n track(s) hidden", "", AudioMixer.hiddenCount) : qsTr("All tracks shown")
                    elide: Text.ElideRight
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                PillButton {
                    objectName: "showAllTracksButton"
                    compact: true
                    text: qsTr("Show All")
                    enabled: AudioMixer.hiddenCount > 0
                    onClicked: AudioMixer.showAllTracks()
                }
            }
        }
    }
}
