pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The Remix preview's report and its actions (M5m; bridge `EssentialSound`): the target and the result, each join with
// how seamless it is, the warnings, Apply / Cancel and the menu (fit to video, render as a take). Shown in the timeline's
// Remix popover and in the Essential Sound panel's Duration section.
ColumnLayout {
    id: summary
    readonly property var remix: EssentialSound.remix
    readonly property bool busy: remix.busy === true
    readonly property bool ready: !busy && (remix.error ?? "") === "" && remix.resultSeconds !== undefined
    /// Apply, Render as take or Cancel was chosen.
    signal finished()

    function seconds(v) {
        return v === undefined ? "—" : qsTr("%1 s").arg(Number(v).toFixed(1))
    }
    function confidenceColor(c) {
        return c >= 0.8 ? Theme.success : c >= 0.35 ? Theme.warning : Theme.danger
    }

    spacing: Theme.space(1.5)

    BeamProgress {
        objectName: "remixProgress"
        Layout.fillWidth: true
        loader: "equalizer"
        visible: summary.busy
        active: summary.busy
        indeterminate: (summary.remix.progress ?? -1) < 0
        value: Math.max(0, summary.remix.progress ?? 0)
        label: summary.remix.phase ?? ""
    }
    Text {
        objectName: "remixError"
        Layout.fillWidth: true
        visible: text !== ""
        text: summary.remix.error ?? ""
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    GridLayout {
        Layout.fillWidth: true
        visible: summary.ready
        columns: 2
        columnSpacing: Theme.space(3)
        rowSpacing: Theme.space(0.5)
        Text {
            text: qsTr("Target")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            text: summary.seconds(summary.remix.targetSeconds)
            color: Theme.textSecondary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
        }
        Text {
            text: qsTr("Result")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            objectName: "remixResult"
            //: The remix's length and how far it is from the target, e.g. "47.8 s (−0.2 s)"
            text: qsTr("%1 (%2)").arg(summary.seconds(summary.remix.resultSeconds))
                                 .arg((summary.remix.errorSeconds ?? 0) >= 0 ? "+" + Number(summary.remix.errorSeconds ?? 0).toFixed(1) + " s"
                                                                             : "−" + Number(-(summary.remix.errorSeconds ?? 0)).toFixed(1) + " s")
            color: Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
        }
    }
    Text {
        Layout.fillWidth: true
        visible: summary.ready
        text: (summary.remix.joins ?? []).length === 0 ? qsTr("No joins: the music already fits")
                                                         : qsTr("%n join(s)", "", (summary.remix.joins ?? []).length)
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.labelTracking
    }
    Repeater {
        model: summary.ready ? summary.remix.joins : []
        delegate: RowLayout {
            id: joinRow
            required property var modelData
            required property int index
            objectName: "remixJoin_" + index
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Rectangle {
                implicitWidth: Theme.dotSize
                implicitHeight: Theme.dotSize
                radius: Theme.dotSize / 2
                color: summary.confidenceColor(joinRow.modelData.confidence)
            }
            Text {
                text: joinRow.modelData.timecode
                color: Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
            }
            Text {
                Layout.fillWidth: true
                text: Math.round(joinRow.modelData.confidence * 100) + " %"
                      + (joinRow.modelData.onBar ? "" : " · " + qsTr("off the bar"))
                elide: Text.ElideRight
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
    }
    Repeater {
        model: summary.remix.warnings ?? []
        delegate: Text {
            required property string modelData
            Layout.fillWidth: true
            text: modelData
            wrapMode: Text.Wrap
            color: Theme.warning
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(1.5)
        IconButton {
            id: moreButton
            objectName: "remixMore"
            compact: true
            iconName: "ellipsis"
            tooltip: qsTr("More remix options")
            onClicked: moreMenu.popup(moreButton, 0, moreButton.height + Theme.space(1))
            ContextMenu {
                id: moreMenu
                ContextMenuItem {
                    objectName: "remixFitVideo"
                    text: qsTr("Fit to video")
                    iconName: "film"
                    onTriggered: EssentialSound.previewFit(summary.remix.clip ?? "")
                }
                ContextMenuItem {
                    objectName: "remixRenderTake"
                    text: qsTr("Render as one take")
                    iconName: "download"
                    enabled: summary.ready
                    onTriggered: {
                        EssentialSound.renderTake()
                        summary.finished()
                    }
                }
            }
        }
        Item { Layout.fillWidth: true }
        PillButton {
            objectName: "remixCancel"
            compact: true
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: {
                EssentialSound.cancel()
                summary.finished()
            }
        }
        PillButton {
            objectName: "remixApply"
            compact: true
            variant: "primary"
            text: qsTr("Apply")
            enabled: summary.ready
            tooltip: qsTr("Cut the music into its pieces, with a short crossfade at each join")
            onClicked: {
                EssentialSound.apply()
                summary.finished()
            }
        }
    }
}
