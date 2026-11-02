pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Dialogs

// Highlights > Find options (M5o, gaming): what highlights.find listens for (game audio, your voice, the picture,
// chat, the stream's own markers), the chat log (TwitchDownloader, yt-dlp live chat or chat-downloader JSON, picked
// from a file and read by highlights.importChat, which says what it found), how many moments and how strict. The
// panel reads options() when it finds.
// 0.6 (P1 #56): Audio streams, once a find has run or the recording has two or more streams: one row per stream (its
// title: OBS's Game / Mic/Aux / Discord...) and a role menu: Auto, Game, Your voice, Mixed or Left out. Auto is the
// find's own choice (the stream's title, else how voice-like it sounds; the button and its tooltip say which). The choices are remembered by stream
// position (Preferences.highlightsAudioRoles: a recorder keeps its track order) and sent as audioRoles when one is not
// Auto.
Popover {
    id: root
    objectName: "highlightFindOptions"

    title: qsTr("Find options")
    transformOrigin: Item.TopRight

    readonly property var counts: [5, 10, 20, 50]
    readonly property var minScores: [0.15, 0.25, 0.45]
    /// 0.6 (P1 #56): the roles a stream row offers, in its tabs' order.
    readonly property var roleIds: ["auto", "game", "mic", "mixed", "ignore"]
    /// The recording's audio streams (Highlights.audioStreams), read when the popover opens.
    property var streams: []
    /// The roles chosen, by stream (as saved).
    readonly property var savedRoles: {
        const saved = Preferences.parsedJson(Preferences.highlightsAudioRoles, [])
        const out = []
        if (Array.isArray(saved)) {
            for (let i = 0; i < saved.length; ++i)
                out.push(root.roleIds.indexOf(String(saved[i])) >= 0 ? String(saved[i]) : "auto")
        }
        return out
    }

    function refreshStreams() {
        root.streams = Highlights.audioStreams()
    }
    /// Remembers stream `index`'s role.
    function setRole(index: int, role: string) {
        const out = []
        for (let i = 0; i < root.savedRoles.length; ++i) // copied: a var property's array is not a JS array here
            out.push(String(root.savedRoles[i]))
        while (out.length <= index)
            out.push("auto")
        out[index] = role
        while (out.length > 0 && out[out.length - 1] === "auto")
            out.pop()
        Preferences.highlightsAudioRoles = out.length > 0 ? JSON.stringify(out) : ""
    }
    function roleName(role: string): string {
        return role === "game" ? qsTr("Game") : role === "mic" ? qsTr("Your voice") : role === "mixed" ? qsTr("Mixed")
             : role === "ignore" ? qsTr("Left out") : qsTr("Auto")
    }
    /// The role button's text on Auto: "Auto · Game" (the last find's role, else the title's), else "Auto".
    function autoLabel(stream: var): string {
        const found = String(stream.found ?? "")
        const role = found !== "" ? found : String(stream.named ?? "")
        return role !== "" ? qsTr("Auto · %1").arg(root.roleName(role)) : qsTr("Auto")
    }
    /// "Stream 2 · Mic/Aux" (its title, else its track name).
    function streamLabel(stream: var): string {
        const title = String(stream.title ?? "") !== "" ? String(stream.title) : String(stream.name ?? "")
        const number = Number(stream.index ?? 0) + 1
        return title !== "" ? qsTr("Stream %1 · %2").arg(number).arg(title) : qsTr("Stream %1").arg(number)
    }
    /// What Auto does for the stream: the last find's role, else the one its title gives, else "from the sound".
    function autoText(stream: var): string {
        const found = String(stream.found ?? "")
        if (found !== "")
            return qsTr("Auto: %1 (last find)").arg(root.roleName(found))
        const named = String(stream.named ?? "")
        return named !== "" ? qsTr("Auto: %1 (from the title)").arg(root.roleName(named)) : qsTr("Auto: from the sound")
    }

    onAboutToShow: root.refreshStreams()

    /// highlights.find's options, for Highlights.find().
    function options() {
        const chosen = []
        if (gameToggle.checked) chosen.push("game")
        if (micToggle.checked) chosen.push("mic")
        if (visualToggle.checked) chosen.push("visual")
        if (chatToggle.checked) chosen.push("chat")
        if (markersToggle.checked) chosen.push("markers")
        const o = { signals: chosen, count: root.counts[countTabs.currentIndex],
                    minScore: root.minScores[scoreTabs.currentIndex] }
        // 0.6 (P1 #56): the stream roles, when one is not Auto (the bridge checks each entry).
        const roles = []
        let corrected = false
        for (let i = 0; i < root.savedRoles.length; ++i) {
            roles.push(String(root.savedRoles[i]))
            corrected = corrected || roles[i] !== "auto"
        }
        if (corrected)
            o.audioRoles = roles
        return o
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

        // ---- Audio streams (0.6, P1 #56) ----
        Column {
            id: streamsGroup
            objectName: "highlightsAudioStreams"
            width: parent.width
            spacing: Theme.space(1.5)
            visible: root.streams.length >= 2 || (root.streams.length > 0 && Highlights.recordings.length > 0)

            GroupLabel { text: qsTr("Audio streams") }
            Repeater {
                model: root.streams
                delegate: Item {
                    id: streamRow
                    required property var modelData
                    required property int index
                    readonly property string role: streamRow.index < root.savedRoles.length
                                                   ? root.savedRoles[streamRow.index] : "auto"
                    objectName: "highlightsStreamRow" + streamRow.index
                    width: streamsGroup.width
                    height: roleButton.height
                    Text {
                        objectName: "highlightsStreamTitle" + streamRow.index
                        anchors.left: parent.left
                        anchors.right: roleButton.left
                        anchors.rightMargin: Theme.space(2)
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.streamLabel(streamRow.modelData)
                        elide: Text.ElideRight
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    // The role: Auto (what it does in brackets) or the user's choice, from a menu.
                    PillButton {
                        id: roleButton
                        objectName: "highlightsStreamRole" + streamRow.index
                        anchors.right: parent.right
                        width: Math.min(implicitWidth, streamsGroup.width * 0.55)
                        compact: true
                        iconName: "chevron-down"
                        text: streamRow.role === "auto" ? root.autoLabel(streamRow.modelData) : root.roleName(streamRow.role)
                        tooltip: streamRow.role === "auto" ? root.autoText(streamRow.modelData)
                                                           : qsTr("What this stream carries, for the find")
                        Accessible.name: qsTr("What %1 carries: %2").arg(root.streamLabel(streamRow.modelData))
                                                                   .arg(roleButton.text)
                        onClicked: {
                            roleMenu.options = root.roleIds.map(id => ({ id: id, name: root.roleName(id) }))
                            roleMenu.popup(roleButton, 0, roleButton.height + Theme.space(1))
                        }
                        OptionsMenu {
                            id: roleMenu
                            objectName: "highlightsStreamRoleMenu" + streamRow.index
                            current: streamRow.role
                            onChosen: id => root.setRole(streamRow.index, id)
                        }
                    }
                }
            }
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
