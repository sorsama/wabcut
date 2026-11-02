pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The Creator autopilot's B-roll page (AutopilotSheet): Find B-roll reads the sequence's transcript line by line and
// suggests B-roll from the library (by picture with the visual search model, by words always), staged on a free video
// track and drawn on the timeline as a ghost. Each suggestion can be switched off; Apply puts the chosen ones in as one
// undo step. Lines nothing matched are listed with Generate..., which opens the Generate panel with the line as the
// prompt (the cost and consent are shown there).
ColumnLayout {
    id: page
    objectName: "autopilotBrollPage"

    signal said(string text)
    signal generateLine(int line)

    readonly property var report: Autopilot.broll
    readonly property var suggestions: report.suggestions ?? []
    readonly property var unmatched: report.unmatched ?? []
    readonly property bool applied: report.done === true
    readonly property var perMinute: [2, 4, 6]

    spacing: Theme.space(2)

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    component Small: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        elide: Text.ElideRight
    }

    function find() {
        const error = Autopilot.previewBroll({ perMinute: page.perMinute[density.currentIndex], visual: visualToggle.checked })
        if (error !== "")
            page.said(error)
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(3)
        Label { text: qsTr("At most per minute") }
        PillTabs {
            id: density
            objectName: "autopilotBrollDensity"
            Accessible.name: qsTr("B-roll clips per minute at most")
            Layout.preferredWidth: Theme.space(36)
            compact: true
            currentIndex: 1
            model: ["2", "4", "6"]
        }
        Toggle {
            id: visualToggle
            objectName: "autopilotBrollVisual"
            text: qsTr("Match by picture")
            checked: true
            tooltip: qsTr("Also match what the footage shows, with the visual search model on this computer when it is installed")
        }
        Item { Layout.fillWidth: true }
        PillButton {
            objectName: "autopilotFindBroll"
            variant: "primary"
            iconName: "sparkles"
            text: qsTr("Find B-roll")
            enabled: !Autopilot.busy
            onClicked: page.find()
        }
    }
    Text {
        objectName: "autopilotBrollSummary"
        Layout.fillWidth: true
        visible: page.report.lines !== undefined
        text: {
            const track = page.report.track ?? {}
            const where = track.label ? (track.created ? qsTr("a new track %1").arg(track.label) : track.label) : ""
            const placed = page.suggestions.filter(s => s.chosen).length
            return (page.applied ? qsTr("Added %1 B-roll clip(s) on %2").arg(placed).arg(where)
                                 : where !== "" ? qsTr("%1 of %2 suggestion(s) on %3").arg(placed).arg(page.suggestions.length).arg(where)
                                                : qsTr("%1 suggestion(s)").arg(page.suggestions.length))
                   + " · " + qsTr("%1 line(s) read, %2 library item(s) looked at").arg(page.report.lines ?? 0).arg(page.report.candidates ?? 0)
                   + (page.report.visual ? " · " + qsTr("by picture and words") : " · " + qsTr("by words"))
        }
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        elide: Text.ElideRight
    }

    ListView {
        id: list
        objectName: "autopilotBrollList"
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        spacing: Theme.space(1)
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}
        model: page.suggestions
        delegate: Rectangle {
            id: row
            required property var modelData
            required property int index
            objectName: "autopilotBroll_" + modelData.index
            width: list.width
            implicitHeight: rowLayout.implicitHeight + Theme.space(1.5)
            radius: Theme.radiusField
            color: modelData.chosen ? Theme.raised : Theme.panel
            border.width: Theme.hairline
            border.color: modelData.chosen ? Theme.border : Theme.borderStrong
            opacity: modelData.chosen ? 1 : 0.6
            RowLayout {
                id: rowLayout
                anchors.fill: parent
                anchors.margins: Theme.space(0.75)
                spacing: Theme.space(2)
                Toggle {
                    objectName: "autopilotBrollChosen_" + row.modelData.index
                    checked: row.modelData.chosen === true
                    enabled: !Autopilot.busy && !page.applied
                    Accessible.name: qsTr("Use this B-roll: %1").arg(row.modelData.mediaName)
                    onToggled: {
                        const error = Autopilot.setBrollChosen(row.modelData.index, checked)
                        if (error !== "")
                            page.said(error)
                    }
                }
                Image {
                    Layout.preferredWidth: Theme.space(16)
                    Layout.preferredHeight: Theme.space(9)
                    source: row.modelData.thumb ?? ""
                    sourceSize: Qt.size(192, 108)
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        Layout.fillWidth: true
                        text: row.modelData.text ?? ""
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        elide: Text.ElideRight
                    }
                    Small {
                        Layout.fillWidth: true
                        text: qsTr("%1 at %2 for %3 s").arg(row.modelData.mediaName).arg(row.modelData.timecode)
                              .arg(Number(row.modelData.duration).toFixed(1))
                    }
                    Small {
                        Layout.fillWidth: true
                        text: row.modelData.reason ?? ""
                    }
                }
                // How well it fits: a short meter, the number for screen readers.
                Rectangle {
                    Layout.preferredWidth: Theme.space(12)
                    Layout.preferredHeight: 4
                    radius: 2
                    color: Theme.sunken
                    Accessible.name: qsTr("Fit %1%").arg(Math.round((row.modelData.score ?? 0) * 100))
                    Rectangle {
                        width: parent.width * (row.modelData.score ?? 0)
                        height: parent.height
                        radius: 2
                        color: (row.modelData.score ?? 0) > 0.7 ? Theme.success : Theme.warning
                    }
                }
            }
        }
        EmptyState {
            anchors.centerIn: parent
            width: parent.width - Theme.space(4)
            visible: list.count === 0 && !Autopilot.busy && page.unmatched.length === 0
            illustration: "transcript"
            text: qsTr("Find B-roll reads what is said and puts matching shots from your library on a free track. Name your clips or add a Description in the Library for better matches.")
        }
    }

    // ---- Lines nothing in the library matched ----
    Text {
        Layout.fillWidth: true
        visible: page.unmatched.length > 0
        text: qsTr("Nothing in your library for these lines")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
    }
    ListView {
        id: missing
        objectName: "autopilotBrollUnmatched"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, Theme.space(28))
        visible: page.unmatched.length > 0
        clip: true
        spacing: Theme.space(0.5)
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}
        model: page.unmatched
        delegate: RowLayout {
            id: gap
            required property var modelData
            width: missing.width
            spacing: Theme.space(2)
            Small {
                text: gap.modelData.timecode
                font.family: Theme.fontMono
            }
            Small {
                Layout.fillWidth: true
                text: gap.modelData.text
                color: Theme.textSecondary
            }
            PillButton {
                objectName: "autopilotGenerate_" + gap.modelData.line
                compact: true
                iconName: "sparkles"
                text: page.applied || Autopilot.stagedKind !== "broll" ? qsTr("Generate…") : qsTr("Apply and generate…")
                enabled: !Autopilot.busy && Preferences.aiEnabled && !AiController.offlineMode
                tooltip: qsTr("Opens Generate with this line as the prompt, at its place on the timeline; it shows the cost and asks first")
                onClicked: page.generateLine(gap.modelData.line)
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        Text {
            Layout.fillWidth: true
            text: qsTr("Undo takes the whole B-roll out again in one step.")
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            elide: Text.ElideRight
        }
        PillButton {
            objectName: "autopilotApplyBroll"
            variant: "primary"
            text: qsTr("Apply B-roll")
            enabled: Autopilot.stagedKind === "broll" && !Autopilot.busy
            onClicked: {
                const error = Autopilot.applyBroll()
                if (error !== "")
                    page.said(error)
            }
        }
    }
}
