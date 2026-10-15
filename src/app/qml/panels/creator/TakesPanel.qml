pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Takes (AI-11): lines recorded more than once in the current sequence (and, optionally, in every recording), grouped.
// Each group lists its takes best first: the words with the difference from the best take (added words underlined,
// missing ones struck through), quality chips (fillers, false starts, pauses, clipping, level over the noise floor) and
// pace. Play auditions a take in place; Use stages "use this take" (drawn on the timeline as a ghost); Keep best takes
// stages every group at once with a review list. Apply makes it one undo step.
Panel {
    id: root
    objectName: "takesPanel"

    title: qsTr("Takes")

    function focusContent() {
        findButton.forceActiveFocus(Qt.TabFocusReason)
    }

    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    component Chip: Rectangle {
        property alias text: chipText.text
        property color tone: Theme.textTertiary
        implicitWidth: chipText.implicitWidth + Theme.space(2)
        implicitHeight: chipText.implicitHeight + Theme.space(0.5)
        radius: Theme.radiusPill
        color: Qt.alpha(tone, 0.14)
        Text {
            id: chipText
            anchors.centerIn: parent
            color: parent.tone
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
    }

    // Stops an audition at the end of its take.
    Timer {
        id: auditionStop
        repeat: false
        property bool source: false
        onTriggered: source ? SourceMonitor.pause() : Playback.pause()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)
        clip: true

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Toggle {
                id: allMediaToggle
                objectName: "takesAllMedia"
                Layout.fillWidth: true
                text: qsTr("Also from all recordings")
                tooltip: qsTr("Look for takes in every transcribed recording, not only the ones this sequence uses")
            }
            PillButton {
                id: findButton
                objectName: "takesFind"
                variant: "primary"
                iconName: "search"
                text: qsTr("Find takes")
                enabled: !Takes.busy
                onClicked: Takes.find({ allMedia: allMediaToggle.checked })
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: Takes.busy
            spacing: Theme.space(1)
            StatusPill {
                Layout.fillWidth: true
                label: Takes.phase
                detail: Takes.progress >= 0 ? Math.round(Takes.progress * 100) + "%" : ""
                dotColor: Theme.accentFill
            }
            BeamProgress {
                Layout.fillWidth: true
                value: Takes.progress >= 0 ? Takes.progress : 1
                active: Takes.busy
                label: Takes.phase
            }
        }
        Note {
            objectName: "takesMessage"
            visible: Takes.message !== ""
            text: Takes.message
            maximumLineCount: 4
            elide: Text.ElideRight
        }

        // ---- The staged edit and its review list ----
        Rectangle {
            objectName: "takesStagedBar"
            Layout.fillWidth: true
            visible: Takes.staged
            implicitHeight: stagedColumn.implicitHeight + Theme.space(3)
            radius: Theme.radiusField
            color: Theme.accentWash
            border.width: Theme.hairline
            border.color: Theme.accentStroke
            ColumnLayout {
                id: stagedColumn
                anchors.fill: parent
                anchors.margins: Theme.space(1.5)
                spacing: Theme.space(1)
                Repeater {
                    model: Takes.review
                    delegate: Text {
                        required property var modelData
                        Layout.fillWidth: true
                        text: (modelData.replaced ? qsTr("Use the take from %1: “%2”").arg(modelData.source).arg(modelData.text)
                                                  : qsTr("Keep take %1: “%2”").arg(modelData.take).arg(modelData.text))
                              + (modelData.removed > 0 ? " · " + qsTr("%n other take(s) removed", "", modelData.removed) : "")
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    PillButton { compact: true; text: qsTr("Discard"); onClicked: Takes.discard() }
                    PillButton {
                        objectName: "takesApply"
                        compact: true
                        variant: "primary"
                        text: qsTr("Apply")
                        onClicked: Takes.apply()
                    }
                }
            }
        }

        // ---- The groups ----
        ListView {
            id: list
            objectName: "takesList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Theme.space(2)
            model: Takes.groups
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}
            delegate: Rectangle {
                id: groupCard
                required property var modelData
                required property int index
                width: list.width
                implicitHeight: groupColumn.implicitHeight + Theme.space(3)
                radius: Theme.radiusCard
                color: Theme.raised
                border.width: Theme.hairline
                border.color: Theme.border
                ColumnLayout {
                    id: groupColumn
                    anchors.fill: parent
                    anchors.margins: Theme.space(1.5)
                    spacing: Theme.space(1.5)
                    RowLayout {
                        Layout.fillWidth: true
                        SliceMark {}
                        Text {
                            Layout.fillWidth: true
                            text: groupCard.modelData.text
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }
                        Text {
                            text: qsTr("%n take(s)", "", (groupCard.modelData.takes ?? []).length)
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                    }
                    Repeater {
                        model: groupCard.modelData.takes ?? []
                        delegate: Rectangle {
                            id: takeRow
                            required property var modelData
                            required property int index
                            objectName: "takesTake_" + groupCard.modelData.group + "_" + modelData.take
                            Layout.fillWidth: true
                            implicitHeight: takeColumn.implicitHeight + Theme.space(1.5)
                            radius: Theme.radiusField
                            color: modelData.best ? Theme.accentWash : Theme.panel
                            border.width: modelData.best ? 1.5 : Theme.hairline
                            border.color: modelData.best ? Theme.accentStroke : Theme.border
                            ColumnLayout {
                                id: takeColumn
                                anchors.fill: parent
                                anchors.margins: Theme.space(0.75)
                                spacing: Theme.space(0.75)
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.space(1)
                                    Text {
                                        text: takeRow.modelData.best ? qsTr("Best") : qsTr("Take %1").arg(takeRow.modelData.take)
                                        color: takeRow.modelData.best ? Theme.accentStroke : Theme.textSecondary
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.textSm
                                        font.weight: Font.DemiBold
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: (takeRow.modelData.onTimeline ? qsTr("in the sequence") : takeRow.modelData.source) + " · " + takeRow.modelData.range
                                        color: Theme.textTertiary
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.textXs
                                        elide: Text.ElideRight
                                    }
                                    IconButton {
                                        objectName: "takesPlay_" + groupCard.modelData.group + "_" + takeRow.modelData.take
                                        iconName: "play"
                                        tooltip: qsTr("Play this take")
                                        onClicked: {
                                            const seconds = Takes.audition(groupCard.modelData.group, takeRow.modelData.take)
                                            auditionStop.source = !takeRow.modelData.onTimeline
                                            if (auditionStop.source) SourceMonitor.play(); else Playback.play()
                                            auditionStop.interval = Math.max(200, seconds * 1000)
                                            auditionStop.restart()
                                        }
                                    }
                                    PillButton {
                                        objectName: "takesUse_" + groupCard.modelData.group + "_" + takeRow.modelData.take
                                        compact: true
                                        text: qsTr("Use this take")
                                        enabled: !Takes.busy && groupCard.modelData.onTimeline > 0
                                        onClicked: Takes.use(groupCard.modelData.group, takeRow.modelData.take)
                                    }
                                }
                                // The words, with the difference from the best take.
                                Text {
                                    Layout.fillWidth: true
                                    textFormat: Text.StyledText
                                    wrapMode: Text.Wrap
                                    color: Theme.textPrimary
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textSm
                                    text: {
                                        const diff = takeRow.modelData.diff ?? []
                                        if (diff.length === 0)
                                            return takeRow.modelData.text
                                        const escape = s => s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
                                        return diff.map(d => d.op === "insert" ? "<u>" + escape(d.text) + "</u>"
                                                           : d.op === "delete" ? "<s><font color=\"" + Theme.textTertiary + "\">" + escape(d.text) + "</font></s>"
                                                                                : escape(d.text)).join(" ")
                                    }
                                }
                                Flow {
                                    Layout.fillWidth: true
                                    spacing: Theme.space(1)
                                    Chip {
                                        visible: takeRow.modelData.fillers > 0
                                        text: qsTr("%n filler(s)", "", takeRow.modelData.fillers)
                                        tone: Theme.warning
                                    }
                                    Chip {
                                        visible: takeRow.modelData.falseStarts > 0
                                        text: qsTr("%n false start(s)", "", takeRow.modelData.falseStarts)
                                        tone: Theme.warning
                                    }
                                    Chip {
                                        visible: takeRow.modelData.hesitations > 0
                                        text: qsTr("%n long pause(s)", "", takeRow.modelData.hesitations)
                                        tone: Theme.warning
                                    }
                                    Chip { visible: takeRow.modelData.clipped === true; text: qsTr("Clipped"); tone: Theme.danger }
                                    Chip { visible: takeRow.modelData.complete === false; text: qsTr("Incomplete"); tone: Theme.warning }
                                    Chip {
                                        visible: takeRow.modelData.levelDb !== undefined && takeRow.modelData.noiseDb !== undefined
                                        text: qsTr("%1 dB over noise").arg(Math.round((takeRow.modelData.levelDb ?? 0) - (takeRow.modelData.noiseDb ?? 0)))
                                    }
                                    Chip { text: qsTr("%1 words/s").arg((takeRow.modelData.wordsPerSecond ?? 0).toFixed(1)) }
                                    Chip { text: qsTr("Score %1").arg(Math.round((takeRow.modelData.score ?? 0) * 100)); tone: Theme.accentStroke }
                                }
                            }
                        }
                    }
                }
            }
            EmptyState {
                anchors.centerIn: parent
                width: parent.width - Theme.space(4)
                visible: list.count === 0 && !Takes.busy && Takes.message === ""
                illustration: "transcript"
                text: qsTr("Find lines you recorded more than once, compare the takes word by word, and keep the best.")
                actionText: qsTr("Find takes")
                actionIcon: "search"
                onActionTriggered: findButton.clicked()
            }
        }

        PillButton {
            objectName: "takesKeepBest"
            Layout.fillWidth: true
            visible: list.count > 0
            variant: "primary"
            iconName: "sparkles"
            text: qsTr("Keep best takes")
            enabled: !Takes.busy
            onClicked: Takes.useBest()
        }
    }
}
