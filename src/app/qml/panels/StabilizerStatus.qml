pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// P.2 (docs/motion.md "Warp Stabilizer"): the analysis state and the Analyze / Cancel button in the Effect Controls
// header of a Warp Stabilizer or Rolling Shutter Repair (the `Stabilizer` singleton). Invisible for other effects.
// 0.7 (card #128): "Analyze All" next to it (when the sequence has more than one) re-analyses every stabilizer that is
// not analysed or out of date in one job and one undo step; the state reads "Out of date" for an analysis made for other
// frames or settings.
RowLayout {
    id: root

    /// The effect's section key ("e12").
    required property string section
    readonly property var info: ProjectController.revision >= 0 && root.section !== "" ? Stabilizer.effectInfo(root.section) : ({})
    readonly property var overview: ProjectController.revision >= 0 && root.info.stabilizer === true ? Stabilizer.overview() : ({})
    readonly property bool analyzing: Stabilizer.section === root.section && Stabilizer.busy
    /// Analyze All is running (it belongs to the whole sequence: every header shows it).
    readonly property bool allBusy: Stabilizer.busy && Stabilizer.section === "*"
    readonly property int pending: root.overview.pending ?? 0

    visible: root.info.stabilizer === true
    spacing: Theme.space(1)

    Text {
        objectName: "stabilizerStatus"
        Layout.maximumWidth: Theme.space(40)
        elide: Text.ElideRight
        text: root.analyzing ? qsTr("Analyzing %1%").arg(Math.round(Stabilizer.progress * 100))
            : root.allBusy ? qsTr("Analyzing all %1%").arg(Math.round(Stabilizer.progress * 100))
            : root.info.status === "analyzed" ? (root.info.outOfDate === true ? qsTr("Out of date") : qsTr("Analysis complete"))
            : root.info.status === "failed" ? qsTr("Analysis failed")
            : qsTr("Not analyzed")
        color: root.analyzing || root.allBusy ? Theme.accentStroke
            : root.info.outOfDate === true ? Theme.warning
            : root.info.status === "analyzed" ? Theme.textSecondary : Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    PillButton {
        objectName: "stabilizerAnalyze"
        compact: true
        variant: root.analyzing ? "ghost" : "secondary"
        enabled: root.analyzing || !Stabilizer.busy
        text: root.analyzing ? qsTr("Cancel") : root.info.status === "analyzed" ? qsTr("Reanalyze") : qsTr("Analyze")
        tooltip: root.analyzing ? Stabilizer.message
            : root.info.kind === "shutter" ? qsTr("Measure the picture's motion in the background (Rolling Shutter Repair)")
            : root.info.gyro === true ? qsTr("Read the camera's gyro, sync it with the picture in the background, then smooth it with these settings (Smoothness follows its keyframes)")
            : root.info.smoothnessKeyed === true ? qsTr("Track the camera motion in the background, then smooth it with these settings: Smoothness follows its keyframes (0 leaves the shot as it was shot)")
            : qsTr("Track the camera motion in the background, then smooth it with these settings")
        onClicked: root.analyzing ? Stabilizer.cancel() : Stabilizer.analyze(root.section)
    }

    PillButton {
        objectName: "stabilizerAnalyzeAll"
        compact: true
        variant: "ghost"
        visible: (root.overview.count ?? 0) > 1 || root.allBusy
        enabled: root.allBusy || !Stabilizer.busy
        text: root.allBusy ? qsTr("Cancel all")
            : root.pending > 0 ? qsTr("Analyze All (%1)").arg(root.pending) : qsTr("Reanalyze All")
        tooltip: root.allBusy ? Stabilizer.message
            : root.pending > 0 ? qsTr("Analyze every Warp Stabilizer and Rolling Shutter Repair in this sequence that is not analyzed or out of date (one undo step)")
            : qsTr("Every stabilizer is analyzed: analyze them all again")
        onClicked: root.allBusy ? Stabilizer.cancel() : Stabilizer.analyzeAll(root.pending === 0)
    }
}
