pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// One export job: the delegate of ExportController.jobs (its roles are the required properties). A pill
// whose background fills with the progress in flat accent (spring; instant under reduce-motion), the echo mark's
// scan loader while it renders (a status dot otherwise), the file name and the live figures in monospace (percent ·
// fps · time left). Cancel (×) while queued or rendering; "Show" (in Explorer / Finder) when done. A failed job
// shows its plain-language error under the pill.
// 0.4: a done job also has "Open" (the file in its default app; a double-click on the pill does the same) before "Show";
// `notice` (set by the owner, e.g. "The file was moved or deleted.") shows in the danger colour under the pill.
Item {
    id: root

    required property string jobId
    required property string title
    required property string presetName
    required property string jobState // "held" | "queued" | "running" | "paused" | "done" | "failed" | "cancelled"
    required property string phaseText
    required property real progress
    required property string fpsText
    required property string etaText
    required property string encoder
    required property string detail
    required property bool finished
    /// 0.4: a problem to show under the pill instead of its usual line ("" for none).
    property string notice: ""

    signal cancelRequested()
    signal revealRequested()
    // 0.2 (Media Encoder's queue): pause a running or queued job, resume it, start a held one.
    signal pauseRequested()
    signal resumeRequested()
    signal startRequested()
    /// 0.4: open the finished file in its default app.
    signal openRequested()

    readonly property bool running: jobState === "running"
    readonly property color stateColor: jobState === "done" ? Theme.success
                                      : jobState === "failed" ? Theme.danger
                                      : running ? Theme.accentStroke
                                      : Theme.textTertiary

    implicitWidth: Theme.space(80)
    implicitHeight: column.implicitHeight

    ColumnLayout {
        id: column
        width: root.width
        spacing: Theme.space(1)

        Rectangle {
            id: pill
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlHeight
            radius: height / 2
            color: Theme.raised
            clip: true

            Accessible.role: Accessible.ProgressBar
            Accessible.name: qsTr("Export %1").arg(root.title)
            Accessible.description: root.phaseText

            // 0.4: a double-click on a finished export opens it.
            TapHandler {
                objectName: "exportJobOpenTap"
                enabled: root.jobState === "done"
                onDoubleTapped: root.openRequested()
            }

            Rectangle {
                id: fill
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: Math.max(root.progress > 0 ? pill.height : 0, pill.width * root.progress)
                radius: pill.radius
                // Flat accent (design language v3), light enough that the figures over it keep their contrast.
                color: root.jobState === "failed" ? Qt.alpha(Theme.danger, 0.22)
                     : root.jobState === "done" ? Qt.alpha(Theme.success, 0.22)
                     : Qt.alpha(Theme.accentFill, 0.38)
                Behavior on width {
                    enabled: !Theme.reduceMotion
                    SpringAnimation {
                        spring: Theme.springStiffness
                        damping: Theme.springDamping
                        mass: Theme.springMass
                        epsilon: Theme.springEpsilon
                    }
                }
                Behavior on color { ColorAnimation { duration: Theme.durState } }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.space(3)
                // A trailing button (cancel / show) brings its own padding; plain text needs the pill's.
                anchors.rightMargin: root.finished && root.jobState !== "done" ? Theme.space(4) : Theme.space(1)
                spacing: Theme.space(2)

                Rectangle {
                    visible: !root.running
                    Layout.preferredWidth: Theme.dotSize
                    Layout.preferredHeight: Theme.dotSize
                    radius: width / 2
                    color: root.stateColor
                    Behavior on color { ColorAnimation { duration: Theme.durState } }
                }
                EchoLoader {
                    objectName: "exportJobLoader"
                    visible: root.running
                    Layout.preferredWidth: Theme.space(5)
                    Layout.preferredHeight: Theme.space(5)
                    size: Theme.space(5)
                    variant: "scan"
                }

                Text {
                    Layout.fillWidth: true
                    text: root.title
                    elide: Text.ElideMiddle
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Theme.weightLabel
                }


                Text {
                    text: root.running && root.phaseText !== "" && root.fpsText !== ""
                          ? qsTr("%1% · %2 · %3").arg(Math.round(root.progress * 100)).arg(root.fpsText).arg(root.etaText)
                          : root.jobState === "done" ? root.detail
                          : root.phaseText
                    color: root.jobState === "failed" ? Theme.danger : Theme.textSecondary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                }

                IconButton {
                    objectName: "exportJobPause"
                    visible: !root.finished
                    compact: true
                    iconName: root.jobState === "paused" || root.jobState === "held" ? "play" : "pause"
                    tooltip: root.jobState === "held" ? qsTr("Start") : root.jobState === "paused" ? qsTr("Resume") : qsTr("Pause")
                    onClicked: {
                        if (root.jobState === "held")
                            root.startRequested()
                        else if (root.jobState === "paused")
                            root.resumeRequested()
                        else
                            root.pauseRequested()
                    }
                }

                IconButton {
                    objectName: "exportJobCancel"
                    visible: !root.finished
                    compact: true
                    iconName: "x"
                    tooltip: qsTr("Cancel export")
                    onClicked: root.cancelRequested()
                }

                // 0.4: the finished file in its default app.
                PillButton {
                    objectName: "exportJobOpen"
                    visible: root.jobState === "done"
                    compact: true
                    variant: "ghost"
                    text: qsTr("Open")
                    tooltip: qsTr("Open %1").arg(root.title)
                    onClicked: root.openRequested()
                }
                PillButton {
                    objectName: "exportJobShow"
                    visible: root.jobState === "done"
                    compact: true
                    variant: "ghost"
                    text: qsTr("Show")
                    tooltip: qsTr("Show in %1").arg(ExportController.fileManagerName)
                    onClicked: root.revealRequested()
                }
            }
        }

        Text {
            objectName: "exportJobDetail"
            Layout.fillWidth: true
            Layout.leftMargin: Theme.space(3)
            text: root.notice !== "" ? root.notice
                  : root.jobState === "failed" || root.jobState === "cancelled" ? root.detail
                  : root.encoder !== "" ? qsTr("%1 · %2").arg(root.presetName).arg(root.encoder)
                  : root.presetName
            wrapMode: Text.WordWrap
            color: root.notice !== "" || root.jobState === "failed" ? Theme.danger : Theme.textTertiary
            font.family: root.notice !== "" || root.jobState === "failed" ? Theme.fontSans : Theme.fontMono
            font.pixelSize: Theme.textSm
        }
    }
}
