pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// The Transcript tab's Source mode (P0 round): what the clip in the Source Monitor says (SourceTranscript), to pick a
// soundbite by its words and edit it into the sequence - Premiere's Text panel on a source clip.
//   click a word        the Source Monitor goes there; the word is selected and becomes the source In to Out
//   drag over words     selects a run of words; Shift-click extends it; its frames become the source In and Out (one
//                       undo step when the drag ends)
//   Insert / Overwrite  the keymap's Insert (,) and Overwrite (.) with those marks, as the Source Monitor's buttons
//   Copy                the selected words (Ctrl+C); find marks every occurrence, Return / the arrows jump
// Clips with several audio streams (OBS recordings) show one at a time: the stream tabs choose it (Mic first when it
// has no transcript yet). Without a transcript: Transcribe this clip (the provider chosen in the Sequence view, with
// its progress and Cancel). The sequence's Delete / Keep only / move are not here: they edit the sequence.
FocusScope {
    id: root
    objectName: "sourceTranscriptPage"

    signal setupRequested()

    readonly property string sourceState: SourceTranscript.state
    readonly property bool hasWords: SourceTranscript.wordCount > 0
    readonly property bool ready: root.sourceState === "ready"
    readonly property var availableProviders: Transcript.providers.filter(p => p.available === true)
    readonly property string providerLabel: {
        const all = Transcript.providers
        for (let i = 0; i < all.length; ++i) {
            if (all[i].id === Transcript.provider)
                return all[i].label
        }
        return ""
    }

    function start() {
        const prompt = SourceTranscript.consentFor()
        if (prompt.needed)
            consent.ask(prompt)
        else
            SourceTranscript.transcribe(false)
    }

    // Insert (true) or Overwrite with the selection's marks: the marks first (set again even when the Source Monitor's
    // were changed since: the button edits the selected words), then the keymap command Main.qml runs (sourceEdit: the
    // Fit Clip question, the ghost, the playhead).
    function editIn(insert: bool) {
        if (SourceTranscript.applyMarks(true) !== "")
            return
        KeymapDispatcher.trigger(insert ? "clip.insert" : "clip.overwrite")
    }

    // Brings word `word` into view (its paragraph, or its line in a paragraph taller than the view).
    function showWord(word: int) {
        const paragraph = SourceTranscript.paragraphOfWord(word)
        if (paragraph >= 0)
            list.positionViewAtIndex(paragraph, ListView.Contain)
    }

    Connections {
        target: SourceTranscript
        function onSearchChanged() {
            if (!root.hasWords || SourceTranscript.matchIndex < 0)
                return
            const first = SourceTranscript.selectionFirst()
            if (first >= 0)
                root.showWord(first)
        }
        function onCurrentWordChanged() {
            const word = SourceTranscript.currentWord
            if (root.hasWords && Preferences.transcriptFollowPlayhead && word >= 0 && Date.now() - list.userScrolledAt >= 3000)
                root.showWord(word)
        }
    }

    // One word: its index, its marks (selected, a search match) and the Source Monitor's position.
    component WordText: Text {
        id: word
        required property var modelData
        readonly property int wordIndex: modelData.i
        readonly property int marks: SourceTranscript.markRevision
        readonly property bool selected: marks >= 0 && SourceTranscript.isSelected(wordIndex)
        readonly property bool match: marks >= 0 && SourceTranscript.isMatch(wordIndex)
        readonly property bool current: SourceTranscript.currentWord === wordIndex

        text: modelData.text
        color: word.current ? Theme.accentOnFill : Theme.textPrimary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textLg
        lineHeight: 1.25

        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.topMargin: 1
            anchors.bottomMargin: 1
            radius: Theme.radiusBadge
            color: word.current ? Theme.accentFill
                 : word.selected ? Theme.accentWash
                 : word.match ? Qt.alpha(Theme.warning, 0.28) : "transparent"
            border.width: word.selected && !word.current ? Theme.hairline : 0
            border.color: Theme.accentStroke
        }
    }

    // The word index under a point of an item (-1 between words).
    function wordUnder(item: Item, x: real, y: real): int {
        let target = item
        let point = Qt.point(x, y)
        while (target) {
            const found = target as WordText
            if (found)
                return found.wordIndex
            const child = target.childAt(point.x, point.y)
            if (!child)
                return -1
            point = target.mapToItem(child, point.x, point.y)
            target = child
        }
        return -1
    }

    component Caption: Text {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)

        // The clip and, for a multi-track recording, which of its audio streams is read.
        RowLayout {
            Layout.fillWidth: true
            visible: root.sourceState !== "none"
            spacing: Theme.space(2)
            Icon {
                name: "film"
                size: Theme.iconSm
                color: Theme.textTertiary
            }
            Text {
                objectName: "sourceTranscriptClipName"
                Layout.fillWidth: true
                text: SourceTranscript.mediaName
                elide: Text.ElideMiddle
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                font.weight: Font.DemiBold
            }
        }
        PillTabs {
            objectName: "sourceTranscriptStreams"
            Layout.fillWidth: true
            visible: root.ready && SourceTranscript.streams.length > 1
            compact: true
            model: SourceTranscript.streams.map(s => s.label)
            currentIndex: SourceTranscript.stream
            Accessible.name: qsTr("Audio stream to read")
            onActivated: index => SourceTranscript.stream = index
        }

        // ---- Nothing to read: why, and what to do ----
        Flickable {
            id: empty
            objectName: "sourceTranscriptEmpty"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !root.hasWords
            clip: true
            contentHeight: emptyColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}

            ColumnLayout {
                id: emptyColumn
                width: empty.width
                spacing: Theme.space(3)

                SvgArt {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: Theme.space(2)
                    Layout.preferredWidth: Theme.space(32)
                    Layout.preferredHeight: Theme.space(24)
                    source: "illustrations/empty-transcript.svg"
                    tint: Theme.textTertiary
                }
                Text {
                    objectName: "sourceTranscriptEmptyTitle"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: root.sourceState === "none" ? qsTr("No clip in the Source Monitor")
                        : root.sourceState === "preview" ? qsTr("Import this file to read it")
                        : root.sourceState === "noAudio" ? qsTr("This clip has no sound")
                        : qsTr("No transcript of this clip yet")
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXl
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: root.sourceState === "none"
                          ? qsTr("Double-click a clip in the Project panel (or Match Frame, F) to open it in the Source Monitor, then read what it says here, select a soundbite and Insert or Overwrite it.")
                        : root.sourceState === "preview"
                          ? qsTr("The Media Browser shows this file before it is imported. Mark it or edit it in to import it; its transcript shows here then.")
                        : root.sourceState === "noAudio" ? qsTr("Only clips with sound have words to read.")
                        : qsTr("Transcribe this clip to read it here: select words to mark a soundbite, then Insert or Overwrite it into the sequence.")
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                }

                // Transcribe this clip (the provider chosen for the sequence; change it here too).
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.ready
                    spacing: Theme.space(2)
                    PillButton {
                        objectName: "transcribeClipButton"
                        Layout.fillWidth: true
                        variant: "primary"
                        iconName: "captions"
                        text: qsTr("Transcribe this clip")
                        enabled: !Transcript.setupNeeded && Transcript.jobStatus !== "running"
                        onClicked: root.start()
                    }
                    PillButton {
                        id: providerButton
                        objectName: "sourceTranscriptProvider"
                        visible: !Transcript.setupNeeded && root.availableProviders.length > 1
                        compact: true
                        text: root.providerLabel
                        tooltip: qsTr("How to transcribe")
                        onClicked: providerMenu.popup(providerButton, 0, providerButton.height)
                    }
                }
                Caption {
                    visible: root.ready && !Transcript.setupNeeded && root.providerLabel !== ""
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("With %1").arg(root.providerLabel)
                    color: Theme.textTertiary
                }
                PillButton {
                    objectName: "sourceTranscriptSetupButton"
                    Layout.alignment: Qt.AlignHCenter
                    visible: root.ready && Transcript.setupNeeded
                    compact: true
                    iconName: "sparkles"
                    text: qsTr("Set up transcription…")
                    onClicked: root.setupRequested()
                }
                // The job (shared with the sequence's transcription).
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: Transcript.jobStatus !== "idle"
                    spacing: Theme.space(1)
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            text: Transcript.jobText
                            elide: Text.ElideRight
                            color: Transcript.jobStatus === "error" ? Theme.warningText : Theme.textSecondary
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
                Caption {
                    visible: text !== ""
                    text: SourceTranscript.message
                    color: Theme.warningText
                }
            }
        }

        // ---- The words ----
        RowLayout {
            Layout.fillWidth: true
            visible: root.hasWords
            spacing: Theme.space(1)

            TextField {
                id: search
                objectName: "sourceTranscriptSearch"
                Layout.fillWidth: true
                implicitHeight: Theme.controlHeightSmall + Theme.space(1)
                leftPadding: Theme.space(8)
                rightPadding: Theme.space(3)
                placeholderText: qsTr("Find in this clip")
                placeholderTextColor: Theme.textTertiary
                color: Theme.textPrimary
                selectionColor: Theme.accentFill
                selectedTextColor: Theme.accentOnFill
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                selectByMouse: true
                Accessible.name: qsTr("Find in this clip")
                onTextChanged: SourceTranscript.searchQuery = text
                Keys.onReturnPressed: event => SourceTranscript.nextMatch((event.modifiers & Qt.ShiftModifier) ? -1 : 1)
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
                visible: SourceTranscript.searchQuery !== ""
                text: SourceTranscript.matchCount === 0 ? qsTr("No match")
                    : SourceTranscript.matchIndex >= 0 ? qsTr("%1 of %2").arg(SourceTranscript.matchIndex + 1).arg(SourceTranscript.matchCount)
                                                       : qsTr("%n match(es)", "", SourceTranscript.matchCount)
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            IconButton {
                visible: SourceTranscript.matchCount > 0
                compact: true
                iconName: "chevron-left"
                tooltip: qsTr("Previous match")
                onClicked: SourceTranscript.nextMatch(-1)
            }
            IconButton {
                visible: SourceTranscript.matchCount > 0
                compact: true
                iconName: "chevron-right"
                tooltip: qsTr("Next match")
                onClicked: SourceTranscript.nextMatch(1)
            }
        }

        // A press picks the word under the pointer; dragging extends the run across paragraphs; the release sets the
        // marks.
        FocusScope {
            id: textArea
            objectName: "sourceTranscriptText"
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.hasWords
            activeFocusOnTab: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Source clip words")

            property bool pointerFocus: false
            onActiveFocusChanged: {
                if (!activeFocus)
                    pointerFocus = false
            }

            // Ctrl+C copies the words (the clip clipboard is untouched); Esc drops the selection (the marks stay).
            Keys.onShortcutOverride: event => {
                if (event.matches(StandardKey.Copy) && SourceTranscript.hasSelection)
                    event.accepted = true
                else if (event.key === Qt.Key_Escape && SourceTranscript.hasSelection)
                    event.accepted = true
            }
            Keys.onPressed: event => {
                if (event.matches(StandardKey.Copy) && SourceTranscript.hasSelection) {
                    SourceTranscript.copySelection(false)
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape && SourceTranscript.hasSelection) {
                    SourceTranscript.clearSelection()
                    event.accepted = true
                }
            }

            ListView {
                id: list
                objectName: "sourceTranscriptList"
                anchors.fill: parent
                clip: true
                model: SourceTranscript.paragraphs
                spacing: Theme.space(3)
                boundsBehavior: Flickable.StopAtBounds
                property double userScrolledAt: 0
                onMovementStarted: userScrolledAt = Date.now()
                onMovementEnded: userScrolledAt = Date.now()
                ScrollBar.vertical: ThinScrollBar {
                    onPressedChanged: list.userScrolledAt = Date.now()
                }

                delegate: Item {
                    id: para

                    required property int index
                    required property string speaker
                    required property int speakerIndex
                    required property string timeText
                    required property var words

                    width: ListView.view ? ListView.view.width - Theme.space(3) : 0
                    height: column.implicitHeight

                    // Declared before the words so it lies under them; childAt finds the words first.
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
                            // Fix round 1: a Shift / Ctrl / Cmd click extends the run; no drag starts from it (a drag
                            // from the clicked word would undo the extension once the key is let go).
                            const extending = (mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier | Qt.MetaModifier)) !== 0
                            anchorWord = extending ? -1 : w
                            if (w >= 0)
                                SourceTranscript.clickWord(w, mouse.modifiers)
                        }
                        onPositionChanged: mouse => {
                            if (!pressed || anchorWord < 0
                                    || (mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier | Qt.MetaModifier)))
                                return
                            const p = mapToItem(list, mouse.x, mouse.y)
                            const w = root.wordUnder(list, p.x, p.y)
                            if (w >= 0)
                                SourceTranscript.selectWords(anchorWord, w)
                        }
                        onReleased: SourceTranscript.applyMarks()
                        onCanceled: SourceTranscript.applyMarks()
                    }

                    Column {
                        id: column
                        width: para.width
                        spacing: Theme.space(1)

                        RowLayout {
                            width: column.width
                            spacing: Theme.space(2)
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

        // The soundbite: its words and length, the source marks it set, and the source-mode actions.
        Rectangle {
            objectName: "sourceTranscriptActions"
            Layout.fillWidth: true
            visible: root.hasWords && (SourceTranscript.hasSelection || SourceTranscript.message !== "")
            implicitHeight: actions.implicitHeight + Theme.space(3)
            radius: Theme.radiusField
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: Theme.border

            ColumnLayout {
                id: actions
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: Theme.space(2)
                spacing: Theme.space(1)

                Text {
                    objectName: "sourceTranscriptSelectionInfo"
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: SourceTranscript.selectionInfo
                    elide: Text.ElideRight
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Font.DemiBold
                }
                Text {
                    objectName: "sourceTranscriptMarks"
                    Layout.fillWidth: true
                    visible: SourceTranscript.hasSelection && SourceMonitor.hasIn && SourceMonitor.hasOut
                    //: The source marks the selected words set: "In 00:00:01:02 · Out 00:00:03:12"
                    text: qsTr("In %1 · Out %2").arg(SourceMonitor.inText).arg(SourceMonitor.outText)
                    elide: Text.ElideRight
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
                Caption {
                    visible: text !== ""
                    text: SourceTranscript.message
                    color: Theme.warningText
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.space(1)
                    visible: SourceTranscript.hasSelection

                    PillButton {
                        objectName: "sourceTranscriptInsert"
                        compact: true
                        variant: "primary"
                        iconName: "insert-edit"
                        text: qsTr("Insert")
                        command: "clip.insert"
                        tooltip: qsTr("Insert the selected words into the sequence")
                        enabled: SourceMonitor.canEdit
                        onClicked: root.editIn(true)
                    }
                    PillButton {
                        objectName: "sourceTranscriptOverwrite"
                        compact: true
                        iconName: "overwrite-edit"
                        text: qsTr("Overwrite")
                        command: "clip.overwrite"
                        tooltip: qsTr("Overwrite the sequence with the selected words")
                        enabled: SourceMonitor.canEdit
                        onClicked: root.editIn(false)
                    }
                    PillButton {
                        objectName: "sourceTranscriptCopy"
                        compact: true
                        iconName: "copy"
                        text: qsTr("Copy")
                        command: "edit.copy"
                        tooltip: qsTr("Copy the selected words")
                        onClicked: SourceTranscript.copySelection(false)
                    }
                    PillButton {
                        objectName: "sourceTranscriptClear"
                        compact: true
                        text: qsTr("Clear")
                        onClicked: SourceTranscript.clearSelection()
                    }
                }
            }
        }
    }

    // The transcriber for this clip (shared with the Sequence view).
    OptionsMenu {
        id: providerMenu
        objectName: "sourceTranscriptProviderMenu"
        options: root.availableProviders.map(p => ({ id: p.id, name: p.label }))
        current: Transcript.provider
        onChosen: id => Transcript.provider = id
    }

    AiConsentDialog {
        id: consent
        onAllowed: always => {
            if (always)
                AiController.grantConsent(consent.prompt.provider, "transcribe")
            SourceTranscript.transcribe(true) // the user's answer: allowed for this job
        }
    }
}
