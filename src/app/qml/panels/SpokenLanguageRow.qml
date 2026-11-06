pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// ● SPOKEN LANGUAGE (card 073): which language a recording is spoken in, for Transcribe, Transcribe again and the
// source clip's Transcribe. "Auto-detect" (the default) or any of the recogniser's languages, found by typing its English
// or its own name ("Hebrew", "עברית", "Íslenska") or its code; every row says how well Whisper knows the language, and a
// note under the button names the model size that suits the choice. "Translate to English" is Whisper's translate task
// (the words come out in English, whatever is spoken), local transcription only. "Mixed languages" (0.7, card 111; shown
// with auto-detect) transcribes a recording that switches language stretch by stretch in the language of each. The choice
// is Transcript.spokenLanguage / Transcript.translateToEnglish / Transcript.mixedLanguages (remembered); nothing here
// starts a job.
Item {
    id: root
    objectName: "spokenLanguageRow"
    implicitWidth: column.implicitWidth
    implicitHeight: column.implicitHeight

    /// The picker's rows for the typed text: Auto-detect first (it matches the text "auto" and an empty field), then the
    /// languages whose English name, own name or code contains it.
    function matches(query: string): var {
        const q = query.trim().toLowerCase()
        const out = []
        if (q === "" || "auto-detect".indexOf(q) >= 0 || "automatic".indexOf(q) >= 0)
            out.push({ code: "auto", label: qsTr("Auto-detect"), tier: "", rtl: false })
        const all = Transcript.spokenLanguages
        for (let i = 0; i < all.length; ++i) {
            const l = all[i]
            if (q === "" || l.name.toLowerCase().indexOf(q) >= 0 || l.nativeName.toLowerCase().indexOf(q) >= 0
                    || l.code === q)
                out.push(l)
        }
        return out
    }
    function tierText(tier: string): string {
        return tier === "strong" ? qsTr("Strong") : tier === "fair" ? qsTr("Fair") : tier === "limited" ? qsTr("Limited") : ""
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: Theme.space(1)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                Layout.fillWidth: true
                text: qsTr("Language spoken")
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.DemiBold
            }
            PillButton {
                id: languageButton
                objectName: "spokenLanguageButton"
                compact: true
                iconName: "chevron-down"
                text: Transcript.spokenLanguageLabel
                tooltip: qsTr("The language of the recording: name it when auto-detect gets a short or noisy clip wrong")
                Accessible.name: qsTr("Language spoken: %1").arg(Transcript.spokenLanguageLabel)
                onClicked: picker.openAt()
            }
        }
        Toggle {
            objectName: "translateToEnglish"
            Layout.fillWidth: true
            text: qsTr("Translate to English")
            tooltip: qsTr("Whisper's translate task: the transcript comes out in English, whatever language is spoken (on this computer only)")
            checked: Transcript.translateToEnglish
            onToggled: {
                Transcript.translateToEnglish = checked
                checked = Qt.binding(() => Transcript.translateToEnglish)
            }
        }
        // 0.7 (card 111): a recording that switches language - an interview with a translator, a streamer who changes
        // language - is transcribed stretch by stretch in the language each is spoken in, instead of in the first one
        // detected. Only with auto-detect (naming a language says it is one) and not with translating.
        Toggle {
            objectName: "mixedLanguages"
            Layout.fillWidth: true
            visible: Transcript.spokenLanguage === "auto" && !Transcript.translateToEnglish
            text: qsTr("Mixed languages")
            tooltip: qsTr("For a recording that switches language (an interview with a translator, say): each stretch of speech is transcribed in the language it is spoken in. On this computer only, and slower")
            checked: Transcript.mixedLanguages
            onToggled: {
                Transcript.mixedLanguages = checked
                checked = Qt.binding(() => Transcript.mixedLanguages)
            }
        }
        Text {
            objectName: "spokenLanguageNote"
            Layout.fillWidth: true
            visible: text !== ""
            text: Transcript.spokenLanguageNote
            wrapMode: Text.Wrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        // 0.7 (card 061): names, game titles and jargon the recogniser should spell right (the project's own list).
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            Text {
                Layout.fillWidth: true
                text: qsTr("Custom words")
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.DemiBold
            }
            PillButton {
                id: wordsButton
                objectName: "customWordsButton"
                compact: true
                iconName: "chevron-down"
                text: Transcript.customWordCount === 0 ? qsTr("None") : qsTr("%n word(s)", "", Transcript.customWordCount)
                tooltip: qsTr("Names, game titles and jargon to listen for, so they are not mangled")
                Accessible.name: qsTr("Custom words: %1").arg(text)
                onClicked: wordsPopover.openAt()
            }
        }
    }

    Popover {
        id: wordsPopover
        objectName: "customWordsPopover"
        title: qsTr("Custom words")
        transformOrigin: Item.TopLeft
        parent: wordsButton
        x: 0
        y: wordsButton.height + Theme.space(1)

        function openAt() {
            wordsField.text = Transcript.customWords
            errorText.text = ""
            open()
            wordsField.forceActiveFocus(Qt.PopupFocusReason)
        }
        function save() {
            const error = Transcript.setCustomWords(wordsField.text)
            if (error === "")
                close()
            else
                errorText.text = error
        }

        Column {
            width: Theme.space(64)
            spacing: Theme.space(2)

            Text {
                width: parent.width
                text: qsTr("One name, title or term per line (or separated by commas): character names, game titles, jargon, invented words. The next transcription listens for them. Audio that already has a transcript is heard again with Transcribe again.")
                wrapMode: Text.Wrap
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Rectangle {
                width: parent.width
                height: Theme.space(32)
                radius: Theme.radiusField
                color: Theme.sunken
                border.width: Theme.hairline
                border.color: wordsField.activeFocus ? Theme.focus : Theme.border
                ScrollView {
                    anchors.fill: parent
                    anchors.margins: Theme.space(1)
                    TextArea {
                        id: wordsField
                        objectName: "customWordsField"
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                        placeholderText: qsTr("Malenia\nElden Ring\nRadahn")
                        placeholderTextColor: Theme.textTertiary
                        background: null
                        Accessible.name: qsTr("Custom words")
                        Keys.onShortcutOverride: event => event.accepted = event.text !== "" || event.key === Qt.Key_Backspace
                    }
                }
            }
            Text {
                id: errorText
                objectName: "customWordsError"
                width: parent.width
                visible: text !== ""
                wrapMode: Text.Wrap
                color: Theme.danger
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Row {
                spacing: Theme.space(2)
                layoutDirection: Qt.RightToLeft
                anchors.right: parent.right
                PillButton {
                    objectName: "customWordsSave"
                    variant: "primary"
                    compact: true
                    text: qsTr("Save")
                    onClicked: wordsPopover.save()
                }
                PillButton {
                    variant: "ghost"
                    compact: true
                    text: qsTr("Cancel")
                    onClicked: wordsPopover.close()
                }
            }
        }
    }

    Popover {
        id: picker
        objectName: "spokenLanguagePicker"
        title: qsTr("Language spoken")
        transformOrigin: Item.TopLeft
        parent: languageButton
        x: 0
        y: languageButton.height + Theme.space(1)
        readonly property var shown: root.matches(languageFilter.text)

        function openAt() {
            open()
        }
        function choose(code: string) {
            Transcript.spokenLanguage = code
            close()
        }
        onOpened: {
            languageFilter.text = ""
            languageFilter.forceActiveFocus(Qt.PopupFocusReason)
            let at = 0
            for (let i = 0; i < shown.length; ++i) {
                if (shown[i].code === Transcript.spokenLanguage)
                    at = i
            }
            languageList.currentIndex = at
            languageList.positionViewAtIndex(at, ListView.Center)
        }

        Column {
            width: Theme.space(60)
            spacing: Theme.space(2)

            FieldInput {
                id: languageFilter
                objectName: "spokenLanguageFilter"
                width: parent.width
                iconName: "search"
                clearOnEscape: true
                placeholderText: qsTr("Find a language")
                Accessible.name: qsTr("Find a language")
                onTextChanged: languageList.currentIndex = picker.shown.length > 0 ? 0 : -1
                Keys.onDownPressed: languageList.currentIndex = Math.min(languageList.count - 1, languageList.currentIndex + 1)
                Keys.onUpPressed: languageList.currentIndex = Math.max(0, languageList.currentIndex - 1)
                onAccepted: {
                    const row = picker.shown[Math.max(0, languageList.currentIndex)]
                    if (row)
                        picker.choose(row.code)
                }
            }
            ListView {
                id: languageList
                objectName: "spokenLanguageList"
                width: parent.width
                height: Math.min(Theme.space(70), Math.max(Theme.controlHeight, count * Theme.controlHeight))
                clip: true
                model: picker.shown
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0
                ScrollBar.vertical: ThinScrollBar {}
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Languages")

                delegate: Rectangle {
                    id: row
                    required property var modelData
                    required property int index
                    width: ListView.view ? ListView.view.width : 0
                    height: Theme.controlHeight
                    radius: Theme.radiusField
                    color: rowHover.hovered || row.ListView.isCurrentItem ? Theme.hover : "transparent"
                    border.width: row.modelData.code === Transcript.spokenLanguage ? Theme.hairline : 0
                    border.color: Theme.accentStroke
                    Accessible.role: Accessible.ListItem
                    Accessible.name: row.modelData.tier !== ""
                                     ? qsTr("%1, %2 support").arg(row.modelData.label).arg(root.tierText(row.modelData.tier))
                                     : row.modelData.label
                    Accessible.onPressAction: picker.choose(row.modelData.code)

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.space(2)
                        anchors.rightMargin: Theme.space(2)
                        spacing: Theme.space(2)
                        Text {
                            Layout.fillWidth: true
                            text: row.modelData.label
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                        }
                        Text {
                            visible: row.modelData.tier !== ""
                            text: root.tierText(row.modelData.tier)
                            color: row.modelData.tier === "limited" ? Theme.warningText : Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                    }
                    HoverHandler {
                        id: rowHover
                    }
                    TapHandler {
                        onTapped: picker.choose(row.modelData.code)
                    }
                }
            }
            Text {
                width: parent.width
                text: qsTr("Support is how much audio of the language Whisper learned from: Strong is dependable from the Base model up, Limited needs the Medium or Large v3 model and a read-through.")
                wrapMode: Text.Wrap
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
    }
}
