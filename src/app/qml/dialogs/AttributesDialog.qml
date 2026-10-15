pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Paste Attributes… / Remove Attributes… (C.11, M5m; bridge `Clipboard`), Premiere's dialog: Video Attributes (Motion,
// Opacity, Time Remapping, each video effect) | Audio Attributes (Volume, Channel Volume / Panner, each audio effect),
// Scale Attribute Times (paste) or All effects (remove). Paste takes them from the copied clip of each kind; Remove
// lists the selection's own effects. Under the switches, a live preview of what each selected clip takes (or loses)
// and skips, with the reason (dry runs of clip.pasteAttributes / clip.removeAttributes). OK is one undo step.
WabcutDialog {
    id: dialog
    objectName: "attributesDialog"

    property string mode: "paste"
    readonly property bool pasting: mode === "paste"
    /// Clipboard.attributesInfo(mode)
    property var info: ({})
    /// Clipboard.previewAttributes(mode, choices())
    property var preview: ({})
    property string error: ""
    readonly property bool videoAvailable: info.video !== undefined && info.video.available === true
    readonly property bool audioAvailable: info.audio !== undefined && info.audio.available === true
    readonly property var rows: preview.rows !== undefined ? preview.rows : []
    readonly property int shownRows: 8

    function openFor(which) {
        mode = which === "remove" ? "remove" : "paste"
        error = ""
        info = Clipboard.attributesInfo(mode)
        for (const toggle of [motionToggle, opacityToggle, timeRemapToggle, volumeToggle, pannerToggle])
            toggle.checked = true
        // Premiere: everything is checked, and pasted keyframes stretch to the target's length.
        scaleTimesToggle.checked = true
        allEffectsToggle.checked = false
        open()
        refresh()
    }
    function checkedKeys(repeater) {
        const keys = []
        for (let i = 0; i < repeater.count; ++i) {
            const toggle = repeater.itemAt(i)
            if (toggle && toggle.checked)
                keys.push(toggle.key)
        }
        return keys
    }
    function choices() {
        return {
            motion: videoAvailable && motionToggle.checked,
            opacity: videoAvailable && opacityToggle.checked,
            timeRemap: videoAvailable && timeRemapToggle.checked,
            volume: audioAvailable && volumeToggle.checked,
            panner: audioAvailable && pannerToggle.checked,
            videoEffects: videoAvailable ? checkedKeys(videoEffects) : [],
            audioEffects: audioAvailable ? checkedKeys(audioEffects) : [],
            allEffects: !pasting && allEffectsToggle.checked,
            scaleTimes: pasting && scaleTimesToggle.checked
        }
    }
    function refresh() {
        error = ""
        if (info.available === true)
            preview = Clipboard.previewAttributes(mode, choices())
        else
            preview = { ok: false, error: info.reason !== undefined ? info.reason : "", rows: [] }
    }
    function accept() {
        const failure = Clipboard.applyAttributes(mode, choices())
        if (failure === "")
            dialog.close()
        else
            dialog.error = failure
    }
    function summary() {
        if (info.available !== true)
            return info.reason !== undefined ? info.reason : ""
        const count = info.targets !== undefined ? info.targets : 0
        if (pasting)
            return count === 1 ? qsTr("From %1 onto the selected clip").arg(info.source)
                               : qsTr("From %1 onto %2 selected clips").arg(info.source).arg(count)
        return count === 1 ? qsTr("From the selected clip") : qsTr("From %1 selected clips").arg(count)
    }

    title: pasting ? qsTr("Paste Attributes") : qsTr("Remove Attributes")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(140), Theme.space(140))

    Text {
        objectName: "attributesSummary"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: dialog.summary()
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(6)

        // ---- Video Attributes ----
        ColumnLayout {
            objectName: "videoAttributes"
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(0.5)
            enabled: dialog.videoAvailable

            Text {
                text: qsTr("Video Attributes")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                visible: !dialog.videoAvailable && text !== ""
                wrapMode: Text.Wrap
                text: dialog.info.video !== undefined ? dialog.info.video.reason : ""
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            Toggle {
                id: motionToggle
                objectName: "attrMotion"
                text: qsTr("Motion")
                onToggled: dialog.refresh()
            }
            Toggle {
                id: opacityToggle
                objectName: "attrOpacity"
                text: qsTr("Opacity")
                onToggled: dialog.refresh()
            }
            Toggle {
                id: timeRemapToggle
                objectName: "attrTimeRemap"
                text: qsTr("Time Remapping")
                onToggled: dialog.refresh()
            }
            Repeater {
                id: videoEffects
                objectName: "attrVideoEffects"
                model: dialog.info.video !== undefined ? dialog.info.video.effects : []

                delegate: Toggle {
                    required property var modelData
                    required property int index
                    readonly property string key: modelData.key
                    objectName: "attrVideoEffect" + index
                    text: modelData.name
                    checked: true
                    enabled: !allEffectsToggle.checked
                    onToggled: dialog.refresh()
                }
            }
        }

        // ---- Audio Attributes ----
        ColumnLayout {
            objectName: "audioAttributes"
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.alignment: Qt.AlignTop
            spacing: Theme.space(0.5)
            enabled: dialog.audioAvailable

            Text {
                text: qsTr("Audio Attributes")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                visible: !dialog.audioAvailable && text !== ""
                wrapMode: Text.Wrap
                text: dialog.info.audio !== undefined ? dialog.info.audio.reason : ""
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            Toggle {
                id: volumeToggle
                objectName: "attrVolume"
                text: qsTr("Volume")
                onToggled: dialog.refresh()
            }
            Toggle {
                id: pannerToggle
                objectName: "attrPanner"
                text: qsTr("Channel Volume / Panner")
                onToggled: dialog.refresh()
            }
            Repeater {
                id: audioEffects
                objectName: "attrAudioEffects"
                model: dialog.info.audio !== undefined ? dialog.info.audio.effects : []

                delegate: Toggle {
                    required property var modelData
                    required property int index
                    readonly property string key: modelData.key
                    objectName: "attrAudioEffect" + index
                    text: modelData.name
                    checked: true
                    enabled: !allEffectsToggle.checked
                    onToggled: dialog.refresh()
                }
            }
        }
    }

    Toggle {
        id: scaleTimesToggle
        objectName: "attrScaleTimes"
        visible: dialog.pasting
        enabled: dialog.info.available === true
        text: qsTr("Scale Attribute Times")
        tooltip: qsTr("Keyframes stretch to each clip's length instead of keeping their time from its start")
        onToggled: dialog.refresh()
    }
    Toggle {
        id: allEffectsToggle
        objectName: "attrAllEffects"
        visible: !dialog.pasting
        enabled: dialog.info.available === true
        text: qsTr("All effects")
        onToggled: dialog.refresh()
    }

    // ---- Live preview ----
    Rectangle {
        objectName: "attributesPreview"
        Layout.fillWidth: true
        implicitHeight: previewColumn.implicitHeight + Theme.space(4)
        radius: Theme.radiusCard
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border

        ColumnLayout {
            id: previewColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.space(2)
            spacing: Theme.space(1)

            Text {
                text: qsTr("Preview")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
                font.weight: Font.DemiBold
            }
            Repeater {
                id: previewRows
                objectName: "attributesPreviewRows"
                model: dialog.rows.slice(0, dialog.shownRows)

                delegate: ColumnLayout {
                    id: row
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 0

                    Text {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: row.modelData.name
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: row.modelData.applied.length > 0
                        wrapMode: Text.Wrap
                        text: dialog.pasting ? qsTr("Takes %1").arg(row.modelData.applied.join(", "))
                                             : qsTr("Loses %1").arg(row.modelData.applied.join(", "))
                        color: Theme.success
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: row.modelData.skipped.length > 0
                        wrapMode: Text.Wrap
                        text: qsTr("Skips %1").arg(row.modelData.skipped.join("; "))
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                }
            }
            Text {
                visible: dialog.rows.length > dialog.shownRows
                text: qsTr("and %1 more clips").arg(dialog.rows.length - dialog.shownRows)
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
            Text {
                objectName: "attributesPreviewError"
                Layout.fillWidth: true
                visible: text !== ""
                wrapMode: Text.Wrap
                text: dialog.error !== "" ? dialog.error
                                          : dialog.preview.ok === true || dialog.preview.error === undefined ? "" : dialog.preview.error
                color: dialog.error !== "" ? Theme.danger : Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
    }

    footerData: [
        PillButton {
            objectName: "attributesCancel"
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "attributesOk"
            variant: "primary"
            text: qsTr("OK")
            enabled: dialog.preview.ok === true
            onClicked: dialog.accept()
        }
    ]
}
