pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The Creator autopilot's pacing page (AutopilotSheet): pick a reference video from the project (one whose pace you
// like) and Analyse: its cuts come from shot detection (cached per file). Both rhythms show as cuts per minute, the
// median shot and the shot length over the running time (bars, eight parts); the suggested cuts are listed (each can
// be switched off) and drawn on the timeline as markers. Add as markers or Cut there applies them as one undo step.
// Shots already shorter than the reference's are left alone: pacing match never removes footage.
ColumnLayout {
    id: page
    objectName: "autopilotPacingPage"

    signal said(string text)

    readonly property var report: Autopilot.pacing
    readonly property var suggestions: report.suggestions ?? []
    readonly property bool ready: report.reference !== undefined
    readonly property bool applied: report.done === true
    property var references: []
    /// The chosen reference video's key (Autopilot.referenceVideos()).
    property string referenceKey: ""
    readonly property var referenceChoice: page.references.find(r => r.key === page.referenceKey) ?? null

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
    /// One rhythm as eight bars: each part's median shot length (longer = taller), on the scale both rows share.
    component RhythmRow: RowLayout {
        id: rhythm
        property string name: ""
        property var rhythmData: ({})
        property real longest: 1
        property color barColor: Theme.accentFill
        spacing: Theme.space(2)
        Small {
            Layout.preferredWidth: Theme.space(26)
            text: rhythm.name
            color: Theme.textSecondary
        }
        Row {
            Layout.preferredHeight: Theme.space(8)
            spacing: Theme.space(0.5)
            Repeater {
                model: rhythm.rhythmData.curve ?? []
                delegate: Item {
                    id: bar
                    required property var modelData
                    width: Theme.space(5)
                    height: Theme.space(8)
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width
                        // A log scale, so 2 s shots still show next to a minute-long take.
                        height: Math.max(2, parent.height * Math.min(1, Math.log(1 + Number(bar.modelData)) / Math.log(1 + rhythm.longest)))
                        radius: 1.5
                        color: rhythm.barColor
                    }
                }
            }
        }
        Small {
            Layout.fillWidth: true
            text: qsTr("%1 cuts/min · median shot %2 s · %3 cuts").arg(Number(rhythm.rhythmData.cutsPerMinute ?? 0).toFixed(1))
                  .arg(Number(rhythm.rhythmData.medianShot ?? 0).toFixed(1)).arg(rhythm.rhythmData.cuts ?? 0)
        }
    }

    function refresh() {
        page.references = Autopilot.referenceVideos()
        if (!page.references.some(r => r.key === page.referenceKey))
            page.referenceKey = page.references.length > 0 ? page.references[0].key : ""
    }
    function analyse() {
        const choice = page.referenceChoice
        if (!choice) {
            page.said(qsTr("Import a video whose pace you like first: it is the reference"))
            return
        }
        const error = Autopilot.analysePacing(choice.key, { strength: strength.currentIndex === 0 ? 1.0 : 0.5 })
        if (error !== "")
            page.said(error)
    }
    function applyAs(mode: string) {
        const error = Autopilot.applyPacing(mode)
        if (error !== "")
            page.said(error)
    }

    Component.onCompleted: page.refresh()
    onVisibleChanged: {
        if (visible)
            page.refresh()
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(3)
        Label { text: qsTr("Reference") }
        PillButton {
            id: reference
            objectName: "autopilotPacingReference"
            Layout.preferredWidth: Theme.space(70)
            iconName: "chevron-down"
            text: page.referenceChoice ? page.referenceChoice.label : qsTr("No video in the project")
            enabled: page.references.length > 0
            Accessible.name: qsTr("Reference video: %1").arg(text)
            onClicked: referenceMenu.popup(reference, 0, reference.height + Theme.space(1))
        }
        OptionsMenu {
            id: referenceMenu
            objectName: "autopilotPacingReferenceMenu"
            options: page.references.map(r => ({ id: r.key, name: r.label }))
            current: page.referenceKey
            onChosen: id => page.referenceKey = id
        }
        Label { text: qsTr("Match") }
        PillTabs {
            id: strength
            objectName: "autopilotPacingStrength"
            Accessible.name: qsTr("How closely to match the reference")
            Layout.preferredWidth: Theme.space(40)
            compact: true
            model: [qsTr("Its pace"), qsTr("Halfway")]
        }
        Item { Layout.fillWidth: true }
        PillButton {
            objectName: "autopilotAnalysePacing"
            variant: "primary"
            iconName: "gauge"
            text: qsTr("Analyse")
            enabled: !Autopilot.busy && page.references.length > 0
            onClicked: page.analyse()
        }
    }

    ColumnLayout {
        id: rhythms
        objectName: "autopilotPacingRhythms"
        Layout.fillWidth: true
        visible: page.ready
        spacing: Theme.space(1)
        readonly property real longest: Math.max(1, ...(page.report.reference?.curve ?? [1]), ...(page.report.edit?.curve ?? [1]))
        RhythmRow {
            Layout.fillWidth: true
            name: page.report.reference?.name ?? qsTr("Reference")
            rhythmData: page.report.reference ?? ({})
            longest: rhythms.longest
            barColor: Theme.textTertiary
        }
        RhythmRow {
            Layout.fillWidth: true
            name: qsTr("Your edit")
            rhythmData: page.report.edit ?? ({})
            longest: rhythms.longest
        }
        RhythmRow {
            Layout.fillWidth: true
            name: qsTr("With the cuts")
            rhythmData: page.report.after ?? ({})
            longest: rhythms.longest
            barColor: Theme.accentStroke
        }
    }

    ListView {
        id: list
        objectName: "autopilotPacingList"
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        spacing: Theme.space(0.5)
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}
        model: page.suggestions
        delegate: Rectangle {
            id: row
            required property var modelData
            objectName: "autopilotPacing_" + modelData.index
            width: list.width
            implicitHeight: rowLayout.implicitHeight + Theme.space(1)
            radius: Theme.radiusField
            color: modelData.chosen ? Theme.raised : Theme.panel
            opacity: modelData.chosen ? 1 : 0.6
            RowLayout {
                id: rowLayout
                anchors.fill: parent
                anchors.margins: Theme.space(0.5)
                spacing: Theme.space(2)
                Toggle {
                    objectName: "autopilotPacingChosen_" + row.modelData.index
                    checked: row.modelData.chosen === true
                    enabled: !Autopilot.busy && !page.applied
                    Accessible.name: qsTr("Cut at %1").arg(row.modelData.timecode)
                    onToggled: {
                        const error = Autopilot.setPacingChosen(row.modelData.index, checked)
                        if (error !== "")
                            page.said(error)
                    }
                }
                Small {
                    text: row.modelData.timecode
                    color: Theme.textPrimary
                    font.family: Theme.fontMono
                }
                Small {
                    Layout.fillWidth: true
                    text: row.modelData.reason
                }
            }
        }
        EmptyState {
            anchors.centerIn: parent
            width: parent.width - Theme.space(4)
            visible: list.count === 0 && !Autopilot.busy
            illustration: "timeline"
            text: page.ready ? qsTr("No cuts to suggest: your edit already cuts about as often as the reference.")
                             : qsTr("Pick a video whose pace you like and Analyse: Wabcut measures how often it cuts and suggests cuts so your edit feels as quick.")
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        Small {
            Layout.fillWidth: true
            text: (page.report.fasterShots ?? 0) > 0
                  ? qsTr("%1 shot(s) are already quicker than the reference and stay as they are.").arg(page.report.fasterShots)
                  : qsTr("Cuts land at sentence ends, pauses or beats; never inside a word.")
        }
        PillButton {
            objectName: "autopilotPacingMarkers"
            text: qsTr("Add as markers")
            enabled: Autopilot.stagedKind === "pacing" && !Autopilot.busy
            tooltip: qsTr("A marker at each suggestion, to cut by hand or drop B-roll on")
            onClicked: page.applyAs("markers")
        }
        PillButton {
            objectName: "autopilotPacingCuts"
            variant: "primary"
            text: qsTr("Cut there")
            enabled: Autopilot.stagedKind === "pacing" && !Autopilot.busy
            tooltip: qsTr("Add Edit on all tracks at each suggestion")
            onClicked: page.applyAs("cuts")
        }
    }
}
