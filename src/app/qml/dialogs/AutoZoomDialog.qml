pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Clip > Auto zoom… (AI-11): a screen recording zooms in where the action is, with editable Position/Scale keyframes.
// Preview finds the activity (the recording's cursor log when there is one, otherwise the picture: clicks, typing, a
// resting cursor) and shows the zoom window over the recording's frames, the activity near the playhead and the zoom
// over time; scrub or click the graph. Apply adds the keyframes as one undo step; running it again replaces them.
WabcutDialog {
    id: dialog
    objectName: "autoZoomDialog"

    /// The clip ("c<id>") the dialog works on; set by openFor().
    property string clipKey: ""
    property int sample: 0
    readonly property var easings: ["easeInOut", "linear", "smooth"]
    readonly property var path: AutoZoom.zoomPreview.path ?? []
    readonly property var point: path.length > 0 ? path[Math.min(sample, path.length - 1)] : null
    readonly property var activity: AutoZoom.zoomPreview.activity ?? []
    readonly property var segments: AutoZoom.zoomPreview.segments ?? []

    function openFor(key) {
        clipKey = key
        open()
    }
    function options() {
        return { maxZoom: zoomSlider.value, hold: holdSlider.value, easing: easings[easingTabs.currentIndex],
                 clicks: clicksToggle.checked, typing: typingToggle.checked, dwell: dwellToggle.checked }
    }
    function refresh() {
        sample = 0
        AutoZoom.preview(clipKey, options())
    }
    /// Activity within half a second of `t`.
    function activityNear(t) {
        return activity.filter(e => Math.abs(e.t - t) <= 0.5)
    }
    function originText(origin) {
        return origin === "sidecar" ? qsTr("from the cursor log") : origin === "frames" ? qsTr("from the picture") : origin ?? ""
    }

    title: qsTr("Auto zoom “%1”").arg(AutoZoom.zoomPreview.name ?? "")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(220), Theme.space(220))
    onOpened: refresh()
    onClosed: AutoZoom.discard()

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    component Value: Text {
        color: Theme.textTertiary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textSm
    }
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(4)

        // ---- The preview: a recording frame with the zoom window and the activity ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(1.5)

            Rectangle {
                id: stage
                objectName: "autoZoomPreview"
                Layout.fillWidth: true
                Layout.preferredHeight: width * 9 / 16
                radius: Theme.radiusField
                color: Theme.canvas
                clip: true

                Image {
                    id: frameImage
                    anchors.fill: parent
                    source: dialog.point && dialog.point.frame ? dialog.point.frame : ""
                    sourceSize: Qt.size(640, 360)
                    fillMode: Image.Stretch
                    asynchronous: true
                }
                // Outside the window: dimmed.
                Rectangle {
                    anchors.fill: parent
                    visible: dialog.point !== null && dialog.point.zoom > 1.001
                    color: Theme.scrim
                }
                Item {
                    id: window
                    objectName: "autoZoomWindow"
                    visible: dialog.point !== null
                    x: dialog.point ? (dialog.point.x - dialog.point.w / 2) * stage.width : 0
                    y: dialog.point ? (dialog.point.y - dialog.point.h / 2) * stage.height : 0
                    width: dialog.point ? Math.min(dialog.point.w, 1) * stage.width : 0
                    height: dialog.point ? Math.min(dialog.point.h, 1) * stage.height : 0
                    clip: true
                    Image {
                        x: -window.x
                        y: -window.y
                        width: stage.width
                        height: stage.height
                        source: frameImage.source
                        sourceSize: frameImage.sourceSize
                        fillMode: Image.Stretch
                    }
                    Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                        border.width: 2
                        border.color: Theme.accentStroke
                    }
                }
                // The activity near the playhead: clicks as rings, the rest as dots.
                Repeater {
                    model: dialog.point ? dialog.activityNear(dialog.point.t) : []
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool click: modelData.kind === "click"
                        objectName: "autoZoomEvent"
                        width: click ? Theme.space(4) : Theme.space(2)
                        height: width
                        radius: width / 2
                        x: modelData.x * stage.width - width / 2
                        y: modelData.y * stage.height - height / 2
                        color: click ? "transparent" : Theme.success
                        border.width: click ? 2 : 0
                        border.color: Theme.success
                    }
                }
                Text {
                    anchors.centerIn: parent
                    visible: !dialog.point
                    text: AutoZoom.busy ? AutoZoom.phase : qsTr("Preview to see where it zooms")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                StatusPill {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.space(1.5)
                    visible: dialog.point !== null
                    label: dialog.point ? "×" + dialog.point.zoom.toFixed(2) : ""
                    dotColor: Theme.accentFill
                }
            }

            // The zoom over time: the curve, the zoom segments and the activity ticks; click to scrub.
            Rectangle {
                objectName: "autoZoomGraph"
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.space(14)
                radius: Theme.radiusField
                color: Theme.sunken
                visible: dialog.path.length > 1
                Canvas {
                    id: graph
                    anchors.fill: parent
                    anchors.margins: Theme.space(1)
                    readonly property real t0: dialog.path.length > 0 ? dialog.path[0].t : 0
                    readonly property real t1: dialog.path.length > 0 ? dialog.path[dialog.path.length - 1].t : 1
                    readonly property real ceiling: Math.max(2, AutoZoom.zoomPreview.maxZoom ?? 2)
                    function xOf(t) { return (t - t0) / Math.max(1e-6, t1 - t0) * width }
                    function yOf(z) { return height - (z - 1) / Math.max(1e-6, ceiling - 1) * (height - 4) - 2 }
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        ctx.fillStyle = Qt.alpha(Theme.accentFill, 0.12)
                        for (const s of dialog.segments) {
                            if (s.zoom > 1.001)
                                ctx.fillRect(xOf(s.start), 0, Math.max(1, xOf(s.end) - xOf(s.start)), height)
                        }
                        ctx.fillStyle = Theme.success
                        for (const e of dialog.activity)
                            ctx.fillRect(xOf(e.t) - 0.5, height - 5, 1, 5)
                        ctx.strokeStyle = Theme.accentStroke
                        ctx.lineWidth = 1.5
                        ctx.beginPath()
                        dialog.path.forEach((p, i) => i === 0 ? ctx.moveTo(xOf(p.t), yOf(p.zoom)) : ctx.lineTo(xOf(p.t), yOf(p.zoom)))
                        ctx.stroke()
                    }
                    Connections {
                        target: AutoZoom
                        function onResultChanged() { graph.requestPaint() }
                    }
                    Rectangle {
                        width: 1.5
                        height: parent.height
                        color: Theme.textPrimary
                        x: dialog.point ? graph.xOf(dialog.point.t) : 0
                        visible: dialog.point !== null
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        function scrub(mouseX) {
                            const t = graph.t0 + Math.max(0, Math.min(1, mouseX / width)) * (graph.t1 - graph.t0)
                            let best = 0
                            for (let i = 0; i < dialog.path.length; ++i) {
                                if (Math.abs(dialog.path[i].t - t) < Math.abs(dialog.path[best].t - t))
                                    best = i
                            }
                            dialog.sample = best
                        }
                        onPressed: mouse => scrub(mouse.x)
                        onPositionChanged: mouse => { if (pressed) scrub(mouse.x) }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                visible: dialog.path.length > 0
                ValueSlider {
                    objectName: "autoZoomTimeSlider"
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(0, dialog.path.length - 1)
                    stepSize: 1
                    value: dialog.sample
                    onMoved: dialog.sample = Math.round(value)
                }
                Value { text: dialog.point ? dialog.point.t.toFixed(1) + " s" : "" }
            }
            Note {
                objectName: "autoZoomSummary"
                visible: AutoZoom.zoomPreview.keyframes !== undefined
                text: qsTr("%n zoom(s), up to ×%1, %2 keyframes", "", dialog.segments.filter(s => s.zoom > 1.001).length)
                      .arg((AutoZoom.zoomPreview.maxZoom ?? 1).toFixed(1)).arg(AutoZoom.zoomPreview.keyframes ?? 0)
                      + " · " + dialog.originText(AutoZoom.zoomPreview.origin)
                      + ((AutoZoom.zoomPreview.replacedKeys ?? 0) > 0 ? " · " + qsTr("replaces the earlier auto zoom") : "")
            }
            Note {
                objectName: "autoZoomMessage"
                visible: AutoZoom.message !== ""
                text: AutoZoom.message
                color: Theme.warning
            }
        }

        // ---- The settings ----
        GridLayout {
            Layout.preferredWidth: Theme.space(70)
            Layout.alignment: Qt.AlignTop
            columns: 2
            rowSpacing: Theme.space(1.5)
            enabled: !AutoZoom.busy

            Label { text: qsTr("Most zoom") }
            Value { Layout.alignment: Qt.AlignRight; text: "×" + zoomSlider.value.toFixed(1) }
            ValueSlider {
                id: zoomSlider
                objectName: "autoZoomMaxZoom"
                Layout.columnSpan: 2
                Layout.fillWidth: true
                from: 1.2
                to: 4
                stepSize: 0.1
                value: 2
                onPressedChanged: if (!pressed) dialog.refresh()
            }
            Label { text: qsTr("Hold") }
            Value { Layout.alignment: Qt.AlignRight; text: holdSlider.value.toFixed(1) + " s" }
            ValueSlider {
                id: holdSlider
                objectName: "autoZoomHold"
                Layout.columnSpan: 2
                Layout.fillWidth: true
                from: 0.5
                to: 5
                stepSize: 0.1
                value: 1.5
                onPressedChanged: if (!pressed) dialog.refresh()
            }
            Label { Layout.columnSpan: 2; text: qsTr("Motion") }
            PillTabs {
                id: easingTabs
                objectName: "autoZoomEasing"
                Layout.columnSpan: 2
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Ease"), qsTr("Linear"), qsTr("Smooth")]
                onActivated: dialog.refresh()
            }
            Label { Layout.columnSpan: 2; text: qsTr("Zoom in on") }
            Toggle {
                id: clicksToggle
                objectName: "autoZoomClicks"
                Layout.columnSpan: 2
                Layout.fillWidth: true
                text: qsTr("Clicks")
                checked: true
                onToggled: dialog.refresh()
            }
            Toggle {
                id: typingToggle
                objectName: "autoZoomTyping"
                Layout.columnSpan: 2
                Layout.fillWidth: true
                text: qsTr("Typing")
                checked: true
                onToggled: dialog.refresh()
            }
            Toggle {
                id: dwellToggle
                objectName: "autoZoomDwell"
                Layout.columnSpan: 2
                Layout.fillWidth: true
                text: qsTr("Where the cursor rests")
                checked: true
                onToggled: dialog.refresh()
            }
            Note {
                Layout.columnSpan: 2
                text: qsTr("A cursor log next to the recording (“name.cursor.json” or “.csv”) is used when there is one. The keyframes stay editable in the Inspector; running Auto zoom again replaces them.")
            }
        }
    }

    // ---- The job: a pill and the UV beam ----
    RowLayout {
        Layout.fillWidth: true
        visible: AutoZoom.busy
        spacing: Theme.space(2)
        StatusPill {
            objectName: "autoZoomJobPill"
            label: AutoZoom.phase
            detail: AutoZoom.progress >= 0 ? Math.round(AutoZoom.progress * 100) + "%" : ""
            dotColor: Theme.accentFill
        }
        BeamProgress {
            Layout.fillWidth: true
            value: AutoZoom.progress >= 0 ? AutoZoom.progress : 1
            active: AutoZoom.busy
            label: AutoZoom.phase
        }
    }

    footerData: [
        PillButton {
            text: AutoZoom.busy ? qsTr("Stop") : qsTr("Cancel")
            onClicked: AutoZoom.busy ? AutoZoom.cancel() : dialog.close()
        },
        PillButton {
            objectName: "autoZoomPreviewButton"
            iconName: "rotate-cw"
            text: qsTr("Preview")
            enabled: !AutoZoom.busy
            onClicked: dialog.refresh()
        },
        PillButton {
            objectName: "autoZoomApplyButton"
            variant: "primary"
            iconName: "zoom-in"
            text: qsTr("Add zoom")
            enabled: !AutoZoom.busy && AutoZoom.staged
            onClicked: {
                if (AutoZoom.apply() === "")
                    dialog.close()
            }
        }
    ]
}
