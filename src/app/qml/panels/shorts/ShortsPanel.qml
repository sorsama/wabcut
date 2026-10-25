pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// ● SHORTS (AI-9): the best 20-90 s moments of the current sequence as vertical shorts. Find scores the transcript on
// this computer (optionally re-ranked by the user's cloud model, with consent); each card shows its frames, score
// breakdown and an editable title. Create / Create all stage new 9:16 sequences (reframed, captioned, with the title
// as a hook); Apply makes them one undo step, then Export all queues 1080x1920 exports. Laid out for the narrow
// browser slot: controls stack, options wrap.
// 0.4: the captions' style beside the Captions switch (a caption preset, "Shorts" by default: big outlined words above
// the platforms' buttons; remembered in Preferences.shortsCaptionPreset).
Panel {
    id: root

    title: qsTr("Shorts")

    property var selected: ({})

    function focusContent() {
        queryField.forceActiveFocus(Qt.TabFocusReason)
    }
    function chosen() {
        const out = []
        for (let i = 0; i < Shorts.candidates.length; ++i) {
            if (root.selected[i] !== false)
                out.push(i)
        }
        return out
    }
    function createOptions() {
        const o = { aspect: "9:16", mode: "speaker", captions: captionsToggle.checked, hookTitle: hookToggle.checked,
                    cleanup: cleanupToggle.checked, normalize: normalizeToggle.checked }
        if (captionsToggle.checked)
            o.captionPreset = Preferences.shortsCaptionPreset
        return o
    }
    /// The caption presets for the style menu: "Shorts" first, the other built-in ones, then the project's.
    function captionPresetOptions(): var {
        const names = Captions.presets.map(p => String(p.name))
        return names.filter(n => n === "Shorts").concat(names.filter(n => n !== "Shorts")).map(n => ({ id: n, name: n }))
    }

    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)
        clip: true

        // ---- Find ----
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.controlHeightSmall
            radius: Theme.radiusField
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: queryField.activeFocus ? Theme.accentStroke : Theme.border
            TextInput {
                id: queryField
                objectName: "shortsQueryField"
                anchors.fill: parent
                anchors.leftMargin: Theme.space(2)
                anchors.rightMargin: Theme.space(2)
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                clip: true
                selectByMouse: true
                Accessible.name: qsTr("About")
                onAccepted: findButton.clicked()
                Text {
                    anchors.fill: parent
                    verticalAlignment: Text.AlignVCenter
                    visible: queryField.text === "" && !queryField.activeFocus
                    text: qsTr("About… (optional)")
                    color: Theme.textTertiary
                    font: queryField.font
                    elide: Text.ElideRight
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            PillTabs {
                id: countTabs
                objectName: "shortsCountTabs"
                Layout.fillWidth: true
                compact: true
                model: [qsTr("3"), qsTr("5"), qsTr("10")]
                currentIndex: 1
            }
            PillButton {
                id: findButton
                objectName: "shortsFindButton"
                variant: "primary"
                iconName: "search"
                text: qsTr("Find")
                enabled: !Shorts.busy
                onClicked: {
                    root.selected = ({})
                    Shorts.find([3, 5, 10][countTabs.currentIndex], queryField.text, rerankToggle.checked)
                }
            }
        }
        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Toggle {
                id: rerankToggle
                objectName: "shortsRerankToggle"
                text: qsTr("Re-rank with my AI")
                enabled: Shorts.cloudAvailable
                tooltip: Shorts.cloudAvailable ? qsTr("Sends the candidates' transcript text (no media) to your chat model; asks for consent first")
                                               : qsTr("No cloud chat model, or this project is no-cloud")
            }
            Toggle { id: captionsToggle; objectName: "shortsCaptionsToggle"; text: qsTr("Captions"); checked: true }
            PillButton {
                id: captionStyleButton
                objectName: "shortsCaptionStyle"
                visible: captionsToggle.checked
                compact: true
                iconName: "chevron-down"
                text: qsTr("Style: %1").arg(Preferences.shortsCaptionPreset)
                tooltip: qsTr("The look of the shorts' captions: a caption preset")
                Accessible.name: qsTr("Caption style: %1").arg(Preferences.shortsCaptionPreset)
                onClicked: {
                    captionStyleMenu.options = root.captionPresetOptions()
                    captionStyleMenu.popup(captionStyleButton, 0, captionStyleButton.height + Theme.space(1))
                }
                OptionsMenu {
                    id: captionStyleMenu
                    objectName: "shortsCaptionStyleMenu"
                    current: Preferences.shortsCaptionPreset
                    onChosen: id => Preferences.shortsCaptionPreset = id
                }
            }
            Toggle { id: hookToggle; objectName: "shortsHookToggle"; text: qsTr("Hook title"); checked: true }
            Toggle { id: cleanupToggle; objectName: "shortsCleanupToggle"; text: qsTr("Remove fillers") }
            Toggle { id: normalizeToggle; objectName: "shortsNormalizeToggle"; text: qsTr("-14 LUFS") }
        }

        // ---- The job: a pill and the UV beam ----
        ColumnLayout {
            Layout.fillWidth: true
            visible: Shorts.busy
            spacing: Theme.space(1)
            RowLayout {
                Layout.fillWidth: true
                StatusPill {
                    objectName: "shortsJobPill"
                    Layout.fillWidth: true
                    label: Shorts.phase
                    detail: Shorts.progress >= 0 ? Math.round(Shorts.progress * 100) + "%" : ""
                    dotColor: Theme.accentFill
                }
                PillButton {
                    compact: true
                    text: qsTr("Cancel")
                    onClicked: Shorts.cancel()
                }
            }
            BeamProgress {
                Layout.fillWidth: true
                value: Shorts.progress >= 0 ? Shorts.progress : 1
                active: Shorts.busy
                label: Shorts.phase
            }
        }
        Note {
            objectName: "shortsMessage"
            visible: Shorts.message !== ""
            text: Shorts.message
            maximumLineCount: 4
            elide: Text.ElideRight
        }

        // ---- Staged shorts ----
        Rectangle {
            objectName: "shortsPendingBar"
            Layout.fillWidth: true
            visible: Shorts.pendingCount > 0 || Shorts.createdCount > 0
            implicitHeight: pendingColumn.implicitHeight + Theme.space(3)
            radius: Theme.radiusField
            color: Theme.accentWash
            border.width: Theme.hairline
            border.color: Theme.accentStroke
            ColumnLayout {
                id: pendingColumn
                anchors.fill: parent
                anchors.margins: Theme.space(1.5)
                spacing: Theme.space(1)
                Text {
                    Layout.fillWidth: true
                    text: Shorts.pendingCount > 0 ? qsTr("%n short(s) ready: %1", "", Shorts.pendingCount).arg(Shorts.pendingNames.join(", "))
                                                  : qsTr("%n short(s) made", "", Shorts.createdCount)
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    Item { Layout.fillWidth: true }
                    PillButton {
                        visible: Shorts.pendingCount > 0
                        compact: true
                        text: qsTr("Discard")
                        onClicked: Shorts.discard()
                    }
                    PillButton {
                        objectName: "shortsApplyButton"
                        visible: Shorts.pendingCount > 0
                        compact: true
                        variant: "primary"
                        text: qsTr("Apply")
                        onClicked: Shorts.apply()
                    }
                    Toggle {
                        id: hevcToggle
                        visible: Shorts.pendingCount === 0 && Shorts.createdCount > 0
                        text: qsTr("HEVC")
                    }
                    PillButton {
                        objectName: "shortsExportButton"
                        visible: Shorts.pendingCount === 0 && Shorts.createdCount > 0
                        compact: true
                        iconName: "upload"
                        text: qsTr("Export all")
                        onClicked: Shorts.exportAll(hevcToggle.checked)
                    }
                }
            }
        }

        // ---- Candidates ----
        ListView {
            id: list
            objectName: "shortsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Theme.space(2)
            model: Shorts.candidates
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}
            delegate: ShortCard {
                width: list.width
                selected: root.selected[index] !== false
                onSelectedChanged: {
                    const s = Object.assign({}, root.selected)
                    s[index] = selected
                    root.selected = s
                }
                onTitleEdited: title => Shorts.setTitle(index, title)
                onCreateRequested: Shorts.create([index], root.createOptions())
            }
            EmptyState {
                anchors.centerIn: parent
                width: parent.width - Theme.space(4)
                visible: list.count === 0 && !Shorts.busy && Shorts.message === ""
                illustration: "transcript"
                text: qsTr("Find the moments of this sequence that work as vertical shorts. Its media need transcripts.")
                actionText: qsTr("Find shorts")
                actionIcon: "search"
                onActionTriggered: findButton.clicked()
            }
        }

        PillButton {
            objectName: "shortsCreateAllButton"
            Layout.fillWidth: true
            visible: list.count > 0
            variant: "primary"
            iconName: "clapperboard"
            text: qsTr("Create %n short(s)", "", root.chosen().length)
            enabled: !Shorts.busy && root.chosen().length > 0
            onClicked: Shorts.create(root.chosen(), root.createOptions())
        }
    }
}
