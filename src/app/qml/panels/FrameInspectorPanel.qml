pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Explain this frame (M5k, bridge `FrameInspector`): why the program monitor shows what it shows at the playhead and
// why the speakers play what they play, read from the same evaluators preview and export run.
//
//   Picture, top layer first: the source (original or proxy, the file), the exact source frame and timecode, the
//   timing (speed, reverse, freeze, the time-remap segment), a transition in progress, motion and opacity, the effect
//   stack (applied, off, unsupported), the grade's stages, masks and redactions; a layer an opaque one covers says so.
//   Sound, A1 first: the source sample, the gain chain (clip volume and pan, track, master), clip and track effects,
//   bleeps and mutes, and why a clip is silent. Captions: the cue and where its timing comes from.
//
// "Solo" and "Bypass" are preview-only overrides: the monitor and the speakers change, the project does not (no undo
// step, nothing to save). The banner counts them and clears them.
Panel {
    id: root
    objectName: "frameInspector"

    signal closeRequested()

    title: qsTr("Explain this frame")

    // Evaluate only while on screen.
    Binding {
        target: FrameInspector
        property: "active"
        value: root.visible && root.enabled
    }

    trailing: [
        IconButton {
            objectName: "frameInspectorClose"
            compact: true
            iconName: "x"
            tooltip: qsTr("Back to Effect Controls")
            onClicked: root.closeRequested()
        }
    ]

    component Caption: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        font.weight: Font.Medium
    }
    component Line: Text {
        width: parent ? parent.width : 0
        wrapMode: Text.WordWrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    component Mono: Text {
        color: Theme.textPrimary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textSm
    }
    // One effect: its name, what the evaluator does with it, and the preview-only bypass.
    component EffectRow: Item {
        id: effectRow
        required property var modelData
        width: parent ? parent.width : 0
        height: Theme.rowHeight
        readonly property bool bypassable: modelData.state === "applied" || modelData.state === "previewBypassed"
        Rectangle {
            x: 0
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.space(1.5)
            height: Theme.space(1.5)
            radius: Theme.radiusBadge
            color: effectRow.modelData.state === "applied" ? Theme.success
                   : effectRow.modelData.state === "previewBypassed" ? Theme.warning : Theme.textDisabled
        }
        Text {
            x: Theme.space(3)
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - x - bypass.width - Theme.space(2)
            // 0.2: Border, Ultra Key and Track Matte Key say what they do at this frame (detail).
            readonly property string base: effectRow.modelData.masks !== ""
                  ? qsTr("%1 · %2 · masks: %3").arg(effectRow.modelData.name).arg(effectRow.modelData.stateText).arg(effectRow.modelData.masks)
                  : qsTr("%1 · %2").arg(effectRow.modelData.name).arg(effectRow.modelData.stateText)
            text: (effectRow.modelData.detail ?? "") !== "" ? qsTr("%1 · %2").arg(base).arg(effectRow.modelData.detail) : base
            elide: Text.ElideRight
            color: effectRow.modelData.state === "applied" ? Theme.textPrimary : Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        IconButton {
            id: bypass
            objectName: "frameInspectorBypass_" + effectRow.modelData.owner + "_" + effectRow.modelData.key
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            compact: true
            visible: effectRow.bypassable
            checkable: true
            checked: effectRow.modelData.state === "previewBypassed"
            checkedColor: Theme.warning
            iconName: "power"
            tooltip: checked ? qsTr("Restore this effect in the preview") : qsTr("Bypass this effect in the preview only")
            onClicked: FrameInspector.toggleBypass(effectRow.modelData.owner, effectRow.modelData.key)
        }
    }

    // Preview-only overrides are active.
    Rectangle {
        id: banner
        objectName: "frameInspectorPreviewBanner"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: visible ? Theme.controlHeight : 0
        visible: FrameInspector.previewing
        radius: Theme.radiusField
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.warning
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.space(3)
            anchors.right: clearButton.left
            anchors.verticalCenter: parent.verticalCenter
            elide: Text.ElideRight
            //: Explain this frame: preview-only overrides, e.g. "Preview only: 2 overrides. The project is unchanged."
            text: qsTr("Preview only: %n override(s). The project is unchanged.", "", FrameInspector.overrideCount)
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        PillButton {
            id: clearButton
            objectName: "frameInspectorClear"
            anchors.right: parent.right
            anchors.rightMargin: Theme.space(1)
            anchors.verticalCenter: parent.verticalCenter
            compact: true
            variant: "ghost"
            text: qsTr("Clear")
            onClicked: FrameInspector.clearOverrides()
        }
    }

    Text {
        id: timecode
        objectName: "frameInspectorTimecode"
        anchors.left: parent.left
        anchors.top: banner.bottom
        anchors.topMargin: banner.visible ? Theme.space(2) : 0
        text: qsTr("At %1").arg(FrameInspector.timecode)
        color: Theme.textSecondary
        font.family: Theme.fontMono
        font.pixelSize: Theme.textSm
    }

    Flickable {
        id: scroll
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: timecode.bottom
        anchors.topMargin: Theme.space(2)
        anchors.bottom: parent.bottom
        contentHeight: content.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        Column {
            id: content
            width: scroll.width - Theme.space(2)
            spacing: Theme.space(3)

            Caption {
                text: qsTr("Picture, top layer first")
                visible: FrameInspector.layers.length > 0
            }
            Repeater {
                model: FrameInspector.layers
                delegate: Rectangle {
                    id: layer
                    required property var modelData
                    objectName: "frameInspectorLayer_" + modelData.key
                    width: content.width
                    height: layerColumn.height + Theme.space(4)
                    radius: Theme.radiusCard
                    color: Theme.raised
                    border.width: modelData.soloed ? Theme.clipSelectionWidth : Theme.hairline
                    border.color: modelData.soloed ? Theme.accentStroke : Theme.border
                    opacity: modelData.occluded ? 0.7 : 1

                    Column {
                        id: layerColumn
                        x: Theme.space(3)
                        y: Theme.space(2)
                        width: parent.width - Theme.space(6)
                        spacing: Theme.space(1)

                        Item {
                            width: parent.width
                            height: Theme.controlHeightSmall
                            Rectangle {
                                id: trackChip
                                anchors.verticalCenter: parent.verticalCenter
                                width: trackText.implicitWidth + Theme.space(2)
                                height: trackText.implicitHeight + Theme.space(1)
                                radius: Theme.radiusBadge
                                color: Theme.sunken
                                Text {
                                    id: trackText
                                    anchors.centerIn: parent
                                    text: layer.modelData.track
                                    color: Theme.textSecondary
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.textXs
                                }
                            }
                            Text {
                                anchors.left: trackChip.right
                                anchors.leftMargin: Theme.space(2)
                                anchors.right: soloButton.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: layer.modelData.clipName
                                elide: Text.ElideRight
                                color: Theme.textPrimary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: Font.DemiBold
                            }
                            IconButton {
                                id: soloButton
                                objectName: "frameInspectorSolo_" + layer.modelData.key
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                compact: true
                                checkable: true
                                checked: layer.modelData.soloed
                                iconName: "eye"
                                tooltip: checked ? qsTr("Show every layer again") : qsTr("Solo this layer in the preview only")
                                onClicked: FrameInspector.toggleSolo(layer.modelData.key)
                            }
                        }
                        Line {
                            text: layer.modelData.sourceFile !== ""
                                  ? qsTr("%1 · %2").arg(layer.modelData.sourceText).arg(layer.modelData.sourceFile)
                                  : layer.modelData.sourceText
                            color: layer.modelData.source === "original" ? Theme.textSecondary : Theme.warning
                        }
                        Row {
                            spacing: Theme.space(2)
                            Mono {
                                objectName: "frameInspectorSourceTimecode"
                                text: layer.modelData.sourceTimecode
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: layer.modelData.sourceFrame
                                color: Theme.textTertiary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textXs
                            }
                        }
                        Line {
                            text: layer.modelData.mapping
                            color: layer.modelData.remapped ? Theme.accentStroke : Theme.textSecondary
                        }
                        Line {
                            visible: layer.modelData.transition !== ""
                            text: qsTr("Transition: %1").arg(layer.modelData.transition)
                        }
                        Line {
                            text: qsTr("Opacity %1 · %2").arg(layer.modelData.opacityText).arg(layer.modelData.motionText)
                            color: Theme.textTertiary
                        }
                        // 0.2: the blend mode, and a layer that is another layer's matte (not drawn itself).
                        Line {
                            visible: layer.modelData.blendText !== ""
                            text: qsTr("Blend mode: %1").arg(layer.modelData.blendText)
                        }
                        Line {
                            objectName: "frameInspectorMatte_" + layer.modelData.key
                            visible: layer.modelData.matteText !== ""
                            text: qsTr("%1: not drawn itself, it shapes that layer").arg(layer.modelData.matteText)
                            color: Theme.accentStroke
                        }
                        Line {
                            visible: layer.modelData.gradeText !== ""
                            text: qsTr("Grade: %1").arg(layer.modelData.gradeText)
                        }
                        Line {
                            visible: layer.modelData.redactions !== ""
                            text: qsTr("Redacted: %1").arg(layer.modelData.redactions)
                            color: Theme.warning
                        }
                        Line {
                            visible: layer.modelData.occluded
                            text: qsTr("Covered by an opaque layer above: not decoded.")
                            color: Theme.textTertiary
                        }
                        Repeater {
                            model: layer.modelData.effects
                            delegate: EffectRow {}
                        }
                    }
                }
            }

            Caption {
                text: qsTr("Sound")
                visible: FrameInspector.audio.length > 0
            }
            Repeater {
                model: FrameInspector.audio
                delegate: Rectangle {
                    id: voice
                    required property var modelData
                    objectName: "frameInspectorAudio_" + modelData.key
                    width: content.width
                    height: voiceColumn.height + Theme.space(4)
                    radius: Theme.radiusCard
                    color: Theme.raised
                    border.width: modelData.soloed ? Theme.clipSelectionWidth : Theme.hairline
                    border.color: modelData.soloed ? Theme.accentStroke : Theme.border

                    Column {
                        id: voiceColumn
                        x: Theme.space(3)
                        y: Theme.space(2)
                        width: parent.width - Theme.space(6)
                        spacing: Theme.space(1)

                        Item {
                            width: parent.width
                            height: Theme.controlHeightSmall
                            Text {
                                anchors.left: parent.left
                                anchors.right: audioSolo.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("%1 · %2").arg(voice.modelData.track).arg(voice.modelData.clipName)
                                elide: Text.ElideRight
                                color: Theme.textPrimary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: Font.DemiBold
                            }
                            IconButton {
                                id: audioSolo
                                objectName: "frameInspectorSolo_" + voice.modelData.key
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                compact: true
                                checkable: true
                                checked: voice.modelData.soloed
                                iconName: "headphones"
                                tooltip: checked ? qsTr("Hear every clip again") : qsTr("Solo this clip in the preview only")
                                onClicked: FrameInspector.toggleSolo(voice.modelData.key)
                            }
                        }
                        Row {
                            spacing: Theme.space(2)
                            Mono {
                                text: voice.modelData.sourceTimecode
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: voice.modelData.sampleText
                                color: Theme.textTertiary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textXs
                            }
                        }
                        Line {
                            visible: voice.modelData.silentReason !== ""
                            text: voice.modelData.silentReason
                            color: Theme.warning
                        }
                        Repeater {
                            model: voice.modelData.gain
                            delegate: Item {
                                id: gainRow
                                required property var modelData
                                width: voiceColumn.width
                                height: Theme.space(5)
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: gainRow.modelData.stage
                                    color: Theme.textTertiary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textSm
                                }
                                Text {
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.min(implicitWidth, parent.width * 0.6)
                                    horizontalAlignment: Text.AlignRight
                                    elide: Text.ElideLeft
                                    text: gainRow.modelData.value
                                    color: Theme.textPrimary
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.textSm
                                }
                            }
                        }
                        Line {
                            visible: voice.modelData.transition !== ""
                            text: qsTr("Transition: %1").arg(voice.modelData.transition)
                        }
                        Line {
                            visible: voice.modelData.redactions !== ""
                            text: qsTr("Redacted: %1").arg(voice.modelData.redactions)
                            color: Theme.warning
                        }
                        Repeater {
                            model: voice.modelData.effects
                            delegate: EffectRow {}
                        }
                    }
                }
            }
            Caption {
                text: qsTr("Master")
                visible: FrameInspector.masterEffects.length > 0
            }
            Repeater {
                model: FrameInspector.masterEffects
                delegate: EffectRow {}
            }

            Caption {
                text: qsTr("Captions")
                visible: FrameInspector.caption.text !== undefined
            }
            Rectangle {
                objectName: "frameInspectorCaption"
                visible: FrameInspector.caption.text !== undefined
                width: content.width
                height: visible ? captionColumn.height + Theme.space(4) : 0
                radius: Theme.radiusCard
                color: Theme.raised
                border.width: Theme.hairline
                border.color: Theme.border
                Column {
                    id: captionColumn
                    x: Theme.space(3)
                    y: Theme.space(2)
                    width: parent.width - Theme.space(6)
                    spacing: Theme.space(1)
                    Line {
                        text: FrameInspector.caption.text ?? ""
                        color: Theme.textPrimary
                    }
                    Line {
                        text: qsTr("%1 · %2 – %3").arg(FrameInspector.caption.track ?? "").arg(FrameInspector.caption.inText ?? "")
                                                  .arg(FrameInspector.caption.outText ?? "")
                        color: Theme.textTertiary
                    }
                    Line {
                        text: FrameInspector.caption.timing ?? ""
                    }
                }
            }

            EmptyState {
                width: content.width
                visible: FrameInspector.layers.length === 0 && FrameInspector.audio.length === 0
                illustration: "timeline"
                text: qsTr("Nothing plays at the playhead. Move it over a clip to see why each frame looks the way it does.")
            }
        }
    }
}
