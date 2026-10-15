pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// The Clean up tab: filler words and silences found in the transcript (Transcript.review), each with a remove switch
// (on for confident finds), its kind, the words around it and its length. Find looks again with the thresholds;
// Preview strikes the words through and shows the cuts on the timeline; Remove applies them as one undo step.
Item {
    id: root
    objectName: "cleanupPage"

    readonly property bool hasWords: Transcript.wordCount > 0

    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.labelTracking
    }

    EmptyState {
        anchors.centerIn: parent
        width: parent.width - Theme.space(8)
        visible: !root.hasWords
        illustration: "transcript"
        text: qsTr("Transcribe the sequence first: clean-up finds filler words and long pauses in its words.")
    }

    ColumnLayout {
        anchors.fill: parent
        visible: root.hasWords
        spacing: Theme.space(2)

        // Thresholds
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Toggle {
                objectName: "reviewFillers"
                text: qsTr("Fillers")
                checked: Transcript.reviewFillers
                onToggled: Transcript.reviewFillers = checked
            }
            Item { Layout.fillWidth: true; Layout.preferredHeight: 1 }
            PillButton {
                objectName: "reviewFind"
                compact: true
                variant: "primary"
                iconName: "search"
                text: Transcript.reviewBusy ? qsTr("Cancel") : qsTr("Find")
                onClicked: Transcript.reviewBusy ? Transcript.cancelReview() : Transcript.refreshReview()
            }
        }
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Theme.space(2)
            rowSpacing: Theme.space(1)
            Text {
                Layout.fillWidth: true
                text: qsTr("Silences over")
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            NumberField {
                objectName: "reviewSilence"
                Layout.preferredWidth: Theme.numberFieldWidth
                from: 0
                to: 30
                decimals: 1
                stepSize: 0.1
                suffix: qsTr("s")
                value: Transcript.silenceSeconds
                onValueModified: v => Transcript.silenceSeconds = v
                Accessible.name: qsTr("Silences longer than, in seconds (0 = off)")
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Padding around cuts")
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            NumberField {
                objectName: "reviewPadding"
                Layout.preferredWidth: Theme.numberFieldWidth
                from: 0
                to: 500
                suffix: qsTr("ms")
                value: Transcript.paddingMs
                onValueModified: v => Transcript.paddingMs = Math.round(v)
                Accessible.name: qsTr("Padding kept around cuts, in milliseconds")
            }
        }

        BeamProgress {
            Layout.fillWidth: true
            visible: Transcript.reviewBusy
            value: Transcript.reviewProgress
            indeterminate: Transcript.reviewProgress === 0
            loader: "equalizer"
            Accessible.name: qsTr("Checking pause audio")
        }
        Text {
            Layout.fillWidth: true
            visible: Transcript.reviewBusy
            text: qsTr("Checking pause audio…")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        // Summary and remove all
        RowLayout {
            Layout.fillWidth: true
            visible: Transcript.review.length > 0
            GroupLabel {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: qsTr("%1 found · %2 to remove").arg(Transcript.review.length).arg(Transcript.reviewRemoveCount)
            }
            PillButton {
                objectName: "reviewRemoveAll"
                compact: true
                text: Transcript.reviewRemoveCount === Transcript.review.length ? qsTr("Keep all") : qsTr("Remove all")
                onClicked: Transcript.setAllReviewRemove(Transcript.reviewRemoveCount !== Transcript.review.length)
            }
        }

        ListView {
            id: items
            objectName: "reviewList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: Transcript.review
            spacing: Theme.space(1)
            boundsBehavior: Flickable.StopAtBounds
            activeFocusOnTab: false
            ScrollBar.vertical: ThinScrollBar {}

            delegate: Rectangle {
                id: item
                required property var modelData
                width: ListView.view ? ListView.view.width - Theme.space(3) : 0
                implicitHeight: row.implicitHeight + Theme.space(2)
                radius: Theme.radiusField
                color: modelData.remove ? Qt.alpha(Theme.danger, 0.08) : Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border

                RowLayout {
                    id: row
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.space(2)
                    anchors.rightMargin: Theme.space(1)
                    spacing: Theme.space(2)

                    Icon {
                        name: item.modelData.kind === "silence" ? "volume-x" : "type"
                        size: Theme.iconSm
                        color: item.modelData.kind === "silence" ? Theme.info : Theme.warning
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            Layout.fillWidth: true
                            text: item.modelData.label
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Font.DemiBold
                            font.strikeout: item.modelData.remove && Transcript.previewing
                        }
                        Text {
                            Layout.fillWidth: true
                            text: item.modelData.context
                            elide: Text.ElideRight
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                    }
                    Text {
                        text: item.modelData.durationText
                        color: Theme.textSecondary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textXs
                    }
                    Toggle {
                        objectName: "reviewRemove"
                        tooltip: item.modelData.remove ? qsTr("Remove “%1”").arg(item.modelData.label) : qsTr("Keep “%1”").arg(item.modelData.label)
                        checked: item.modelData.remove
                        onToggled: Transcript.setReviewRemove(item.modelData.index, checked)
                    }
                }
            }

            EmptyState {
                objectName: "cleanupEmptyState"
                anchors.centerIn: parent
                width: parent.width - Theme.space(8)
                visible: Transcript.review.length === 0 && !Transcript.reviewBusy
                illustration: "search"
                text: qsTr("Find lists the filler words and long pauses; choose what goes, then preview.")
            }
        }

        // Preview, then apply.
        RowLayout {
            Layout.fillWidth: true
            visible: Transcript.review.length > 0
            spacing: Theme.space(1)
            Text {
                objectName: "reviewPreviewInfo"
                Layout.fillWidth: true
                text: Transcript.previewing ? Transcript.previewInfo : Transcript.message
                elide: Text.ElideRight
                color: Transcript.previewing ? Theme.danger : Theme.warning
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            PillButton {
                objectName: "reviewPreview"
                visible: !Transcript.previewing
                compact: true
                text: qsTr("Preview")
                enabled: Transcript.reviewRemoveCount > 0
                onClicked: Transcript.previewReview()
            }
            PillButton {
                objectName: "reviewApply"
                visible: Transcript.previewing
                compact: true
                variant: "primary"
                text: qsTr("Remove")
                onClicked: Transcript.applyPreview()
            }
            PillButton {
                visible: Transcript.previewing
                compact: true
                text: qsTr("Cancel")
                onClicked: Transcript.cancelPreview()
            }
        }
    }
}
