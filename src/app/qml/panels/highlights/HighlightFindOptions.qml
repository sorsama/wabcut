pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs

// Highlights > Find options (M5o, gaming): what highlights.find listens for (game audio, your voice, the picture,
// chat, the stream's own markers), the chat log (TwitchDownloader, yt-dlp live chat or chat-downloader JSON, picked
// from a file and read by highlights.importChat, which says what it found), how many moments and how strict. The
// panel reads options() when it finds.
Popover {
    id: root
    objectName: "highlightFindOptions"

    title: qsTr("Find options")
    transformOrigin: Item.TopRight

    readonly property var counts: [5, 10, 20, 50]
    readonly property var minScores: [0.15, 0.25, 0.45]

    /// highlights.find's options, for Highlights.find().
    function options() {
        const chosen = []
        if (gameToggle.checked) chosen.push("game")
        if (micToggle.checked) chosen.push("mic")
        if (visualToggle.checked) chosen.push("visual")
        if (chatToggle.checked) chosen.push("chat")
        if (markersToggle.checked) chosen.push("markers")
        return { signals: chosen, count: root.counts[countTabs.currentIndex], minScore: root.minScores[scoreTabs.currentIndex] }
    }

    /// "Twitch chat · Messages: 18,234 · Gameplay.mkv" from the import's result, or "".
    function chatText(): string {
        const chat = Highlights.chat
        if (chat.messages === undefined)
            return ""
        const kind = chat.format === "twitch" ? qsTr("Twitch chat") : chat.format === "youtube" ? qsTr("YouTube chat")
                                                                                                 : qsTr("Chat")
        const parts = [kind, qsTr("Messages: %1").arg(Number(chat.messages).toLocaleString(Qt.locale(), 'f', 0))]
        if ((chat.name ?? "") !== "")
            parts.push(chat.name)
        return parts.join(qsTr(" · "))
    }

    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }

    Column {
        width: Theme.space(72)
        spacing: Theme.space(2)

        GroupLabel { text: qsTr("Listen for") }
        Flow {
            width: parent.width
            spacing: Theme.space(2)
            Toggle {
                id: gameToggle
                objectName: "highlightsSignalGame"
                text: qsTr("Game audio")
                checked: true
                tooltip: qsTr("Explosions, kills, crowd: the game's audio jumping over the moments before")
            }
            Toggle {
                id: micToggle
                objectName: "highlightsSignalMic"
                text: qsTr("Your voice")
                checked: true
                tooltip: qsTr("You shouting or laughing on the microphone track")
            }
            Toggle {
                id: visualToggle
                objectName: "highlightsSignalVisual"
                text: qsTr("On screen")
                checked: true
                tooltip: qsTr("Flashes, fast motion and kill feeds in the picture (the slow part: it decodes the video)")
            }
            Toggle {
                id: chatToggle
                objectName: "highlightsSignalChat"
                text: qsTr("Chat")
                checked: true
                tooltip: qsTr("Chat getting busy, and its emotes, from the recording's imported chat log")
            }
            Toggle {
                id: markersToggle
                objectName: "highlightsSignalMarkers"
                text: qsTr("Stream markers")
                checked: true
                tooltip: qsTr("Markers and chapters you set while streaming (OBS, Twitch)")
            }
        }

        GroupLabel { text: qsTr("Chat log") }
        Row {
            width: parent.width
            spacing: Theme.space(2)
            PillButton {
                id: chatButton
                objectName: "highlightsChatFileButton"
                width: parent.width - offsetField.width - parent.spacing
                compact: true
                iconName: "message-square"
                text: Highlights.chatFile !== "" ? Highlights.chatFileName : qsTr("Import a chat log…")
                enabled: Highlights.chatAvailable && !Highlights.busy
                tooltip: Highlights.chatAvailable
                         ? qsTr("A chat replay saved as JSON (TwitchDownloader, yt-dlp live chat or chat-downloader), kept with the recording")
                         : qsTr("Chat logs arrive with the next engine update")
                onClicked: {
                    chatDialog.active = true;
                    (chatDialog.item as FileDialog)?.open()
                }
            }
            NumberField {
                id: offsetField
                objectName: "highlightsChatOffset"
                width: Theme.numberFieldWidth
                from: -86400
                to: 86400
                stepSize: 0.5
                decimals: 1
                suffix: qsTr("s")
                value: Highlights.chatOffset
                enabled: Highlights.chatAvailable
                Accessible.name: qsTr("Chat offset: chat time plus this is the recording's time")
                onValueModified: v => Highlights.chatOffset = v
            }
        }
        Text {
            objectName: "highlightsChatSummary"
            width: parent.width
            text: root.chatText() !== "" ? root.chatText() : qsTr("No chat log imported in this session")
            color: root.chatText() !== "" ? Theme.textSecondary : Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
            wrapMode: Text.Wrap
        }

        GroupLabel { text: qsTr("Moments") }
        PillTabs {
            id: countTabs
            objectName: "highlightsCountTabs"
            width: parent.width
            compact: true
            model: root.counts.map(n => ({ label: qsTr("%1").arg(n) }))
            currentIndex: 1
            Accessible.name: qsTr("How many moments")
        }

        GroupLabel { text: qsTr("Minimum score") }
        PillTabs {
            id: scoreTabs
            objectName: "highlightsScoreTabs"
            width: parent.width
            compact: true
            model: [{ label: qsTr("Loose") }, { label: qsTr("Balanced") }, { label: qsTr("Strict") }]
            currentIndex: 1
            Accessible.name: qsTr("Minimum score")
        }
    }

    // The chat log picker, made on first use; highlights.importChat reads what it picks.
    Loader {
        id: chatDialog
        active: false
        sourceComponent: FileDialog {
            objectName: "highlightsChatFileDialog"
            title: qsTr("Import a chat log")
            fileMode: FileDialog.OpenFile
            nameFilters: Highlights.chatFileFilters
            onAccepted: Highlights.importChat(selectedFile.toString())
        }
    }
}
