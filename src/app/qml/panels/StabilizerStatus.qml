pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// P.2 (docs/motion.md "Warp Stabilizer"): the analysis state and the Analyze / Cancel button in the Effect Controls
// header of a Warp Stabilizer or Rolling Shutter Repair (the `Stabilizer` singleton). Invisible for other effects.
RowLayout {
    id: root

    /// The effect's section key ("e12").
    required property string section
    readonly property var info: ProjectController.revision >= 0 && root.section !== "" ? Stabilizer.effectInfo(root.section) : ({})
    readonly property bool analyzing: Stabilizer.section === root.section && Stabilizer.busy

    visible: root.info.stabilizer === true
    spacing: Theme.space(1)

    Text {
        objectName: "stabilizerStatus"
        Layout.maximumWidth: Theme.space(40)
        elide: Text.ElideRight
        text: root.analyzing ? qsTr("Analyzing %1%").arg(Math.round(Stabilizer.progress * 100))
            : root.info.status === "analyzed" ? qsTr("Analysis complete")
            : root.info.status === "failed" ? qsTr("Analysis failed")
            : qsTr("Not analyzed")
        color: root.analyzing ? Theme.accentStroke : root.info.status === "analyzed" ? Theme.textSecondary : Theme.textTertiary
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
            : qsTr("Track the camera motion in the background, then smooth it with these settings")
        onClicked: root.analyzing ? Stabilizer.cancel() : Stabilizer.analyze(root.section)
    }
}
