pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Sequence > Reframe… (AI-9): the current sequence made again at another aspect, its clips following their subjects
// with editable Position/Scale keyframes. Preview finds the subjects (on this computer, cached) and shows the frame path
// over the source frames: the crop window (accent) and the subjects it keeps in view (dashed); scrub through each clip.
// Create sequence applies it as one undo step and switches to the new sequence.
WabcutDialog {
    id: dialog
    objectName: "reframeDialog"

    readonly property var aspects: ["9:16", "1:1", "4:5", "16:9"]
    readonly property var modes: ["follow", "faces", "speaker"]
    readonly property var fits: ["crop", "letterbox", "blur"]
    property int clipIndex: 0
    property int sample: 0
    readonly property var clips: Reframe.preview.clips ?? []
    readonly property var activeClip: clips.length > 0 ? clips[Math.min(clipIndex, clips.length - 1)] : null
    readonly property var point: activeClip && activeClip.path.length > 0 ? activeClip.path[Math.min(sample, activeClip.path.length - 1)] : null

    function options() {
        return { aspect: aspects[aspectTabs.currentIndex], mode: modes[modeTabs.currentIndex], fit: fits[fitTabs.currentIndex],
                 smoothness: smoothSlider.value }
    }
    function refresh() {
        clipIndex = 0
        sample = 0
        Reframe.makePreview(options())
    }
    /// The subject box nearest in time to the current sample.
    function subjectAt(t) {
        if (!activeClip)
            return null
        let best = null
        let bestDistance = 1e9
        for (const s of activeClip.subjects) {
            const d = Math.abs(s[0] - t)
            if (d < bestDistance) {
                bestDistance = d
                best = s
            }
        }
        return bestDistance < 0.3 ? best : null
    }

    title: qsTr("Reframe “%1”").arg(Reframe.sourceName)
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(220), Theme.space(220))
    onOpened: refresh()
    onClosed: Reframe.discard()

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
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

        // ---- The preview: a source frame with the crop window and the subjects ----
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(1.5)

            Rectangle {
                id: stage
                objectName: "reframePreview"
                Layout.fillWidth: true
                Layout.preferredHeight: width / (dialog.activeClip ? dialog.activeClip.sourceAspect : 16 / 9)
                radius: Theme.radiusField
                color: Theme.canvas
                clip: true

                Image {
                    id: frameImage
                    anchors.fill: parent
                    source: dialog.activeClip && dialog.activeClip.frames.length > 0 ? dialog.activeClip.frames[Math.min(dialog.sample, dialog.activeClip.frames.length - 1)] : ""
                    sourceSize: Qt.size(640, 360)
                    fillMode: Image.Stretch
                    asynchronous: true
                }
                // Outside the window: dimmed.
                Rectangle {
                    anchors.fill: parent
                    visible: dialog.point !== null
                    color: Theme.scrim
                    layer.enabled: true
                }
                Item {
                    id: window
                    objectName: "reframeWindow"
                    visible: dialog.point !== null
                    x: dialog.point ? Math.max(0, dialog.point[1]) * stage.width : 0
                    y: dialog.point ? Math.max(0, dialog.point[2]) * stage.height : 0
                    width: dialog.point ? Math.min(dialog.point[3], 1) * stage.width : 0
                    height: dialog.point ? Math.min(dialog.point[4], 1) * stage.height : 0
                    clip: true
                    Behavior on x { enabled: !Theme.reduceMotion; NumberAnimation { duration: Theme.durState } }
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
                // The subject the window keeps in view.
                Rectangle {
                    readonly property var box: dialog.point ? dialog.subjectAt(dialog.point[0]) : null
                    objectName: "reframeSubject"
                    visible: box !== null
                    x: box ? box[1] * stage.width : 0
                    y: box ? box[2] * stage.height : 0
                    width: box ? box[3] * stage.width : 0
                    height: box ? box[4] * stage.height : 0
                    color: "transparent"
                    border.width: 1.5
                    border.color: Theme.success
                }
                Text {
                    anchors.centerIn: parent
                    visible: !dialog.point
                    text: Reframe.busy ? Reframe.phase : qsTr("Preview to see the frame path")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
            }

            // Scrub the clip's path; step between clips.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                visible: dialog.clips.length > 0
                IconButton {
                    iconName: "chevron-left"
                    tooltip: qsTr("Previous clip")
                    enabled: dialog.clipIndex > 0
                    onClicked: { dialog.clipIndex -= 1; dialog.sample = 0 }
                }
                Text {
                    text: dialog.activeClip ? qsTr("Clip %1 of %2 · %3").arg(dialog.clipIndex + 1).arg(dialog.clips.length).arg(dialog.activeClip.name) : ""
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    elide: Text.ElideRight
                    Layout.maximumWidth: Theme.space(60)
                }
                IconButton {
                    iconName: "chevron-right"
                    tooltip: qsTr("Next clip")
                    enabled: dialog.clipIndex + 1 < dialog.clips.length
                    onClicked: { dialog.clipIndex += 1; dialog.sample = 0 }
                }
                ValueSlider {
                    id: timeSlider
                    objectName: "reframeTimeSlider"
                    Layout.fillWidth: true
                    from: 0
                    to: dialog.activeClip ? Math.max(0, dialog.activeClip.path.length - 1) : 0
                    stepSize: 1
                    value: dialog.sample
                    onMoved: dialog.sample = Math.round(value)
                }
                Text {
                    text: dialog.point ? dialog.point[0].toFixed(1) + " s" : ""
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                }
            }
            Note {
                objectName: "reframeSummary"
                visible: Reframe.preview.name !== undefined
                text: qsTr("“%1”, %2 × %3: %n clip(s) reframed, %4 keyframes", "", Reframe.preview.clipsReframed ?? 0)
                      .arg(Reframe.preview.name ?? "").arg(Reframe.preview.width ?? 0).arg(Reframe.preview.height ?? 0)
                      .arg(Reframe.preview.keyframes ?? 0)
                      + (dialog.activeClip ? " · " + qsTr("guided by %1").arg(dialog.activeClip.guide) : "")
            }
            Note {
                objectName: "reframeMessage"
                visible: Reframe.message !== ""
                text: Reframe.message
                color: Theme.warning
            }
        }

        // ---- The settings ----
        GridLayout {
            Layout.preferredWidth: Theme.space(70)
            Layout.alignment: Qt.AlignTop
            columns: 1
            rowSpacing: Theme.space(1.5)
            enabled: !Reframe.busy

            Label { text: qsTr("Aspect") }
            PillTabs {
                id: aspectTabs
                objectName: "reframeAspectTabs"
                Layout.fillWidth: true
                compact: true
                model: [qsTr("9:16"), qsTr("1:1"), qsTr("4:5"), qsTr("16:9")]
                onActivated: dialog.refresh()
            }
            Label { text: qsTr("Follow") }
            PillTabs {
                id: modeTabs
                objectName: "reframeModeTabs"
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Subject"), qsTr("All faces"), qsTr("Speaker")]
                onActivated: dialog.refresh()
            }
            Label { text: qsTr("When subjects don't fit") }
            PillTabs {
                id: fitTabs
                objectName: "reframeFitTabs"
                Layout.fillWidth: true
                compact: true
                model: [qsTr("Crop"), qsTr("Letterbox"), qsTr("Blur fill")]
                onActivated: dialog.refresh()
            }
            Label { text: qsTr("Smoothness") }
            ValueSlider {
                id: smoothSlider
                objectName: "reframeSmoothness"
                Layout.fillWidth: true
                from: 0
                to: 1
                value: 0.5
                onPressedChanged: if (!pressed) dialog.refresh()
            }
            Note {
                text: smoothSlider.value < 0.34 ? qsTr("Follows closely") : smoothSlider.value > 0.66 ? qsTr("Moves only when it must")
                                                                                                    : qsTr("Still, then steady pans")
            }

            // The detectors: faces and people, or salient regions only until they are downloaded.
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: detectorColumn.implicitHeight + Theme.space(3)
                radius: Theme.radiusField
                color: Theme.sunken
                ColumnLayout {
                    id: detectorColumn
                    anchors.fill: parent
                    anchors.margins: Theme.space(1.5)
                    spacing: Theme.space(1)
                    Note {
                        objectName: "reframeDetectorStatus"
                        text: Reframe.detectorStatus
                        color: Reframe.detectorsAvailable ? Theme.textSecondary : Theme.warning
                    }
                    Note {
                        visible: !Reframe.detectorsAvailable
                        text: Reframe.detectorLicense
                        font.pixelSize: Theme.textXs
                    }
                    PillButton {
                        objectName: "reframeDownloadButton"
                        visible: !Reframe.detectorsAvailable
                        compact: true
                        iconName: "download"
                        text: Reframe.downloading ? qsTr("Downloading %1%").arg(Math.round(Reframe.downloadProgress * 100)) : qsTr("Download detectors (21 MB)")
                        onClicked: Reframe.downloading ? Reframe.cancelDownload() : Reframe.downloadDetectors()
                    }
                }
            }
        }
    }

    // ---- The job: a pill and the UV beam ----
    RowLayout {
        Layout.fillWidth: true
        visible: Reframe.busy
        spacing: Theme.space(2)
        StatusPill {
            objectName: "reframeJobPill"
            label: Reframe.phase
            detail: Reframe.progress >= 0 ? Math.round(Reframe.progress * 100) + "%" : ""
            dotColor: Theme.accentFill
        }
        BeamProgress {
            Layout.fillWidth: true
            value: Reframe.progress >= 0 ? Reframe.progress : 1
            active: Reframe.busy
            label: Reframe.phase
        }
    }

    footerData: [
        PillButton {
            text: Reframe.busy ? qsTr("Stop") : qsTr("Cancel")
            onClicked: Reframe.busy ? Reframe.cancel() : dialog.close()
        },
        PillButton {
            objectName: "reframePreviewButton"
            iconName: "rotate-cw"
            text: qsTr("Preview")
            enabled: !Reframe.busy
            onClicked: dialog.refresh()
        },
        PillButton {
            objectName: "reframeCreateButton"
            variant: "primary"
            iconName: "crop"
            text: qsTr("Create sequence")
            enabled: !Reframe.busy && Reframe.preview.name !== undefined
            onClicked: {
                if (Reframe.apply() === "")
                    dialog.close()
            }
        }
    ]
}
