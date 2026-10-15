pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// The Transcript tab: the sequence's words (Transcript.paragraphs) by speaker and time.
//   click a word        the playhead goes there; the word is selected (Shift extends, Ctrl/Cmd toggles)
//   drag over words     selects them; the timeline lights their range
//   Delete / Keep only  preview first: struck-through words and a red ghost on the timeline; Remove applies it (one
//                       undo step), Cancel / Esc drops it
//   paragraph grip      drag a paragraph before another one (transcript.moveRange)
//   find                every occurrence is marked; ↑/↓ jump; Select all selects them
// The word under the playhead is filled with the accent. Thai and other unspaced languages sit word unit by word unit
// with no spaces (the model's word boundaries), so lines only break between units.
// Without a transcript: choose a transcriber (this computer or a cloud model from Preferences > AI, with the consent
// prompt before the first upload) and transcribe the sequence or the selected clips, with progress and Cancel.
FocusScope {
    id: root
    objectName: "transcriptPage"

    signal setupRequested()

    readonly property bool hasWords: Transcript.wordCount > 0

    function start(scope) {
        const prompt = Transcript.consentFor(scope)
        if (prompt.needed) {
            consent.scope = scope
            consent.ask(prompt)
        } else {
            Transcript.transcribe(scope)
        }
    }

    // One word of the transcript: its index, its marks (selected, to be removed, a search match) and the playhead.
    component WordText: Text {
        id: word
        required property var modelData
        readonly property int wordIndex: modelData.i
        readonly property int marks: Transcript.markRevision
        readonly property bool selected: marks >= 0 && Transcript.isSelected(wordIndex)
        readonly property bool removing: marks >= 0 && Transcript.isRemoving(wordIndex)
        readonly property bool match: marks >= 0 && Transcript.isMatch(wordIndex)
        readonly property bool current: Transcript.currentWord === wordIndex

        text: modelData.text
        color: word.removing ? Theme.danger : word.current ? Theme.accentOnFill : Theme.textPrimary
        opacity: word.removing ? 0.75 : 1
        font.family: Theme.fontSans
        font.pixelSize: Theme.textLg
        font.strikeout: word.removing
        lineHeight: 1.25

        // A word to be removed shows only that (one red band over the range); then the current word, the selection
        // and search matches.
        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.topMargin: 1
            anchors.bottomMargin: 1
            radius: Theme.radiusBadge
            color: word.removing ? Qt.alpha(Theme.danger, 0.14)
                 : word.current ? Theme.accentFill
                 : word.selected ? Theme.accentWash
                 : word.match ? Qt.alpha(Theme.warning, 0.28) : "transparent"
            border.width: word.selected && !word.current && !word.removing ? Theme.hairline : 0
            border.color: Theme.accentStroke
        }
    }

    // The word index under a point of an item (-1 between words): down the children under the point to a WordText.
    function wordUnder(item: Item, x: real, y: real): int {
        let target = item
        let point = Qt.point(x, y)
        while (target) {
            const word = target as WordText
            if (word)
                return word.wordIndex
            const child = target.childAt(point.x, point.y)
            if (!child)
                return -1
            point = target.mapToItem(child, point.x, point.y)
            target = child
        }
        return -1
    }

    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.labelTracking
    }

    // Progress of a transcription (also shown above the words when more clips are transcribed).
    component JobRow: ColumnLayout {
        visible: Transcript.jobStatus !== "idle"
        spacing: Theme.space(1)

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: Transcript.jobText
                elide: Text.ElideRight
                color: Transcript.jobStatus === "error" ? Theme.warning : Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            PillButton {
                visible: Transcript.jobStatus === "running"
                compact: true
                text: qsTr("Cancel")
                onClicked: Transcript.cancelTranscription()
            }
        }
        BeamProgress {
            Layout.fillWidth: true
            visible: Transcript.jobStatus === "running"
            value: Transcript.jobProgress
            active: Transcript.jobStatus === "running"
            label: qsTr("Transcription")
            loader: "equalizer"
        }
    }

    // ---- No transcript yet: transcribe ----
    Flickable {
        id: setup
        objectName: "transcribeSetup"
        anchors.fill: parent
        visible: !root.hasWords
        clip: true
        contentHeight: setupColumn.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: false
        ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: setupColumn
            width: setup.width
            spacing: Theme.space(3)

            SvgArt {
                objectName: "transcriptEmptyIllustration"
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Theme.space(2)
                Layout.preferredWidth: Theme.space(32)
                Layout.preferredHeight: Theme.space(24)
                source: "illustrations/empty-transcript.svg"
                tint: Theme.textTertiary
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("No transcript yet")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXl
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: qsTr("Transcribe the sequence, then edit the video by editing its text: delete words, keep the best lines, move paragraphs.")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
            }

            GroupLabel { text: qsTr("Transcribe with") }
            Repeater {
                model: Transcript.providers
                delegate: AbstractButton {
                    id: providerRow
                    required property var modelData
                    readonly property bool chosen: Transcript.provider === modelData.id

                    objectName: "provider_" + modelData.id
                    Layout.fillWidth: true
                    implicitHeight: Theme.controlHeight + Theme.space(3)
                    enabled: modelData.available
                    focusPolicy: Qt.StrongFocus
                    hoverEnabled: true
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData.label
                    Accessible.checked: chosen
                    onClicked: Transcript.provider = modelData.id

                    contentItem: RowLayout {
                        spacing: Theme.space(2)
                        Rectangle {
                            Layout.preferredWidth: Theme.space(4)
                            Layout.preferredHeight: Theme.space(4)
                            radius: width / 2
                            color: "transparent"
                            border.width: providerRow.chosen ? Theme.space(1) : Theme.hairline
                            border.color: providerRow.chosen ? Theme.accentStroke : Theme.borderStrong
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text {
                                Layout.fillWidth: true
                                text: providerRow.modelData.label
                                elide: Text.ElideRight
                                color: providerRow.enabled ? Theme.textPrimary : Theme.textDisabled
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textMd
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.fillWidth: true
                                text: providerRow.modelData.available ? providerRow.modelData.detail : providerRow.modelData.reason
                                elide: Text.ElideRight
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                            }
                        }
                    }
                    background: Rectangle {
                        radius: Theme.radiusField
                        color: providerRow.chosen ? Theme.accentWash : providerRow.hovered ? Theme.hover : "transparent"
                        FocusRing {
                            anchors.margins: 0
                            targetRadius: Theme.radiusField
                            visible: providerRow.visualFocus
                        }
                    }
                    HoverHandler {
                        enabled: providerRow.enabled
                        cursorShape: Qt.PointingHandCursor
                    }
                }
            }

            // No transcriber: say how to get one.
            Rectangle {
                Layout.fillWidth: true
                visible: Transcript.setupNeeded
                implicitHeight: setupHint.implicitHeight + Theme.space(4)
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: Theme.border

                ColumnLayout {
                    id: setupHint
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: Theme.space(3)
                    spacing: Theme.space(2)
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: qsTr("Set up transcription: add a speech-to-text provider (Deepgram, ElevenLabs, AssemblyAI) in AI providers, or use a build with local transcription.")
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    PillButton {
                        objectName: "transcriptionSetupButton"
                        compact: true
                        iconName: "sparkles"
                        text: qsTr("Set up transcription…")
                        onClicked: root.setupRequested()
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                PillButton {
                    objectName: "transcribeSequenceButton"
                    Layout.fillWidth: true
                    variant: "primary"
                    iconName: "captions"
                    text: qsTr("Transcribe sequence")
                    enabled: !Transcript.setupNeeded && Transcript.jobStatus !== "running"
                    onClicked: root.start("sequence")
                }
                PillButton {
                    objectName: "transcribeSelectionButton"
                    text: qsTr("Selected clips")
                    enabled: !Transcript.setupNeeded && Transcript.jobStatus !== "running" && ProjectController.selectedClips.length > 0
                    onClicked: root.start("selection")
                }
            }
            JobRow { Layout.fillWidth: true }
            Text {
                Layout.fillWidth: true
                visible: text !== ""
                text: Transcript.message
                wrapMode: Text.Wrap
                color: Theme.warning
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }
    }

    // ---- The transcript ----
    ColumnLayout {
        anchors.fill: parent
        visible: root.hasWords
        spacing: Theme.space(2)

        // Find
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)

            TextField {
                id: search
                objectName: "transcriptSearch"
                Layout.fillWidth: true
                implicitHeight: Theme.controlHeightSmall + Theme.space(1)
                leftPadding: Theme.space(8)
                rightPadding: Theme.space(3)
                placeholderText: qsTr("Find in transcript")
                placeholderTextColor: Theme.textTertiary
                color: Theme.textPrimary
                selectionColor: Theme.accentFill
                selectedTextColor: Theme.accentOnFill
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                selectByMouse: true
                Accessible.name: qsTr("Find in transcript")
                onTextChanged: Transcript.searchQuery = text
                Keys.onReturnPressed: event => Transcript.nextMatch((event.modifiers & Qt.ShiftModifier) ? -1 : 1)
                Keys.onEscapePressed: text = ""

                background: Rectangle {
                    radius: height / 2
                    color: Theme.raised
                    border.width: search.activeFocus ? Theme.focusRingWidth : 0
                    border.color: Theme.focus
                }
                Icon {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.space(3)
                    anchors.verticalCenter: parent.verticalCenter
                    name: "search"
                    size: Theme.iconSm
                    color: Theme.textTertiary
                }
            }
            Text {
                visible: Transcript.searchQuery !== ""
                text: Transcript.matchCount === 0 ? qsTr("No match")
                    : Transcript.matchIndex >= 0 ? qsTr("%1 of %2").arg(Transcript.matchIndex + 1).arg(Transcript.matchCount)
                                                 : qsTr("%n match(es)", "", Transcript.matchCount)
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            IconButton {
                visible: Transcript.matchCount > 0
                compact: true
                iconName: "chevron-left"
                tooltip: qsTr("Previous match")
                onClicked: Transcript.nextMatch(-1)
            }
            IconButton {
                visible: Transcript.matchCount > 0
                compact: true
                iconName: "chevron-right"
                tooltip: qsTr("Next match")
                onClicked: Transcript.nextMatch(1)
            }
            PillButton {
                objectName: "selectAllMatches"
                visible: Transcript.matchCount > 1
                compact: true
                text: qsTr("Select all")
                onClicked: Transcript.selectAllMatches()
            }
        }

        JobRow { Layout.fillWidth: true }

        // The words. A press picks the word under the pointer; dragging extends the selection across paragraphs.
        FocusScope {
            id: textArea
            objectName: "transcriptText"
            Layout.fillWidth: true
            Layout.fillHeight: true
            activeFocusOnTab: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Transcript words")

            // The ring shows for keyboard focus only.
            property bool pointerFocus: false
            onActiveFocusChanged: {
                if (!activeFocus)
                    pointerFocus = false
            }

            // Delete / Backspace previews deleting the selection, Return applies a preview, Esc drops it (claimed before
            // the keymap's Delete, which would delete clips).
            Keys.onShortcutOverride: event => {
                if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) && Transcript.hasSelection)
                    event.accepted = true
                else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && Transcript.previewing)
                    event.accepted = true
                else if (event.key === Qt.Key_Escape && (Transcript.previewing || Transcript.hasSelection))
                    event.accepted = true
            }
            Keys.onPressed: event => {
                if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) && Transcript.hasSelection) {
                    Transcript.previewDelete()
                    event.accepted = true
                } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && Transcript.previewing) {
                    Transcript.applyPreview()
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape) {
                    if (Transcript.previewing)
                        Transcript.cancelPreview()
                    else
                        Transcript.clearSelection()
                    event.accepted = true
                }
            }

            ListView {
                id: list
                objectName: "transcriptList"
                anchors.fill: parent
                clip: true
                model: Transcript.paragraphs
                spacing: Theme.space(3)
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ThinScrollBar {}

                delegate: Item {
                    id: para

                    required property int index
                    required property string speaker
                    required property int speakerIndex
                    required property string timeText
                    required property int first
                    required property int last
                    required property var words

                    // Paragraph drag (move): it follows the pointer, lifted.
                    property real dragOffset: 0
                    readonly property bool dragging: grip.active

                    width: ListView.view ? ListView.view.width - Theme.space(3) : 0
                    height: column.implicitHeight
                    z: dragging ? 10 : 0

                    // Select by clicking and dragging over the words of any paragraph. Declared before the words, so it
                    // lies under them (they take no mouse input) and childAt, which looks from the last child, finds
                    // the words first.
                    MouseArea {
                        anchors.fill: parent
                        anchors.topMargin: flow.y
                        preventStealing: true
                        property int anchorWord: -1
                        onPressed: mouse => {
                            textArea.pointerFocus = true
                            textArea.forceActiveFocus(Qt.MouseFocusReason)
                            const p = mapToItem(para, mouse.x, mouse.y)
                            const w = root.wordUnder(para, p.x, p.y)
                            anchorWord = w
                            if (w >= 0)
                                Transcript.clickWord(w, mouse.modifiers)
                        }
                        onPositionChanged: mouse => {
                            if (!pressed || anchorWord < 0 || (mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier)))
                                return
                            const p = mapToItem(list, mouse.x, mouse.y)
                            const w = root.wordUnder(list, p.x, p.y)
                            if (w >= 0)
                                Transcript.selectWords(anchorWord, w)
                        }
                    }

                    Column {
                        id: column
                        width: para.width
                        y: para.dragOffset
                        spacing: Theme.space(1)
                        scale: para.dragging ? Theme.liftScale : 1

                        RowLayout {
                            width: column.width
                            spacing: Theme.space(2)

                            Icon {
                                name: "grip-vertical"
                                size: Theme.iconSm
                                color: grip.active || grip.containsMouse ? Theme.textSecondary : Theme.textTertiary

                                MouseArea {
                                    id: grip
                                    objectName: "paragraphGrip"
                                    anchors.fill: parent
                                    anchors.margins: -Theme.space(1)
                                    hoverEnabled: true
                                    preventStealing: true
                                    cursorShape: active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                                    property real pressY: 0
                                    property bool active: false
                                    onPressed: mouse => {
                                        pressY = mapToItem(null, mouse.x, mouse.y).y
                                        active = false
                                    }
                                    onPositionChanged: mouse => {
                                        if (!pressed)
                                            return
                                        const dy = mapToItem(null, mouse.x, mouse.y).y - pressY
                                        if (!active && Math.abs(dy) < Application.styleHints.startDragDistance)
                                            return
                                        active = true
                                        para.dragOffset = dy
                                    }
                                    onReleased: {
                                        if (active) {
                                            // Before the paragraph under the pointer (after it when past its middle).
                                            const y = para.y + para.dragOffset + Theme.space(2)
                                            let target = list.indexAt(Theme.space(4), y)
                                            if (target < 0)
                                                target = para.dragOffset < 0 ? 0 : list.count
                                            else if (para.dragOffset > 0)
                                                target += 1
                                            Transcript.moveParagraph(para.index, target)
                                        }
                                        active = false
                                        para.dragOffset = 0
                                    }
                                    onCanceled: {
                                        active = false
                                        para.dragOffset = 0
                                    }
                                }
                            }
                            Text {
                                visible: para.speaker !== ""
                                text: para.speaker
                                color: Theme.clipColor(Math.max(0, para.speakerIndex) + 1)
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                                font.weight: Font.DemiBold
                            }
                            Text {
                                text: para.timeText
                                color: Theme.textTertiary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.textXs
                            }
                            Item { Layout.fillWidth: true; Layout.preferredHeight: 1 }
                        }

                        Flow {
                            id: flow
                            width: column.width

                            Repeater {
                                model: para.words
                                delegate: WordText {}
                            }
                        }
                    }
                }
            }

            FocusRing {
                targetRadius: Theme.radiusField
                visible: textArea.activeFocus && !textArea.pointerFocus
            }
        }

        // The selection, and the preview of an edit.
        Rectangle {
            objectName: "transcriptActions"
            Layout.fillWidth: true
            visible: Transcript.hasSelection || Transcript.previewing || Transcript.message !== ""
            implicitHeight: actions.implicitHeight + Theme.space(3)
            radius: Theme.radiusField
            color: Transcript.previewing ? Qt.alpha(Theme.danger, 0.10) : Theme.sunken
            border.width: Theme.hairline
            border.color: Transcript.previewing ? Theme.danger : Theme.border

            ColumnLayout {
                id: actions
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: Theme.space(2)
                spacing: Theme.space(1)

                Text {
                    objectName: "transcriptActionInfo"
                    Layout.fillWidth: true
                    text: Transcript.previewing ? Transcript.previewInfo : Transcript.selectionInfo
                    visible: text !== ""
                    elide: Text.ElideRight
                    color: Transcript.previewing ? Theme.danger : Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: Transcript.message
                    wrapMode: Text.Wrap
                    color: Theme.warning
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                RowLayout {
                    id: fixRow
                    objectName: "transcriptFixRow"
                    Layout.fillWidth: true
                    visible: false
                    spacing: Theme.space(1)
                    FieldInput {
                        id: fixField
                        objectName: "transcriptFixField"
                        Layout.fillWidth: true
                        Accessible.name: qsTr("The corrected words")
                        onAccepted: fixOk.clicked()
                        Keys.onEscapePressed: fixRow.visible = false
                    }
                    PillButton {
                        id: fixOk
                        objectName: "transcriptFixOk"
                        compact: true
                        variant: "primary"
                        text: qsTr("Fix")
                        onClicked: {
                            if (Transcript.correctSelection(fixField.text) === "")
                                fixRow.visible = false
                        }
                    }
                    PillButton {
                        compact: true
                        text: qsTr("Cancel")
                        onClicked: fixRow.visible = false
                    }
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.space(1)
                    visible: Transcript.hasSelection || Transcript.previewing

                    PillButton {
                        objectName: "transcriptDelete"
                        visible: !Transcript.previewing
                        compact: true
                        iconName: "trash-2"
                        text: qsTr("Delete")
                        command: "edit.clear"
                        onClicked: Transcript.previewDelete()
                    }
                    PillButton {
                        objectName: "transcriptKeep"
                        visible: !Transcript.previewing
                        compact: true
                        iconName: "scissors"
                        text: qsTr("Keep only")
                        onClicked: Transcript.previewKeep()
                    }
                    // C.7 corrections: the selected words read differently from now on (captions follow on Update).
                    PillButton {
                        objectName: "transcriptFix"
                        visible: !Transcript.previewing && !fixRow.visible
                        compact: true
                        iconName: "pencil"
                        text: qsTr("Fix…")
                        tooltip: qsTr("Correct a transcription mistake")
                        onClicked: {
                            fixField.text = Transcript.selectedText()
                            fixRow.visible = true
                            fixField.forceActiveFocus()
                            fixField.selectAll()
                        }
                    }
                    PillButton {
                        objectName: "transcriptApply"
                        visible: Transcript.previewing
                        compact: true
                        variant: "primary"
                        text: qsTr("Remove")
                        onClicked: Transcript.applyPreview()
                    }
                    PillButton {
                        visible: Transcript.previewing
                        compact: true
                        text: qsTr("Cancel")
                        onClicked: Transcript.cancelPreview()
                    }
                    PillButton {
                        visible: !Transcript.previewing
                        compact: true
                        text: qsTr("Clear")
                        onClicked: Transcript.clearSelection()
                    }
                }
            }
        }
    }

    AiConsentDialog {
        id: consent
        property string scope: "sequence"
        onAllowed: always => {
            if (always)
                AiController.grantConsent(consent.prompt.provider, "transcribe")
            Transcript.transcribe(consent.scope, true) // the user's answer: allowed for this job
        }
    }
}
