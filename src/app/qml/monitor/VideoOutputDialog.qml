pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Video Output settings (0.7, card 110; Program Monitor > Video Output Settings...; bridge Monitors, VideoOutput.h):
// the program on another display, Premiere's Mercury Transmit to a display - its own full-screen window on the display you
// pick, fed like the Program Monitor and following its transport. Here: whether the output is on, which display (found again
// when it is plugged back in; the output is off while it is gone), how the picture sits (Fit letterboxes it, Fill crops it
// to cover the display, 1:1 shows the frame's own pixels), the title and action safe lines, the sync offset (how late the
// display shows a picture, so the program is played that much ahead of it), and - on Windows - Match Refresh Rate, which
// sets the display to a refresh rate the sequence's frame rate divides evenly while the output shows, and puts it back.
// A line says how the sequence's rate sits on the display's (24 fps on a 60 Hz display repeats frames unevenly). Video cards
// (Blackmagic DeckLink, AJA, NDI) need their maker's SDK and are not supported.
WabcutDialog {
    id: dialog
    objectName: "videoOutputDialog"

    readonly property var plan: Monitors.outputPlan
    readonly property bool active: plan.state === "active"

    title: qsTr("Video Output")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))

    /// How the sequence's rate sits on the display's, in words ("" while there is no display or no rate).
    function cadenceText(): string {
        const p = dialog.plan
        const rate = Number(p.rate ?? 0)
        const hz = Number(p.refreshHz ?? 0)
        if (hz <= 0 || rate <= 0)
            return ""
        const sequence = qsTr("%1 fps on a %2 Hz display").arg(rate.toFixed(rate % 1 === 0 ? 0 : 3)).arg(hz.toFixed(hz % 1 === 0 ? 0 : 2))
        switch (p.cadence) {
        case "even":
            return qsTr("%1: every frame is shown for %n refresh(es), evenly.", "", p.multiple).arg(sequence)
        case "nearEven":
            return qsTr("%1: nearly even; a frame repeats now and then.").arg(sequence)
        case "pulldown":
            return qsTr("%1: frames alternate between two lengths (pulldown), which shows as slight judder.").arg(sequence)
        case "uneven":
            return qsTr("%1: frames are shown for unequal times, which shows as judder.").arg(sequence)
        }
        return ""
    }
    /// What the output is doing, in words.
    function stateText(): string {
        switch (dialog.plan.state) {
        case "active":
            return qsTr("On: %1").arg(dialog.plan.displayLabel)
        case "missing":
            return qsTr("The chosen display is not connected; the output comes back when it is")
        case "noDisplay":
            return qsTr("Choose a display")
        }
        return qsTr("Off")
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.space(3)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(3)
            Toggle {
                objectName: "videoOutputEnable"
                text: qsTr("Enable video output")
                checked: Monitors.transmit
                onToggled: Monitors.transmit = checked
            }
            StatusPill {
                objectName: "videoOutputState"
                Layout.fillWidth: false
                label: dialog.stateText()
                dotColor: dialog.active ? Theme.success : dialog.plan.state === "off" ? Theme.textTertiary : Theme.warning
            }
        }

        // The display.
        Text {
            text: qsTr("Display")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        Flow {
            objectName: "videoOutputDisplays"
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Repeater {
                model: Monitors.screens
                delegate: PillButton {
                    id: displayButton
                    required property var modelData
                    objectName: "videoOutputDisplay_" + modelData.index
                    compact: true
                    variant: Monitors.transmitDisplayKey === modelData.key ? "primary" : "secondary"
                    text: qsTr("Display %1: %2 × %3%4%5").arg(modelData.index + 1).arg(modelData.width).arg(modelData.height)
                              .arg(modelData.refreshHz > 0 ? qsTr(", %1 Hz").arg(Math.round(modelData.refreshHz * 100) / 100) : "")
                              .arg(modelData.primary ? qsTr(" (main)") : "")
                    tooltip: modelData.label
                    Accessible.role: Accessible.RadioButton
                    Accessible.checked: Monitors.transmitDisplayKey === modelData.key
                    onClicked: Monitors.transmitDisplayKey = modelData.key
                }
            }
        }

        // The picture in the display.
        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Theme.space(3)
            rowSpacing: Theme.space(3)

            Text {
                text: qsTr("Picture")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            PillTabs {
                objectName: "videoOutputScaling"
                Accessible.name: qsTr("How the picture sits in the display")
                compact: true
                model: [qsTr("Fit"), qsTr("Fill"), qsTr("1:1")]
                currentIndex: Monitors.outputScaling === "fill" ? 1 : Monitors.outputScaling === "native" ? 2 : 0
                onActivated: index => Monitors.outputScaling = index === 1 ? "fill" : index === 2 ? "native" : "fit"
            }
            Text {
                text: qsTr("Safe margins")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            Toggle {
                objectName: "videoOutputSafeMargins"
                text: qsTr("Title and action safe lines")
                checked: Monitors.outputSafeMargins
                onToggled: Monitors.outputSafeMargins = checked
            }
            Text {
                text: qsTr("Sync offset")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            RowLayout {
                spacing: Theme.space(2)
                NumberField {
                    objectName: "videoOutputSyncOffset"
                    Layout.preferredWidth: Theme.space(32)
                    from: -500
                    to: 500
                    decimals: 0
                    stepSize: 10
                    value: Monitors.outputSyncOffsetMs
                    onValueModified: v => Monitors.outputSyncOffsetMs = Math.round(v)
                    Accessible.name: qsTr("Sync offset in milliseconds")
                }
                Text {
                    text: qsTr("ms: how late the display shows a picture")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }
            Text {
                text: qsTr("Refresh rate")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            Toggle {
                objectName: "videoOutputMatchRefresh"
                enabled: dialog.plan.canSwitch === true
                text: qsTr("Match the sequence's frame rate")
                checked: Monitors.outputMatchRefreshRate
                onToggled: Monitors.outputMatchRefreshRate = checked
            }
            Text {
                text: qsTr("In the background")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }
            Toggle {
                objectName: "videoOutputBackground"
                text: qsTr("Turn off while Wabcut is in the background")
                checked: Monitors.disableTransmitInBackground
                onToggled: Monitors.disableTransmitInBackground = checked
            }
        }

        // How the sequence's rate sits on the display's, and what Match Refresh Rate does or did.
        Text {
            objectName: "videoOutputCadence"
            Layout.fillWidth: true
            visible: text !== ""
            text: dialog.cadenceText()
            wrapMode: Text.Wrap
            color: dialog.plan.cadence === "pulldown" || dialog.plan.cadence === "uneven" ? Theme.warningText : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            objectName: "videoOutputSwitch"
            Layout.fillWidth: true
            readonly property var mode: dialog.plan.switchTo
            visible: Monitors.outputMatchRefreshRate && (dialog.plan.switched === true || mode !== undefined)
            text: dialog.plan.switched === true
                  ? qsTr("The display runs at a refresh rate that suits the sequence while the output shows; it goes back afterwards.")
                  : mode !== undefined ? qsTr("The output will set the display to %1 Hz while it shows.").arg(Math.round(mode.refreshHz * 100) / 100) : ""
            wrapMode: Text.Wrap
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Video cards (Blackmagic DeckLink, AJA, NDI) need their maker's SDK and are not supported yet: send the picture to a display, or to an SDI or HDMI converter that is one.")
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    footerData: [
        PillButton {
            objectName: "videoOutputClose"
            variant: "primary"
            text: qsTr("Done")
            onClicked: dialog.close()
        }
    ]
}
