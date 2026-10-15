pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The first-run performance check (I.7, docs/release.md "First-run performance check"; PerformanceCheck): optional and
// skippable. It offers about ten seconds of measuring (decoding H.264, HEVC and ProRes in hardware and software, the
// export encoders, the GPU and the audio output), then shows a short summary and the media-readiness settings it
// recommends, applied only on "Use these settings". Main.qml offers it once after Home and the welcome; Settings >
// Media & cache runs it again (PerformanceCheck.openCheck()).
WabcutDialog {
    id: dialog
    objectName: "firstRunCheckDialog"

    // "offer", "running" or "result".
    readonly property string stage: PerformanceCheck.running ? "running"
                                    : showResult && PerformanceCheck.hasReport ? "result" : "offer"
    property bool showResult: false

    title: qsTr("Performance check")

    function offer() {
        showResult = false
        open()
    }

    Connections {
        target: PerformanceCheck
        function onOpenRequested() {
            dialog.showResult = PerformanceCheck.hasReport
            dialog.open()
        }
        function onFinished() {
            dialog.showResult = true
        }
    }

    component Body: Text {
        Layout.fillWidth: true
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    // ---- Offer ----
    Body {
        visible: dialog.stage === "offer"
        text: qsTr("Wabcut can measure what this computer plays and exports smoothly - video decoding, the GPU, export encoders and the audio output - and suggest proxy and preview settings. It takes about ten seconds; nothing leaves this computer.")
    }
    Note {
        visible: dialog.stage === "offer"
        text: qsTr("You can run it later from Settings > Media & cache.")
    }

    // ---- Running ----
    Body {
        objectName: "performanceStep"
        visible: dialog.stage === "running"
        //: %1 is the step being measured, e.g. "Decoding HEVC (hardware)"
        text: qsTr("Measuring: %1").arg(PerformanceCheck.step)
    }
    BeamProgress {
        Layout.fillWidth: true
        visible: dialog.stage === "running"
        value: PerformanceCheck.progress
        active: PerformanceCheck.running
        label: qsTr("Performance check")
        loader: "build"
    }

    // ---- Result: the summary (one row per measurement) and the recommendation ----
    Rectangle {
        objectName: "performanceSummary"
        Layout.fillWidth: true
        visible: dialog.stage === "result"
        implicitHeight: summaryRows.implicitHeight + Theme.space(4)
        radius: Theme.radiusCard
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border

        ColumnLayout {
            id: summaryRows
            anchors.fill: parent
            anchors.margins: Theme.space(2)
            spacing: Theme.space(1)

            Repeater {
                model: dialog.stage === "result" ? PerformanceCheck.rows : []
                delegate: RowLayout {
                    id: resultRow
                    required property var modelData
                    Layout.fillWidth: true
                    objectName: "performanceRow_" + modelData.key
                    spacing: Theme.space(2)

                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: Theme.space(1)
                        implicitWidth: Theme.space(2)
                        implicitHeight: Theme.space(2)
                        radius: width / 2
                        color: resultRow.modelData.good ? Theme.success : Theme.warning
                    }
                    Text {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: Theme.space(32)
                        text: resultRow.modelData.label
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        text: resultRow.modelData.value
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }
    SectionHeader {
        Layout.fillWidth: true
        visible: dialog.stage === "result"
        text: qsTr("Recommended")
    }
    Body {
        objectName: "performanceRecommendation"
        visible: dialog.stage === "result"
        text: PerformanceCheck.recommendation.text ?? ""
        color: Theme.textPrimary
    }
    Repeater {
        model: dialog.stage === "result" ? PerformanceCheck.reasons : []
        delegate: Note {
            required property string modelData
            text: modelData
        }
    }
    Note {
        visible: dialog.stage === "result" && PerformanceCheck.partial
        text: qsTr("The check was stopped early: the suggestion uses what was measured.")
    }

    footerData: [
        PillButton {
            objectName: "performanceSkip"
            visible: dialog.stage === "offer"
            text: qsTr("Not now")
            onClicked: {
                PerformanceCheck.skip()
                dialog.close()
            }
        },
        PillButton {
            objectName: "performanceStart"
            visible: dialog.stage !== "running"
            variant: dialog.stage === "offer" ? "primary" : "secondary"
            text: dialog.stage === "offer" ? qsTr("Run check") : qsTr("Run again")
            onClicked: PerformanceCheck.start()
        },
        PillButton {
            objectName: "performanceCancel"
            visible: dialog.stage === "running"
            text: qsTr("Stop")
            onClicked: PerformanceCheck.cancel()
        },
        PillButton {
            objectName: "performanceKeep"
            visible: dialog.stage === "result"
            text: PerformanceCheck.applied ? qsTr("Close") : qsTr("Keep my settings")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "performanceApply"
            visible: dialog.stage === "result" && !PerformanceCheck.applied
            variant: "primary"
            text: qsTr("Use these settings")
            onClicked: {
                PerformanceCheck.apply()
                dialog.close()
            }
        }
    ]
    // Escape while it offers counts as "Not now"; while it runs it stops it.
    onClosed: {
        if (PerformanceCheck.running)
            PerformanceCheck.cancel()
        else if (stage === "offer")
            PerformanceCheck.skip()
    }
}
