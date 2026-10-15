import QtQuick
import QtQuick.Layouts

// ● SCOPES (Color workspace, left slot, I.5, docs/color.md "Scopes"): waveform (luma, RGB parade or YCbCr
// parade, IRE / nits graticule), vectorscope (75 % / 100 % targets, skin-tone line, zoom 1x / 2x / 5x) and the RGB +
// luma histogram of the program as shown. The program monitor computes them on the GPU about 30 times a second while
// this panel is visible (Scopes.active), and for a paused picture too.
Panel {
    id: root
    objectName: "scopesPanel"

    title: qsTr("Scopes")

    Binding {
        target: Scopes
        property: "active"
        value: root.visible && root.opacity > 0.5
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)

        PillTabs {
            objectName: "waveformTabs"
            Layout.fillWidth: true
            compact: true
            model: [qsTr("Luma"), qsTr("RGB Parade"), qsTr("YCbCr")]
            currentIndex: Scopes.waveformMode
            onActivated: index => Scopes.waveformMode = index
        }

        ScopeView {
            objectName: "waveformView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 120
            kind: Scopes.waveformMode === 1 ? "parade" : Scopes.waveformMode === 2 ? "ycbcr" : "waveform"
            generation: Scopes.generation
            background: Theme.monitorSurround
            gridColor: Theme.guide
            labelColor: Theme.scrimTextMuted
            skinColor: Theme.warning
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                Layout.fillWidth: true
                text: qsTr("Vectorscope")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                font.letterSpacing: Theme.labelTracking
            }
            PillTabs {
                objectName: "zoomTabs"
                Layout.preferredWidth: Theme.space(32)
                compact: true
                model: ["1x", "2x", "5x"]
                currentIndex: Scopes.vectorZoom === 5 ? 2 : Scopes.vectorZoom === 2 ? 1 : 0
                onActivated: index => Scopes.vectorZoom = [1, 2, 5][index]
            }
        }

        ScopeView {
            objectName: "vectorscopeView"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(width, root.height * 0.3)
            kind: "vectorscope"
            generation: Scopes.generation
            background: Theme.monitorSurround
            gridColor: Theme.guide
            labelColor: Theme.scrimTextMuted
            skinColor: Theme.warning
        }

        ScopeView {
            objectName: "histogramView"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(56, root.height * 0.11)
            kind: "histogram"
            generation: Scopes.generation
            background: Theme.monitorSurround
            gridColor: Theme.guide
            labelColor: Theme.scrimTextMuted
            skinColor: Theme.warning
        }

        Text {
            objectName: "scopeStats"
            Layout.fillWidth: true
            readonly property var s: Scopes.stats
            text: s.pixels === undefined ? qsTr("Play or park on a frame")
                  : qsTr("%1 · median %2 IRE (1 %: %3, 99 %: %4) · clipped %5 % · %6 · %7 Hz")
                        .arg(s.exposure).arg(s.median).arg(s.p1).arg(s.p99).arg(s.clipped).arg(s.cast)
                        .arg(Math.round(Scopes.rate))
            color: s.exposure === "ok" || s.exposure === undefined ? Theme.textTertiary : Theme.warning
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }
    }
}
