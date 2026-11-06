pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The Color panel's "Color Chart" section (0.7, Match to a colour chart, docs/color.md). Put the playhead on a frame
// that shows an X-Rite ColorChecker (24 patches) and Find chart: the chart is looked for in the clip's picture (before
// its grade) and drawn on the program monitor with four corners to drag when it is not found or not quite right
// (ColorChartOverlay). The swatches show every patch three ways - the chart's colour (top), now (bottom left) and
// matched (bottom right) - with the mean and worst delta E 2000 before and after; Match writes the fit into the clip's
// grade (temperature, tint, exposure, contrast, saturation, RGB and hue curves: ordinary, editable settings) in one undo
// step, and can give the same correction to the other selected clips shot in the same light.
GradeSection {
    id: root
    objectName: "colorChartSection"

    title: qsTr("Color Chart")

    readonly property var chart: Color.chart
    readonly property bool chartActive: chart.active ?? false
    readonly property var patches: chart.patches ?? []
    readonly property bool hasPatches: patches.length === 24
    property bool alsoSelected: false
    /// The swatch the pointer is over (-1 = none): the line under the grid names it.
    property int hovered: -1
    readonly property var emptyPatch: ({used: false, reference: Qt.rgba(0, 0, 0, 1), before: Qt.rgba(0, 0, 0, 1), after: Qt.rgba(0, 0, 0, 1), name: "", deltaBefore: 0, deltaAfter: 0})

    function format(value: real): string {
        return value.toFixed(1)
    }

    Text {
        Layout.fillWidth: true
        visible: !root.chartActive
        text: qsTr("Match the clip to an X-Rite ColorChecker (24 patches) that is in the picture. Put the playhead on a frame that shows the chart, then find it.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.WordWrap
    }

    Flow {
        Layout.fillWidth: true
        spacing: Theme.space(2)

        PillButton {
            objectName: "chartFindButton"
            text: root.chartActive ? qsTr("Find again") : qsTr("Find chart")
            iconName: "crosshair"
            compact: true
            enabled: Color.clip !== "" && !Color.busy
            tooltip: qsTr("Look for the ColorChecker in the clip's picture at the playhead")
            onClicked: Color.findChart()
        }
        PillButton {
            objectName: "chartMatchButton"
            text: qsTr("Match")
            iconName: "palette"
            variant: "primary"
            compact: true
            visible: root.chartActive
            enabled: (root.chart.canApply ?? false) && !Color.busy
            tooltip: root.alsoSelected && (root.chart.others ?? 0) > 0 ? qsTr("Match this clip and the other selected clips to the chart")
                                                                       : qsTr("Match this clip to the chart (one undo step)")
            onClicked: Color.applyChart(root.alsoSelected && (root.chart.others ?? 0) > 0)
        }
        PillButton {
            objectName: "chartRotateButton"
            text: qsTr("Rotate")
            iconName: "rotate-cw"
            compact: true
            visible: root.chartActive
            tooltip: qsTr("The chart lies turned: assign its corners a quarter turn further")
            onClicked: Color.rotateChart()
        }
        PillButton {
            objectName: "chartMirrorButton"
            text: qsTr("Mirror")
            iconName: "arrow-left-right"
            compact: true
            visible: root.chartActive
            tooltip: qsTr("The picture is mirrored: read the chart from right to left")
            onClicked: Color.mirrorChart()
        }
        PillButton {
            objectName: "chartClearButton"
            text: qsTr("Clear")
            iconName: "x"
            variant: "ghost"
            compact: true
            visible: root.chartActive
            tooltip: qsTr("Close the chart and its corners on the monitor")
            onClicked: Color.clearChart()
        }
    }

    Text {
        objectName: "chartMessage"
        Layout.fillWidth: true
        visible: root.chartActive && text !== ""
        text: root.chart.message ?? ""
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.WordWrap
    }

    // Which print of the chart, and what to fit.
    RowLayout {
        Layout.fillWidth: true
        visible: root.chartActive
        spacing: Theme.space(2)
        Text {
            text: qsTr("Chart print")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
        PillTabs {
            objectName: "chartEditionTabs"
            Layout.fillWidth: true
            compact: true
            model: [qsTr("Since 2014"), qsTr("Before 2014")]
            currentIndex: (root.chart.edition ?? "after-2014") === "after-2014" ? 0 : 1
            onActivated: index => Color.setChartEdition(index === 0 ? "after-2014" : "before-2014")
            Accessible.name: qsTr("Chart print")
        }
    }
    RowLayout {
        Layout.fillWidth: true
        visible: root.chartActive
        Text {
            Layout.fillWidth: true
            text: qsTr("Match saturation and hue too")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            wrapMode: Text.WordWrap
        }
        Toggle {
            objectName: "chartColoursToggle"
            checked: root.chart.colours ?? true
            Accessible.name: qsTr("Match saturation and hue too")
            onToggled: Color.setChartColours(checked)
        }
    }
    RowLayout {
        Layout.fillWidth: true
        visible: root.chartActive && (root.chart.others ?? 0) > 0
        Text {
            Layout.fillWidth: true
            text: qsTr("Also match the other %n selected clip(s)", "", root.chart.others ?? 0)
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
            wrapMode: Text.WordWrap
        }
        Toggle {
            objectName: "chartAlsoToggle"
            checked: root.alsoSelected
            Accessible.name: qsTr("Also match the other selected clips")
            onToggled: root.alsoSelected = checked
        }
    }

    // The patches: top the chart's colour, bottom left now, bottom right matched.
    Item {
        id: grid
        objectName: "chartSwatches"
        Layout.fillWidth: true
        Layout.topMargin: Theme.space(1)
        readonly property real gap: Theme.space(0.5)
        readonly property real cell: Math.max(8, (width - 5 * gap) / 6)
        implicitHeight: 4 * cell + 3 * gap
        Layout.preferredHeight: implicitHeight
        visible: root.chartActive && root.hasPatches
        Accessible.ignored: true

        Repeater {
            model: root.hasPatches ? 24 : 0

            delegate: Item {
                id: swatch

                required property int index
                readonly property var patch: root.patches[index] ?? root.emptyPatch

                x: (index % 6) * (grid.cell + grid.gap)
                y: Math.floor(index / 6) * (grid.cell + grid.gap)
                width: grid.cell
                height: grid.cell
                opacity: patch.used ? 1 : 0.35

                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: root.hovered === swatch.index ? 2 : Theme.hairline
                    border.color: root.hovered === swatch.index ? Theme.accentStroke : Theme.border
                    radius: Theme.radiusField / 2
                    z: 2
                }
                Rectangle {
                    x: 1
                    y: 1
                    width: parent.width - 2
                    height: (parent.height - 2) / 2
                    color: swatch.patch.reference
                }
                Rectangle {
                    x: 1
                    y: parent.height / 2
                    width: (parent.width - 2) / 2
                    height: (parent.height - 2) / 2
                    color: swatch.patch.before
                }
                Rectangle {
                    x: parent.width / 2
                    y: parent.height / 2
                    width: (parent.width - 2) / 2
                    height: (parent.height - 2) / 2
                    color: swatch.patch.after
                }
                HoverHandler {
                    onHoveredChanged: {
                        if (hovered)
                            root.hovered = swatch.index
                        else if (root.hovered === swatch.index)
                            root.hovered = -1
                    }
                }
            }
        }
    }

    Text {
        objectName: "chartLegend"
        Layout.fillWidth: true
        visible: root.chartActive && root.hasPatches
        text: qsTr("Top: the chart's colour. Bottom left: now. Bottom right: matched.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.WordWrap
    }

    // The error, in numbers: the hovered patch, or the whole chart.
    Text {
        objectName: "chartError"
        Layout.fillWidth: true
        visible: root.chartActive && root.hasPatches
        readonly property var patch: root.hovered >= 0 ? root.patches[root.hovered] : null
        text: patch ? qsTr("%1 · ΔE %2 → %3").arg(patch.name).arg(root.format(patch.deltaBefore)).arg(root.format(patch.deltaAfter))
                    : qsTr("Mean ΔE %1 → %2 · worst %3 → %4")
                          .arg(root.format(root.chart.meanBefore ?? 0)).arg(root.format(root.chart.meanAfter ?? 0))
                          .arg(root.format(root.chart.maxBefore ?? 0)).arg(root.format(root.chart.maxAfter ?? 0))
        color: Theme.textPrimary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        font.weight: Font.DemiBold
        wrapMode: Text.WordWrap
        opacity: root.chart.stale ? 0.5 : 1
        Accessible.name: text
    }

    // The hue points always fit the 24 patches; this is the error between them, found by leaving each patch out in turn.
    Text {
        objectName: "chartHoldOut"
        Layout.fillWidth: true
        visible: root.chartActive && root.hasPatches && (root.chart.hueCurvePoints ?? 0) > 0
        text: qsTr("Between the patches (each left out in turn): about ΔE %1").arg(root.format(root.chart.holdOut ?? 0))
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.WordWrap
        opacity: root.chart.stale ? 0.5 : 1
    }

    Text {
        objectName: "chartProblem"
        Layout.fillWidth: true
        visible: root.chartActive && text !== ""
        text: (root.chart.error ?? "") !== "" ? root.chart.error
              : root.hasPatches && !(root.chart.improved ?? false)
                ? qsTr("This does not bring the chart closer to its colours: check the corners, Rotate or Mirror, and the chart print.")
                : ""
        color: Theme.warningText
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.WordWrap
    }
}
