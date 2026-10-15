pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The multi-camera view's angle grid (M5l, bridge `Multicam`), beside the program picture in the Program Monitor:
// every angle of the multi-camera clip under the playhead in one picture (2×2 up to 4×4), drawn by the export renderer
// at a reduced resolution, live while playing.
//
//   * Each cell: the angle's number and name, a speaker when it is heard; the angle in the picture has the active
//     border: yellow when paused, red while playing (switches are being recorded), as in Premiere.
//   * Click an angle (or press 1-9): Select Camera. Ctrl/⌘+click (or Ctrl+1-9): Cut to Camera. While playing both
//     record a switch at the playhead; the switches become one undo step when playback stops.
//   * Picture and sound / Picture / Sound (the pill's menu): what a switch changes.
//   * The footer shows the grid's GPU cost per frame (the request-to-readback time of one composite).
Item {
    id: root

    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Multi-camera angles")
    //: %1 = Select Camera 1's shortcut, %2 = Cut to Camera 1's
    Accessible.description: KeymapDispatcher.revision >= 0
                            ? qsTr("%1 selects angle 1 (and so on to 9), %2 cuts to it")
                                  .arg(KeymapDispatcher.shortcutText("multicam.selectCamera1"))
                                  .arg(KeymapDispatcher.shortcutText("multicam.cutToCamera1")) : ""

    readonly property int columns: Multicam.columns
    readonly property var angles: Multicam.angles
    readonly property bool playing: Playback.playing
    readonly property var stats: Multicam.gridStats

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(1.5)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)

            Text {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: root.width >= Theme.space(80) // a narrow monitor keeps the target pill only
                text: Multicam.sourceName !== "" ? Multicam.sourceName : qsTr("Multi-Camera")
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.Medium
            }
            // What a switch changes: a compact pill and its menu (fits a narrow monitor).
            PillButton {
                id: targetButton
                objectName: "multicamTarget"
                compact: true
                iconName: "chevron-down"
                text: Multicam.target === "video" ? qsTr("Picture") : Multicam.target === "audio" ? qsTr("Sound") : qsTr("Picture and sound")
                tooltip: qsTr("What a switch changes")
                onClicked: targetMenu.popup(targetButton, 0, targetButton.height + Theme.space(1))
                OptionsMenu {
                    id: targetMenu
                    options: [{ id: "both", name: qsTr("Picture and sound") }, { id: "video", name: qsTr("Picture") },
                              { id: "audio", name: qsTr("Sound") }]
                    current: Multicam.target
                    onChosen: id => Multicam.target = id
                }
            }
        }

        Rectangle {
            id: well
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radiusField
            color: Theme.videoBlack
            clip: true

            // The grid keeps the sequence's shape inside the well.
            Item {
                id: grid
                objectName: "multicamGrid"
                readonly property real aspect: ProjectController.frameWidth / Math.max(1, ProjectController.frameHeight)
                anchors.centerIn: parent
                width: Math.min(parent.width, parent.height * aspect)
                height: width / aspect
                visible: Multicam.clip !== ""

                StillFrame {
                    objectName: "multicamGridFrame"
                    anchors.fill: parent
                    image: Multicam.gridImage
                }

                Repeater {
                    model: root.angles.length > root.columns * root.columns ? root.columns * root.columns : root.angles.length

                    delegate: Item {
                        id: cell
                        required property int index
                        readonly property var angle: root.angles[index] ?? ({})
                        readonly property bool active: angle.video === true
                        objectName: "multicamAngle_" + (index + 1)
                        x: (index % root.columns) * grid.width / root.columns
                        y: Math.floor(index / root.columns) * grid.height / root.columns
                        width: grid.width / root.columns
                        height: grid.height / root.columns

                        /// Select Camera (or, with `cut`, Cut to Camera) for this angle.
                        function activate(cut) {
                            if (cut)
                                Multicam.cutToAngle(index + 1)
                            else
                                Multicam.selectAngle(index + 1)
                        }

                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Angle %1, %2").arg(index + 1).arg(angle.name ?? "")
                        Accessible.checked: active
                        Accessible.onPressAction: activate(false)

                        // Hover and the active border (yellow paused, red while recording).
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 1
                            color: cellTap.pressed ? Qt.alpha(Theme.textPrimary, 0.08) : "transparent"
                            border.width: cell.active ? Theme.selectionWidth + 1 : cellHover.hovered ? Theme.hairline : 0
                            border.color: cell.active ? (root.playing ? Theme.danger : Theme.warning) : Theme.borderStrong
                        }
                        // The angle's label: number, name, and a speaker when it is heard.
                        Rectangle {
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.margins: Theme.space(1)
                            width: Math.min(parent.width - Theme.space(2), labelRow.implicitWidth + Theme.space(2))
                            height: labelRow.implicitHeight + Theme.space(1)
                            radius: Theme.radiusBadge
                            color: Theme.scrim
                            Row {
                                id: labelRow
                                anchors.centerIn: parent
                                spacing: Theme.space(1)
                                Text {
                                    text: cell.angle.keyText !== "" ? cell.angle.keyText : String(cell.index + 1)
                                    color: cell.active ? (root.playing ? Theme.danger : Theme.warning) : Theme.scrimText
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.textXs
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    width: Math.min(implicitWidth, cell.width - Theme.space(12))
                                    text: cell.angle.name ?? ""
                                    elide: Text.ElideRight
                                    color: Theme.scrimText
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textXs
                                }
                                Icon {
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: cell.angle.audio === true
                                    name: "audio-lines"
                                    size: Theme.iconXs
                                    color: Theme.scrimText
                                }
                            }
                        }
                        HoverHandler {
                            id: cellHover
                            cursorShape: Qt.PointingHandCursor
                        }
                        TapHandler {
                            id: cellTap
                            onTapped: cell.activate((cellTap.point.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0)
                        }
                    }
                }
            }

            EmptyState {
                anchors.centerIn: parent
                width: parent.width - Theme.space(8)
                visible: Multicam.clip === ""
                illustration: "timeline"
                text: qsTr("Put the playhead over a multi-camera clip to see its angles.")
            }
        }

        // What the grid costs, and the switches waiting for the end of playback.
        Text {
            objectName: "multicamStatus"
            Layout.fillWidth: true
            elide: Text.ElideRight
            color: Multicam.pendingSwitches > 0 ? Theme.danger : Theme.textTertiary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textXs
            text: Multicam.pendingSwitches > 0
                  ? qsTr("Recording: %n switch(es)", "", Multicam.pendingSwitches)
                  : root.stats.error !== "" && root.stats.error !== undefined
                    ? root.stats.error
                    : (root.stats.steadyFrames ?? 0) > 0
                      //: The angle grid's size and its GPU cost per frame (mean, 95th percentile), e.g. "640×360 · 1.0 ms · p95 1.3"
                      ? qsTr("%1 · %2 ms · p95 %3").arg(root.stats.size ?? "")
                            .arg(Number(root.stats.meanMs ?? 0).toFixed(1)).arg(Number(root.stats.p95Ms ?? 0).toFixed(1))
                      //: The angle grid's size and how long its renderer took to open, e.g. "640×360 · opened in 160 ms"
                      : qsTr("%1 · opened in %2 ms").arg(root.stats.size ?? "").arg(Math.round(root.stats.openMs ?? 0))
        }
    }
}
