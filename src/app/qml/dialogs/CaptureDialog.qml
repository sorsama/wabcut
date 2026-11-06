pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// File > Capture… (0.7, docs/capture.md): Premiere's Capture panel adapted. Pick a video device (a camera, a capture card,
// a tape deck the system exposes as one) and one of its formats, the sound input and how the picture is written; watch the
// live picture; press Record and Stop. The finished file is imported into the bin chosen here as one undo step, "Capture".
//
// Nothing is opened until Start preview or Record: picking a device only reads its formats. The panel stays open beside the
// editor (it is not modal) and cannot be closed while a capture runs. Windows only in this version: elsewhere it says so.
WabcutDialog {
    id: dialog
    objectName: "captureDialog"

    /// The Library bin the capture goes into ("b12"; "" = the root).
    property string binKey: ""
    property var binOptions: []
    readonly property string phase: Capture.state
    readonly property bool busy: phase === "opening" || phase === "starting" || phase === "finishing"
    readonly property bool recording: phase === "recording"
    readonly property bool capturing: recording || phase === "starting" || phase === "finishing"
    readonly property bool live: phase !== "idle"
    readonly property bool wide: width >= Theme.space(190)
    readonly property bool ready: Capture.supported && Capture.videoDevice !== "" && Capture.formatIndex >= 0

    function refreshBins() {
        const bins = Library.binTree([])
        const options = []
        for (let i = 0; i < bins.length; ++i) {
            if (bins[i].disabled === true && bins[i].current !== true)
                continue
            options.push({ id: bins[i].key, name: " ".repeat(bins[i].depth) + bins[i].name })
        }
        dialog.binOptions = options
    }
    /// Opens the panel: devices listed (none opened), a fresh name, the bin the Library shows.
    function openPanel() {
        dialog.binKey = Library.currentBin
        dialog.refreshBins()
        nameField.text = Capture.suggestName()
        Capture.clearMessages()
        Capture.refresh()
        dialog.open()
    }

    title: qsTr("Capture")
    modal: false
    dim: false
    closePolicy: dialog.capturing || dialog.busy ? Popup.NoAutoClose : Popup.CloseOnEscape
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(220), Theme.space(220))
    onClosed: {
        if (Capture.state === "previewing")
            Capture.stopPreview() // the camera is not left on behind a closed panel
    }

    Connections {
        target: Capture
        function onCaptured() {
            nameField.text = Capture.suggestName()
        }
    }

    component FieldLabel: Text {
        Layout.fillWidth: true
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Theme.weightLabel
        elide: Text.ElideRight
    }
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    component Stat: Rectangle {
        id: stat
        property string label: ""
        property string value: ""
        implicitHeight: Theme.controlHeightSmall + Theme.space(2)
        implicitWidth: statRow.implicitWidth + Theme.space(6)
        radius: height / 2
        color: Theme.raised
        Accessible.role: Accessible.StaticText
        Accessible.name: stat.label !== "" ? qsTr("%1: %2").arg(stat.label).arg(stat.value) : stat.value
        Row {
            id: statRow
            anchors.centerIn: parent
            spacing: Theme.space(2)
            Text {
                visible: stat.label !== ""
                anchors.verticalCenter: parent.verticalCenter
                text: stat.label
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: stat.value
                color: Theme.textPrimary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textSm
            }
        }
    }

    // The body scrolls when the window is too short for all of it (the smallest window is 1100 x 700); the buttons stay in view.
    Flickable {
        id: scroller
        objectName: "captureScroller"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(bodyColumn.implicitHeight,
                                         Math.max(Theme.space(40), (dialog.parent ? dialog.parent.height : Theme.space(170)) - Theme.space(64)))
        contentWidth: width
        contentHeight: bodyColumn.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: bodyColumn
            width: scroller.width - Theme.space(3) // room for the scroll bar
            spacing: Theme.space(3)

            // ---- Not available here ----
            ColumnLayout {
                objectName: "captureUnsupported"
                visible: !Capture.supported
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignHCenter
                spacing: Theme.space(3)
                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: "camera"
                    size: Theme.iconXl
                    color: Theme.textTertiary
                }
                Text {
                    objectName: "captureUnsupportedText"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: Capture.unsupportedReason
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
            }

            // ---- Captures an earlier run left ----
            ColumnLayout {
                objectName: "captureInterrupted"
                visible: Capture.supported && Capture.interrupted.length > 0
                Layout.fillWidth: true
                spacing: Theme.space(2)
                Text {
                    Layout.fillWidth: true
                    text: qsTr("%n capture(s) did not reach the project. Wabcut stopped before they were added; what reached the disk is kept.", "",
                               Capture.interrupted.length)
                    wrapMode: Text.Wrap
                    color: Theme.warningText
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    font.weight: Theme.weightLabel
                }
                Repeater {
                    model: Capture.interrupted
                    delegate: RowLayout {
                        id: interruptedRow
                        required property var modelData
                        required property int index
                        objectName: "captureInterruptedRow_" + index
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("%1 · %2 · started %3").arg(interruptedRow.modelData.name).arg(interruptedRow.modelData.sizeText)
                                  .arg(interruptedRow.modelData.startedAt)
                            elide: Text.ElideMiddle
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                        PillButton {
                            objectName: "captureRecover"
                            compact: true
                            variant: "primary"
                            text: qsTr("Recover and add")
                            enabled: !dialog.capturing && !Capture.loading
                            tooltip: qsTr("Repair the file and add it to the bin chosen below")
                            onClicked: Capture.recoverInterrupted(interruptedRow.modelData.path, dialog.binKey)
                        }
                        PillButton {
                            objectName: "captureDiscard"
                            compact: true
                            variant: "ghost"
                            text: qsTr("Delete")
                            enabled: !dialog.capturing
                            tooltip: qsTr("Delete the file")
                            onClicked: Capture.discardInterrupted(interruptedRow.modelData.path)
                        }
                    }
                }
            }

            // ---- Messages ----
            RowLayout {
                objectName: "captureErrorRow"
                visible: Capture.error !== ""
                Layout.fillWidth: true
                spacing: Theme.space(3)
                Icon {
                    Layout.alignment: Qt.AlignTop
                    name: "triangle-alert"
                    size: Theme.iconMd
                    color: Theme.dangerText
                }
                Text {
                    objectName: "captureError"
                    Layout.fillWidth: true
                    text: Capture.error
                    wrapMode: Text.Wrap
                    color: Theme.dangerText
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }
                PillButton {
                    objectName: "captureOpenPrivacy"
                    Layout.alignment: Qt.AlignTop
                    visible: Capture.accessKind !== ""
                    compact: true
                    text: Capture.accessKind === "microphone" ? qsTr("Open microphone settings") : qsTr("Open camera settings")
                    iconName: "external-link"
                    onClicked: Capture.openPrivacySettings()
                }
            }
            Text {
                objectName: "captureNotice"
                visible: Capture.notice !== ""
                Layout.fillWidth: true
                text: Capture.notice
                wrapMode: Text.Wrap
                color: Theme.success
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Theme.weightLabel
            }

            // ---- The panel ----
            GridLayout {
                objectName: "captureColumns"
                visible: Capture.supported
                Layout.fillWidth: true
                columns: dialog.wide ? 2 : 1
                columnSpacing: Theme.space(5)
                rowSpacing: Theme.space(3)

                // The live picture, its level and the counters.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: Theme.space(100)
                    spacing: Theme.space(2)

                    Rectangle {
                        id: viewBox
                        objectName: "captureView"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(Math.max(width / Capture.aspect, Theme.space(36)), Theme.space(100))
                        radius: Theme.radiusMonitor
                        color: Theme.sunken
                        clip: true
                        Accessible.role: Accessible.Graphic
                        Accessible.name: qsTr("Live picture")

                        // Two images take turns: the new picture loads (on the loader's thread) behind the one on show and comes
                        // to the front when it is ready, so the view never blinks while a picture converts.
                        Item {
                            id: picture
                            objectName: "capturePicture"
                            anchors.fill: parent
                            readonly property int serial: Capture.previewSerial
                            readonly property bool active: dialog.live && Capture.status.picture === true
                            property int front: 0 // 0: `first` is on show, 1: `second`
                            property bool pending: false
                            property int wanted: -1
                            property int shownSerial: -1
                            readonly property bool hasPicture: picture.active && picture.shownSerial >= 0

                            function request() {
                                if (!picture.active || picture.pending || picture.serial === picture.shownSerial)
                                    return
                                picture.pending = true
                                picture.wanted = picture.serial
                                const back = picture.front === 0 ? second : first
                                back.source = "image://wabcut-capture/" + picture.serial
                            }
                            function loaded(index: int, ready: bool) {
                                picture.pending = false
                                if (ready) {
                                    picture.front = index
                                    picture.shownSerial = picture.wanted
                                }
                                picture.request()
                            }
                            onSerialChanged: picture.request()
                            onActiveChanged: {
                                if (picture.active) {
                                    picture.request()
                                } else {
                                    first.source = ""
                                    second.source = ""
                                    picture.pending = false
                                    picture.shownSerial = -1
                                }
                            }

                            Image {
                                id: first
                                anchors.fill: parent
                                z: picture.front === 0 ? 1 : 0
                                visible: picture.hasPicture
                                cache: false
                                asynchronous: true
                                fillMode: Image.PreserveAspectFit
                                sourceSize.width: Math.max(160, Math.ceil(viewBox.width / 32) * 32)
                                onStatusChanged: if (status === Image.Ready || status === Image.Error) picture.loaded(0, status === Image.Ready)
                            }
                            Image {
                                id: second
                                anchors.fill: parent
                                z: picture.front === 1 ? 1 : 0
                                visible: picture.hasPicture
                                cache: false
                                asynchronous: true
                                fillMode: Image.PreserveAspectFit
                                sourceSize.width: Math.max(160, Math.ceil(viewBox.width / 32) * 32)
                                onStatusChanged: if (status === Image.Ready || status === Image.Error) picture.loaded(1, status === Image.Ready)
                            }
                        }
                        Text {
                            objectName: "captureViewText"
                            visible: !picture.hasPicture
                            anchors.centerIn: parent
                            width: parent.width - Theme.space(10)
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.Wrap
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            text: dialog.phase === "opening" ? qsTr("Opening the device…")
                                  : dialog.live ? qsTr("Waiting for the first picture…")
                                  : !dialog.ready ? qsTr("Choose a video device, then press Start preview.")
                                  : qsTr("Press Start preview to see the picture. The camera light comes on.")
                        }
                        // The recording mark.
                        Rectangle {
                            objectName: "captureRecMark"
                            visible: dialog.recording
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.margins: Theme.space(3)
                            width: markRow.implicitWidth + Theme.space(5)
                            height: Theme.controlHeightSmall
                            radius: height / 2
                            color: Qt.rgba(0, 0, 0, 0.55)
                            Row {
                                id: markRow
                                anchors.centerIn: parent
                                spacing: Theme.space(2)
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Theme.dotSize
                                    height: Theme.dotSize
                                    radius: Theme.dotSize / 2
                                    color: Theme.danger
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("REC")
                                    color: Theme.scrimText
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textSm
                                    font.weight: Theme.weightTitle
                                }
                            }
                        }
                    }

                    // The sound level.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        Icon {
                            name: "mic"
                            size: Theme.iconSm
                            color: Theme.textTertiary
                        }
                        Rectangle {
                            id: meter
                            objectName: "captureMeter"
                            Layout.fillWidth: true
                            Layout.preferredHeight: Theme.space(2)
                            radius: height / 2
                            color: Theme.well
                            readonly property real level: Capture.status.level ?? 0
                            // Peak, 60 dB scale.
                            readonly property real fraction: level > 0.001 ? Math.max(0, Math.min(1, (20 * Math.log10(level) + 60) / 60)) : 0
                            Accessible.role: Accessible.ProgressBar
                            Accessible.name: qsTr("Sound level")
                            Rectangle {
                                width: parent.width * meter.fraction
                                height: parent.height
                                radius: height / 2
                                color: meter.fraction > 0.95 ? Theme.danger : meter.fraction > 0.85 ? Theme.warning : Theme.success
                            }
                        }
                        Text {
                            Layout.preferredWidth: Theme.space(22)
                            horizontalAlignment: Text.AlignRight
                            text: dialog.live ? Capture.status.fpsText ?? "" : ""
                            color: Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                            elide: Text.ElideRight
                        }
                    }

                    // The time counter and the counters.
                    Text {
                        objectName: "captureTime"
                        text: dialog.live ? Capture.status.elapsedText ?? "0:00:00" : "0:00:00"
                        color: dialog.recording ? Theme.textPrimary : Theme.textSecondary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textDisplay
                        font.weight: Theme.weightTitle
                        Accessible.role: Accessible.StaticText
                        Accessible.name: qsTr("Recording time %1").arg(text)
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        Stat {
                            objectName: "captureDropped"
                            visible: dialog.live
                            value: Capture.status.droppedText ?? ""
                        }
                        Stat {
                            objectName: "captureSize"
                            visible: dialog.recording
                            label: qsTr("Written")
                            value: Capture.status.sizeText ?? ""
                        }
                        Stat {
                            objectName: "captureEncoder"
                            visible: dialog.recording && (Capture.status.encoder ?? "") !== ""
                            label: qsTr("Encoder")
                            value: Capture.status.encoder ?? ""
                        }
                    }
                    Note {
                        objectName: "captureDisk"
                        text: Capture.diskText
                    }
                }

                // The choices.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: Theme.space(100)
                    spacing: Theme.space(2)
                    enabled: !dialog.capturing && !dialog.busy

                    FieldLabel { text: qsTr("Video device") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        CaptureChoice {
                            objectName: "captureDevice"
                            Layout.fillWidth: true
                            label: qsTr("Video device")
                            options: Capture.videoDevices
                            current: Capture.videoDevice
                            placeholder: Capture.videoDevices.length > 0 ? qsTr("Choose a video device") : Capture.loading ? qsTr("Looking for devices…")
                                                                                                                         : qsTr("No video device found")
                            enabled: Capture.videoDevices.length > 0
                            onChosen: id => Capture.videoDevice = id
                        }
                        IconButton {
                            objectName: "captureRefresh"
                            variant: "secondary"
                            iconName: "rotate-cw"
                            tooltip: qsTr("Look for devices again")
                            onClicked: Capture.refresh()
                        }
                    }

                    FieldLabel { text: qsTr("Format") }
                    CaptureChoice {
                        objectName: "captureFormat"
                        Layout.fillWidth: true
                        label: qsTr("Format")
                        options: Capture.formats.map(f => ({ id: String(f.index), name: f.label }))
                        current: String(Capture.formatIndex)
                        placeholder: Capture.loading && Capture.videoDevice !== "" ? qsTr("Reading the device's formats…") : qsTr("Choose a device first")
                        enabled: Capture.formats.length > 0
                        tooltip: qsTr("Picture size, frame rate and the format the device sends")
                        onChosen: id => Capture.formatIndex = Number(id)
                    }

                    FieldLabel { text: qsTr("Sound") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(3)
                        Toggle {
                            objectName: "captureAudioToggle"
                            checked: Capture.audioEnabled
                            tooltip: qsTr("Record sound")
                            onToggled: Capture.audioEnabled = checked
                        }
                        CaptureChoice {
                            objectName: "captureAudio"
                            Layout.fillWidth: true
                            label: qsTr("Sound input")
                            enabled: Capture.audioEnabled
                            options: [{ id: "", name: qsTr("System default input") }].concat(
                                         Capture.audioDevices.map(d => ({ id: d.id, name: d.name })))
                            current: Capture.audioDevice
                            onChosen: id => Capture.audioDevice = id
                        }
                    }

                    FieldLabel { text: qsTr("Quality") }
                    CaptureChoice {
                        objectName: "capturePreset"
                        Layout.fillWidth: true
                        label: qsTr("Quality")
                        options: Capture.presets.map(p => ({ id: p.id, name: p.label }))
                        current: Capture.preset
                        onChosen: id => Capture.preset = id
                    }
                    Note {
                        objectName: "capturePresetNote"
                        text: {
                            for (let i = 0; i < Capture.presets.length; ++i) {
                                if (Capture.presets[i].id === Capture.preset)
                                    return Capture.presets[i].description
                            }
                            return ""
                        }
                    }

                    FieldLabel { text: qsTr("Clip name") }
                    FieldInput {
                        id: nameField
                        objectName: "captureNameField"
                        Layout.fillWidth: true
                        Accessible.name: qsTr("Clip name")
                        placeholderText: qsTr("Capture 1")
                        maximumLength: 100
                    }

                    FieldLabel { text: qsTr("Add to bin") }
                    CaptureChoice {
                        objectName: "captureBin"
                        Layout.fillWidth: true
                        label: qsTr("Add to bin")
                        options: dialog.binOptions
                        current: dialog.binKey
                        placeholder: qsTr("Project root")
                        onAboutToOpen: dialog.refreshBins()
                        onChosen: id => dialog.binKey = id
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space(2)
                        Text {
                            text: qsTr("Saved in")
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                        Text {
                            objectName: "captureFolderText"
                            Layout.fillWidth: true
                            text: Capture.captureFolder
                            elide: Text.ElideMiddle
                            color: Theme.textSecondary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                            Accessible.name: qsTr("Saved in %1").arg(Capture.captureFolder)
                        }
                    }
                    Note {
                        objectName: "capturePrivacyNote"
                        text: qsTr("Nothing is opened until you press Start preview.")
                    }
                }
            }
        }
    }

    footerData: [
        PillButton {
            objectName: "capturePreviewButton"
            visible: Capture.supported
            variant: dialog.live ? "ghost" : "primary"
            iconName: "camera"
            text: dialog.live ? qsTr("Stop preview") : qsTr("Start preview")
            enabled: dialog.live ? !dialog.capturing && !dialog.busy : dialog.ready && !Capture.loading
            tooltip: dialog.live ? qsTr("Close the device") : qsTr("Open the device and show its picture")
            onClicked: dialog.live ? Capture.stopPreview() : Capture.startPreview()
        },
        PillButton {
            objectName: "captureRecordButton"
            visible: Capture.supported
            variant: dialog.recording ? "primary" : "danger"
            iconName: dialog.recording ? "square" : "circle"
            text: dialog.recording ? qsTr("Stop") : qsTr("Record")
            enabled: dialog.recording || (dialog.phase === "previewing" && Capture.formats.length > 0)
            tooltip: dialog.recording ? qsTr("Stop and add the capture to the project") : qsTr("Start recording to a file")
            onClicked: dialog.recording ? Capture.stop() : Capture.record(nameField.text, dialog.binKey)
        },
        PillButton {
            objectName: "captureClose"
            variant: "ghost"
            text: qsTr("Close")
            enabled: !dialog.capturing && !dialog.busy
            tooltip: dialog.capturing ? qsTr("Stop the capture first") : qsTr("Close the panel (the device is closed)")
            onClicked: dialog.close()
        }
    ]
}
