pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// What changed between two versions (M5m, History.comparison): "Version 12 → Now", the changes as lines with
// timecodes, then per sequence its clips added (green), removed (red) and changed (amber: moved, trimmed, effects,
// keyframes...). The timeline highlights the current sequence's changed ranges meanwhile; a later edit makes the
// comparison stale (it says so) and drops the highlights.
Rectangle {
    id: root

    /// History.comparison.
    property var comparison: ({})
    readonly property var sequences: comparison.sequences ?? []
    readonly property var summary: comparison.summary ?? []

    function kindColor(kind) {
        return kind === "added" ? Theme.success : kind === "removed" ? Theme.danger : Theme.warning
    }

    radius: Theme.radiusCard
    color: Theme.sunken
    border.width: Theme.hairline
    border.color: Theme.border
    implicitHeight: Theme.space(60)

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.space(2)
        spacing: Theme.space(1.5)

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("%1 → %2").arg(root.comparison.fromText ?? "").arg(root.comparison.toText ?? "")
                elide: Text.ElideRight
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
            }
            IconButton {
                objectName: "historyCompareClose"
                compact: true
                iconName: "x"
                tooltip: qsTr("Close the comparison")
                onClicked: History.clearComparison()
            }
        }
        Text {
            Layout.fillWidth: true
            visible: root.comparison.stale === true
            text: qsTr("The project changed since: compare again to see the latest.")
            wrapMode: Text.Wrap
            color: Theme.warning
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            Layout.fillWidth: true
            visible: root.comparison.empty === true
            text: qsTr("No changes.")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: content.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}

            ColumnLayout {
                id: content
                width: flick.width - Theme.space(2)
                spacing: Theme.space(1.5)

                // The changes, one line each.
                Repeater {
                    model: root.summary
                    delegate: Text {
                        required property string modelData
                        Layout.fillWidth: true
                        text: modelData
                        wrapMode: Text.Wrap
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: [root.comparison.mediaText ?? "", root.comparison.notesText ?? ""].filter(t => t !== "").join("  ·  ")
                    wrapMode: Text.Wrap
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }

                // Per sequence.
                Repeater {
                    model: root.sequences
                    delegate: ColumnLayout {
                        id: section
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Theme.space(0.5)

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.space(1)
                            Rectangle {
                                implicitWidth: Theme.dotSize
                                implicitHeight: Theme.dotSize
                                radius: Theme.dotSize / 2
                                color: root.kindColor(section.modelData.kind)
                            }
                            Text {
                                Layout.fillWidth: true
                                text: section.modelData.name + (section.modelData.current ? "  ·  " + qsTr("shown on the timeline") : "")
                                elide: Text.ElideRight
                                color: Theme.textPrimary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                                font.weight: Font.DemiBold
                            }
                            Text {
                                text: section.modelData.countsText
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textXs
                            }
                        }
                        Repeater {
                            model: section.modelData.clips
                            delegate: RowLayout {
                                id: clipRow
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.leftMargin: Theme.space(3)
                                spacing: Theme.space(1.5)
                                Rectangle {
                                    implicitWidth: Theme.space(1)
                                    implicitHeight: Theme.space(3)
                                    radius: Theme.radiusBadge / 2
                                    color: root.kindColor(clipRow.modelData.kind)
                                }
                                Text {
                                    text: clipRow.modelData.track
                                    color: Theme.textTertiary
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.textXs
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: clipRow.modelData.name + (clipRow.modelData.whatText !== "" ? "  ·  " + clipRow.modelData.whatText : "")
                                    elide: Text.ElideRight
                                    color: Theme.textSecondary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textSm
                                }
                                Text {
                                    text: clipRow.modelData.kind === "added" ? clipRow.modelData.afterText
                                        : clipRow.modelData.kind === "removed" ? clipRow.modelData.beforeText
                                        : clipRow.modelData.beforeText === clipRow.modelData.afterText ? clipRow.modelData.afterText
                                        : clipRow.modelData.beforeText + "  →  " + clipRow.modelData.afterText
                                    color: Theme.textTertiary
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.textXs
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
