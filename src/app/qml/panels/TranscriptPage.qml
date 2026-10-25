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
//   right-click         (0.4) the word menu: Copy (Ctrl+C), Copy with Timecodes, Mark In and Out, Add Marker, Add Chapter
//                       Marker, Lift, Delete, Keep Only, Fix…, Find All (also the Menu key and Shift+F10)
//   follow              (0.4) the word under the playhead stays in view while playing or scrubbing, with its paragraph
//                       when that fits (the crosshair button; scrolling pauses it for 3 s)
// The word under the playhead is filled with the accent. Thai and other unspaced languages sit word unit by word unit
// with no spaces (the model's word boundaries), so lines only break between units.
// Without a transcript: choose a transcriber (this computer or a cloud model from Preferences > AI, with the consent
// prompt before the first upload) and transcribe the sequence or the selected clips, with progress and Cancel.
// 0.4: the audio tracks to transcribe are switches (the mic and dialogue tracks by default; the choice is remembered by
// track name).
FocusScope {
    id: root
    objectName: "transcriptPage"

    signal setupRequested()

    readonly property bool hasWords: Transcript.wordCount > 0
    /// The audio tracks Transcribe sends (Transcript.audioTracks' chosen ones).
    readonly property int chosenTracks: Transcript.audioTracks.filter(t => t.chosen === true).length
    readonly property bool tracksReady: chosenTracks > 0 || Transcript.audioTracks.length === 0
    /// What the word menu runs once it has closed ("fix", "find"): a row that moves the focus waits for the menu.
    property string pendingMenuAction: ""

    function tracksSummary(chosen: int, total: int): string {
        return total === 1 ? qsTr("%1 of 1 audio track").arg(chosen) : qsTr("%1 of %2 audio tracks").arg(chosen).arg(total)
    }

    // The word menu from the keyboard (Menu, Shift+F10): under the first selected word, scrolled into view.
    function openWordMenuFromKeys() {
        let x = Theme.space(4)
        let y = Theme.space(4)
        const first = Transcript.selectionFirst()
        const wordItem = first >= 0 ? root.showWord(first) : null
        if (wordItem) {
            const at = wordItem.mapToItem(textArea, 0, wordItem.height)
            x = at.x
            y = at.y
        }
        wordMenu.popup(textArea, x, y)
    }

    // The WordText of word `word` in a paragraph delegate (null when it is not built).
    function wordItemIn(item: Item, word: int): Item {
        const pending = [item]
        while (pending.length > 0) {
            const at = pending.pop()
            const text = at as WordText
            if (text && text.wordIndex === word)
                return text
            for (let i = 0; i < at.children.length; ++i)
                pending.push(at.children[i])
        }
        return null
    }

    // Brings word `word` into view and returns its WordText (null when it is not built). Its paragraph is contained (no
    // jump while it shows). A paragraph taller than the view (a long monologue runs to 80 words) scrolls to the word's
    // line instead: Contain would put that paragraph's top at the view's top on every word and hide its later lines.
    function showWord(word: int): Item {
        const paragraph = Transcript.paragraphOfWord(word)
        if (paragraph < 0)
            return null
        let item = list.itemAtIndex(paragraph)
        if (!item || item.height <= list.height) {
            list.positionViewAtIndex(paragraph, ListView.Contain)
            item = list.itemAtIndex(paragraph)
        }
        if (!item)
            return null
        const wordItem = root.wordItemIn(item, word)
        if (!wordItem || item.height <= list.height)
            return wordItem
        const top = wordItem.mapToItem(list.contentItem, 0, 0).y
        if (top >= list.contentY && top + wordItem.height <= list.contentY + list.height)
            return wordItem
        const lowest = list.originY + Math.max(0, list.contentHeight - list.height)
        list.contentY = Math.max(list.originY, Math.min(top - Theme.space(2), lowest))
        return wordItem
    }

    function openFixRow() {
        fixField.text = Transcript.selectedText()
        fixRow.visible = true
        fixField.forceActiveFocus()
        fixField.selectAll()
    }

    // Follow the playhead (0.4): the word under it comes into view (showWord: its paragraph, or its line in a paragraph
    // taller than the view), unless the user scrolled in the last 3 s. A Find jump always shows its match.
    Connections {
        target: Transcript
        function onCurrentWordChanged() {
            const word = Transcript.currentWord
            if (!root.hasWords || !Preferences.transcriptFollowPlayhead || word < 0 || Date.now() - list.userScrolledAt < 3000)
                return
            root.showWord(word)
        }
        function onSearchChanged() {
            if (!root.hasWords || Transcript.matchIndex < 0)
                return
            const first = Transcript.selectionFirst()
            if (first >= 0)
                root.showWord(first)
        }
    }

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

            // 0.4: the audio tracks to transcribe (the voice ones by default; game audio, music and voice chat off).
            GroupLabel {
                objectName: "transcribeTracksLabel"
                visible: Transcript.audioTracks.length > 0
                text: qsTr("Tracks")
            }
            ColumnLayout {
                objectName: "transcribeTracks"
                Layout.fillWidth: true
                visible: Transcript.audioTracks.length > 0
                spacing: 0
                Repeater {
                    // By count, so a switch keeps its focus when the choice changes.
                    model: Transcript.audioTracks.length
                    delegate: Toggle {
                        id: trackToggle
                        required property int index
                        readonly property var track: Transcript.audioTracks[index] ?? ({})
                        objectName: "transcribeTrack_" + index
                        Layout.fillWidth: true
                        text: track.label ?? ""
                        checked: track.chosen === true
                        tooltip: checked ? qsTr("Leave %1 out of the transcription").arg(text) : qsTr("Transcribe %1 too").arg(text)
                        onToggled: Transcript.setTrackChosen(trackToggle.index, trackToggle.checked)
                    }
                }
                Text {
                    objectName: "transcribeTracksSummary"
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.space(1)
                    text: root.tracksSummary(root.chosenTracks, Transcript.audioTracks.length)
                    color: root.chosenTracks > 0 ? Theme.textSecondary : Theme.warning
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Font.DemiBold
                }
                Text {
                    objectName: "transcribeTracksHint"
                    Layout.fillWidth: true
                    visible: root.chosenTracks < Transcript.audioTracks.length
                    text: root.chosenTracks > 0 ? qsTr("Game audio, music and voice chat are left out; switch them on to transcribe them too.")
                                                : qsTr("Choose at least one audio track")
                    wrapMode: Text.Wrap
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
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
                    enabled: !Transcript.setupNeeded && Transcript.jobStatus !== "running" && root.tracksReady
                    onClicked: root.start("sequence")
                }
                PillButton {
                    objectName: "transcribeSelectionButton"
                    text: qsTr("Selected clips")
                    enabled: !Transcript.setupNeeded && Transcript.jobStatus !== "running" && ProjectController.selectedClips.length > 0
                             && root.tracksReady
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
            // 0.4: follow the playhead (remembered).
            IconButton {
                objectName: "transcriptFollowToggle"
                compact: true
                checkable: true
                checked: Preferences.transcriptFollowPlayhead
                iconName: "crosshair"
                tooltip: checked ? qsTr("Stop following the playhead") : qsTr("Follow the playhead")
                onClicked: {
                    Preferences.transcriptFollowPlayhead = checked
                    checked = Qt.binding(() => Preferences.transcriptFollowPlayhead)
                }
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
            // 0.4: Ctrl+C copies the selected words (the clip clipboard is untouched), Menu / Shift+F10 open the word menu.
            Keys.onShortcutOverride: event => {
                if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) && Transcript.hasSelection)
                    event.accepted = true
                else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && Transcript.previewing)
                    event.accepted = true
                else if (event.key === Qt.Key_Escape && (Transcript.previewing || Transcript.hasSelection))
                    event.accepted = true
                else if (event.matches(StandardKey.Copy) && Transcript.hasSelection)
                    event.accepted = true
                else if ((event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && event.modifiers === Qt.ShiftModifier))
                         && Transcript.hasSelection)
                    event.accepted = true
            }
            Keys.onPressed: event => {
                if (event.matches(StandardKey.Copy) && Transcript.hasSelection) {
                    Transcript.copySelection(false)
                    event.accepted = true
                } else if ((event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && event.modifiers === Qt.ShiftModifier))
                           && Transcript.hasSelection) {
                    root.openWordMenuFromKeys()
                    event.accepted = true
                } else if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) && Transcript.hasSelection) {
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
                // 0.4: when the user last scrolled (wheel, drag, the scroll bar): following waits 3 s after it.
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
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        property int anchorWord: -1
                        onPressed: mouse => {
                            textArea.pointerFocus = true
                            textArea.forceActiveFocus(Qt.MouseFocusReason)
                            const p = mapToItem(para, mouse.x, mouse.y)
                            const w = root.wordUnder(para, p.x, p.y)
                            // 0.4: the right button selects the word when it is not selected and opens the word menu
                            // (no drag).
                            if (mouse.button === Qt.RightButton) {
                                anchorWord = -1
                                if (w >= 0 && !Transcript.isSelected(w))
                                    Transcript.clickWord(w, 0)
                                if (Transcript.hasSelection) {
                                    const at = mapToItem(textArea, mouse.x, mouse.y)
                                    wordMenu.popup(textArea, at.x, at.y)
                                }
                                return
                            }
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
                        onClicked: root.openFixRow()
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

    // 0.4: the word menu (right-click, Menu, Shift+F10). It acts on the span from the first to the last selected word,
    // except Copy (the selected words). OptionsMenu, not ContextMenu: this file imports QtQuick.Controls.
    OptionsMenu {
        id: wordMenu
        objectName: "transcriptContextMenu"
        onClosed: {
            const action = root.pendingMenuAction
            root.pendingMenuAction = ""
            if (action === "fix") {
                root.openFixRow()
            } else if (action === "find") {
                // As the language writes it (Thai without spaces), as the search matches the shown words; with several
                // runs (Ctrl-click), the first one.
                search.text = Transcript.selectionSearchText()
                search.forceActiveFocus()
            }
        }

        ContextMenuItem {
            objectName: "transcriptCopyItem"
            text: qsTr("Copy")
            iconName: "copy"
            command: "edit.copy"
            onTriggered: Transcript.copySelection(false)
        }
        ContextMenuItem {
            objectName: "transcriptCopyTimecodesItem"
            text: qsTr("Copy with Timecodes")
            iconName: "clock"
            enabled: !Transcript.previewing
            onTriggered: Transcript.copySelection(true)
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            objectName: "transcriptMarkItem"
            text: qsTr("Mark In and Out")
            iconName: "play-in-out"
            enabled: !Transcript.previewing
            onTriggered: Transcript.markSelection()
        }
        ContextMenuItem {
            objectName: "transcriptMarkerItem"
            text: qsTr("Add Marker")
            iconName: "bookmark"
            enabled: !Transcript.previewing
            onTriggered: Transcript.addMarkerFromSelection(false)
        }
        ContextMenuItem {
            objectName: "transcriptChapterItem"
            text: qsTr("Add Chapter Marker")
            iconName: "book-open"
            enabled: !Transcript.previewing
            onTriggered: Transcript.addMarkerFromSelection(true)
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            objectName: "transcriptLiftItem"
            text: qsTr("Lift")
            iconName: "lift-edit"
            enabled: !Transcript.previewing
            onTriggered: Transcript.liftSelection()
        }
        ContextMenuItem {
            objectName: "transcriptDeleteItem"
            text: qsTr("Delete")
            iconName: "trash-2"
            enabled: !Transcript.previewing
            onTriggered: Transcript.previewDelete()
        }
        ContextMenuItem {
            objectName: "transcriptKeepItem"
            text: qsTr("Keep Only")
            iconName: "scissors"
            enabled: !Transcript.previewing
            onTriggered: Transcript.previewKeep()
        }
        ContextMenuItem {
            objectName: "transcriptFixItem"
            text: qsTr("Fix…")
            iconName: "pencil"
            enabled: !Transcript.previewing
            onTriggered: root.pendingMenuAction = "fix"
        }
        ContextMenuSeparator {}
        ContextMenuItem {
            objectName: "transcriptFindAllItem"
            text: qsTr("Find All")
            iconName: "search"
            enabled: !Transcript.previewing
            onTriggered: root.pendingMenuAction = "find"
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
