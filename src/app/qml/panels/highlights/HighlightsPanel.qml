pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// ● HIGHLIGHTS (M5o, gaming; G.2 highlights.find / toReel / toShort, all on this computer): the stream's best moments
// from the game audio, your voice, the picture, chat and your stream markers. Find highlights (options in a popover:
// signals, chat log, how many, how strict) runs as a job with its progress and Cancel and marks the moments on the
// timeline (one undo step); the list is those highlight markers, so it comes back on open, a sequence switch, undo and
// redo. Each moment is a card: its frame, score and confidence, waveform, signals and reasons; Accept / Reject review
// them, Jump goes to the peak, Preview plays the range. Make highlight reel strings the accepted moments together
// (order, length, transition, music, slow motion); Make a Short makes a 1080x1920 Short of the chosen moment (split
// with the facecam or blurred fill; its tooltip says about how long, with the Short options' lead-in, tail and minimum
// length). 0.4: Make Shorts (N) makes a Short of every accepted moment (else of every moment not rejected) as one job
// and one undo step and stays on this sequence (Cancel keeps the Shorts made so far); Export all (N) then queues them
// at 1080x1920 (H.264, or HEVC with its toggle). Keyboard: Tab to the list, Up / Down between cards, A accept, R reject,
// Enter jump, Space preview. 0.5: the scissors button (and the Moments popover's Find dead time) opens Remove dead time's
// review over the panel (DeadTimeReview.qml); its × brings the moments back.
Panel {
    id: root
    objectName: "highlightsPanel"

    title: qsTr("Highlights")

    function focusContent() {
        if (Moments.deadTimeReviewing) {
            deadTimeSheet.focusContent()
            return
        }
        if (list.count > 0)
            list.forceActiveFocus(Qt.TabFocusReason)
        else
            findButton.forceActiveFocus(Qt.TabFocusReason)
    }
    /// A count in the locale's format ("18,234").
    function formatCount(n: int): string {
        return Number(n).toLocaleString(Qt.locale(), 'f', 0)
    }
    function summary(): string {
        const parts = [qsTr("Moments: %1").arg(root.formatCount(list.count))]
        if (Highlights.recordingCount > 0)
            parts.push(qsTr("Recordings: %1").arg(root.formatCount(Highlights.recordingCount)))
        if (Highlights.chatMessages > 0)
            parts.push(qsTr("Chat messages: %1").arg(root.formatCount(Highlights.chatMessages)))
        if (Highlights.acceptedCount > 0)
            parts.push(qsTr("Accepted: %1").arg(root.formatCount(Highlights.acceptedCount)))
        return parts.join(qsTr(" · "))
    }
    function find() {
        Highlights.find(findOptions.options())
    }
    /// "m:ss" of `seconds`.
    function clock(seconds: real): string {
        const s = Math.max(0, Math.round(seconds))
        return Math.floor(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + (s % 60)
    }
    /// 0.4: how long the Short of moment `rank` would be with the Short options (its range plus lead-in and tail).
    function shortLength(rank: int): real {
        const p = Highlights.shortParams(rank, shortOptions.options())
        if (p.start === undefined || p.end === undefined)
            return 0
        return Number(p.end) - Number(p.start) + Number(p.handleBefore ?? 0) + Number(p.handleAfter ?? 0)
    }
    // Card actions keep the keyboard on the list: the cards are made again when a review changes.
    function act(rank: int, action: string) {
        const keep = list.activeFocus
        Highlights.selectedRank = rank
        if (action === "accept")
            Highlights.accept(rank)
        else if (action === "reject")
            Highlights.reject(rank)
        else if (action === "jump")
            Highlights.jump(rank)
        else if (action === "preview")
            Highlights.preview(rank)
        list.syncCurrent()
        if (keep)
            list.forceActiveFocus()
    }

    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    // 0.5: Remove dead time's review, a sheet over the panel while Moments.deadTimeReviewing (DeadTimeReview.qml).
    DeadTimeReview {
        id: deadTimeSheet
        anchors.fill: parent
        visible: Moments.deadTimeReviewing
        z: 1
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(2)
        clip: true
        visible: !Moments.deadTimeReviewing

        // ---- Find ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            PillButton {
                id: findButton
                objectName: "highlightsFindButton"
                Layout.fillWidth: true
                variant: "primary"
                iconName: "zap"
                text: qsTr("Find highlights")
                enabled: Highlights.available && !Highlights.busy
                tooltip: Highlights.available ? qsTr("Finds the best moments of this sequence and marks them")
                                              : qsTr("Highlights arrive with the next engine update")
                onClicked: root.find()
            }
            IconButton {
                id: optionsButton
                objectName: "highlightsOptionsButton"
                compact: true
                iconName: "sliders-horizontal"
                checkable: true
                checked: findOptions.opened
                enabled: Highlights.available
                tooltip: qsTr("Find options: signals, chat log, how many")
                onClicked: {
                    checked = Qt.binding(() => findOptions.opened)
                    if (findOptions.opened)
                        findOptions.close()
                    else
                        findOptions.open()
                }
                HighlightFindOptions {
                    id: findOptions
                    x: optionsButton.width - width
                    y: optionsButton.height + Theme.space(2)
                }
            }
            // 0.5: Remove dead time's review (the sheet above).
            IconButton {
                objectName: "highlightsDeadTimeButton"
                visible: Moments.gamingEdits.deadTime === true
                compact: true
                iconName: "scissors"
                tooltip: qsTr("Remove dead time: review the quiet stretches before cutting them")
                onClicked: Moments.openDeadTimeReview()
            }
        }

        // ---- The job: a pill, Cancel and the UV beam ----
        ColumnLayout {
            objectName: "highlightsJob"
            Layout.fillWidth: true
            visible: Highlights.busy
            spacing: Theme.space(1)
            RowLayout {
                Layout.fillWidth: true
                StatusPill {
                    objectName: "highlightsJobPill"
                    Layout.fillWidth: true
                    label: Highlights.activity
                    detail: Highlights.progress >= 0 ? qsTr("%1 %").arg(Math.round(Highlights.progress * 100)) : ""
                    dotColor: Theme.accentFill
                }
                PillButton {
                    objectName: "highlightsCancelButton"
                    compact: true
                    text: qsTr("Cancel")
                    tooltip: Highlights.job === "shorts" ? qsTr("Stop after this Short: the Shorts made so far are kept")
                                                         : qsTr("Stop now: nothing is changed")
                    onClicked: Highlights.cancel()
                }
            }
            BeamProgress {
                objectName: "highlightsProgress"
                Layout.fillWidth: true
                value: Highlights.progress >= 0 ? Highlights.progress : 1
                indeterminate: Highlights.progress < 0
                active: Highlights.busy
                label: Highlights.activity
                loader: ""      // the status pill above already names the activity
            }
        }
        // Another sequence is current (a reel or a Short just made, a new sequence): the list stays on its moments.
        Note {
            objectName: "highlightsOtherSequence"
            visible: Highlights.otherSequence && list.count > 0
            text: qsTr("Moments of %1").arg(Highlights.sequenceName)
            color: Theme.textSecondary
            elide: Text.ElideRight
            maximumLineCount: 1
        }
        Note {
            objectName: "highlightsSummary"
            visible: list.count > 0
            text: root.summary()
            elide: Text.ElideRight
            maximumLineCount: 2
        }
        Note {
            objectName: "highlightsMessage"
            visible: Highlights.message !== "" && !Highlights.busy
            text: Highlights.message
            maximumLineCount: 4
            elide: Text.ElideRight
        }

        // ---- The moments ----
        ListView {
            id: list
            objectName: "highlightsList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Theme.space(2)
            model: Highlights.moments
            boundsBehavior: Flickable.StopAtBounds
            activeFocusOnTab: count > 0
            keyNavigationEnabled: false // the keys below move the chosen moment; the list follows it
            highlightFollowsCurrentItem: false
            currentIndex: -1
            ScrollBar.vertical: ThinScrollBar {}
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Highlights, moments: %1. Up and Down move, A accepts, R rejects, Enter jumps, Space plays")
                                 .arg(root.formatCount(count))

            function currentMoment() {
                return currentIndex >= 0 && currentIndex < count ? Highlights.moments[currentIndex] : null
            }
            function choose(i: int) {
                const moments = Highlights.moments
                if (moments.length > 0)
                    Highlights.selectedRank = moments[Math.max(0, Math.min(moments.length - 1, i))].rank
            }
            // The keyboard position follows the chosen moment (the cards are made again on every change).
            function syncCurrent() {
                const moments = Highlights.moments
                for (let i = 0; i < moments.length; ++i) {
                    if (moments[i].rank === Highlights.selectedRank) {
                        if (currentIndex !== i)
                            currentIndex = i
                        positionViewAtIndex(i, ListView.Contain)
                        return
                    }
                }
                currentIndex = moments.length > 0 ? 0 : -1
            }
            onActiveFocusChanged: {
                if (activeFocus && currentIndex < 0 && count > 0)
                    syncCurrent()
            }
            Connections {
                target: Highlights
                function onMomentsChanged() {
                    Qt.callLater(list.syncCurrent)
                }
                function onSelectedRankChanged() {
                    list.syncCurrent()
                }
            }

            // The list keeps its keys from the editor's shortcuts (Up / Down are edit points, Space plays, A and R tools).
            Keys.onShortcutOverride: event => {
                event.accepted = (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier
                    && [Qt.Key_Up, Qt.Key_Down, Qt.Key_Home, Qt.Key_End, Qt.Key_A, Qt.Key_R, Qt.Key_Return, Qt.Key_Enter,
                        Qt.Key_Space].includes(event.key)
            }
            Keys.onPressed: event => {
                if ((event.modifiers & ~Qt.KeypadModifier) !== Qt.NoModifier)
                    return
                const moves = { [Qt.Key_Up]: list.currentIndex - 1, [Qt.Key_Down]: list.currentIndex + 1,
                                [Qt.Key_Home]: 0, [Qt.Key_End]: list.count - 1 }
                const to = moves[event.key]
                if (to !== undefined) {
                    list.choose(to)
                    event.accepted = true
                    return
                }
                const m = list.currentMoment()
                if (!m)
                    return
                const actions = { [Qt.Key_A]: "accept", [Qt.Key_R]: "reject", [Qt.Key_Return]: "jump",
                                  [Qt.Key_Enter]: "jump", [Qt.Key_Space]: "preview" }
                const action = actions[event.key]
                if (action !== undefined) {
                    root.act(m.rank, action)
                    event.accepted = true
                }
            }

            delegate: HighlightCard {
                width: list.width - Theme.space(2)
                x: Theme.space(1)
                current: ListView.isCurrentItem
                keyboardFocus: list.activeFocus
                onChosen: {
                    Highlights.selectedRank = rank
                    list.forceActiveFocus()
                }
                onAcceptRequested: root.act(rank, "accept")
                onRejectRequested: root.act(rank, "reject")
                onJumpRequested: root.act(rank, "jump")
                onPreviewRequested: root.act(rank, "preview")
            }
            header: Item { width: 1; height: Theme.space(1) }

            EmptyState {
                objectName: "highlightsEmpty"
                anchors.centerIn: parent
                width: parent.width - Theme.space(4)
                visible: list.count === 0 && !Highlights.busy
                illustration: "markers"
                text: !Highlights.available
                      ? qsTr("Highlights arrive with the next engine update: the best moments of a stream, from the game audio, your voice, the picture and chat.")
                      : Highlights.searched
                        ? qsTr("No moment stood out. Listen for more signals, add the chat log or lower the minimum score.")
                        : qsTr("Find the moments worth clipping: game audio spikes, your reactions, what happens on screen and chat.")
                actionText: !Highlights.available ? "" : Highlights.searched ? qsTr("Find options") : qsTr("Find highlights")
                actionIcon: Highlights.searched ? "sliders-horizontal" : "zap"
                onActionTriggered: {
                    if (Highlights.searched)
                        findOptions.open()
                    else
                        root.find()
                }
            }
        }

        // ---- Reel and Short ----
        ColumnLayout {
            objectName: "highlightsFooter"
            Layout.fillWidth: true
            visible: list.count > 0
            spacing: Theme.space(1)
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                PillButton {
                    id: reelButton
                    objectName: "highlightsReelButton"
                    Layout.fillWidth: true
                    variant: "primary"
                    iconName: "film"
                    text: qsTr("Make highlight reel (%1)").arg(Highlights.reelCount)
                    enabled: Highlights.reelAvailable && !Highlights.busy && Highlights.reelCount > 0
                    tooltip: !Highlights.reelAvailable ? qsTr("Highlight reels arrive with the next engine update")
                             : Highlights.acceptedCount > 0 ? qsTr("A new sequence of the accepted moments")
                                                            : qsTr("A new sequence of every moment not rejected")
                    onClicked: Highlights.makeReel(reelOptions.options())
                }
                IconButton {
                    id: reelOptionsButton
                    objectName: "highlightsReelOptionsButton"
                    compact: true
                    iconName: "sliders-horizontal"
                    checkable: true
                    checked: reelOptions.opened
                    tooltip: qsTr("Reel options: order, length, transition, music, slow motion")
                    onClicked: {
                        checked = Qt.binding(() => reelOptions.opened)
                        if (reelOptions.opened)
                            reelOptions.close()
                        else
                            reelOptions.open()
                    }
                    HighlightReelOptions {
                        id: reelOptions
                        x: reelOptionsButton.width - width
                        y: -height - Theme.space(2)
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                PillButton {
                    objectName: "highlightsShortButton"
                    Layout.fillWidth: true
                    iconName: "clapperboard"
                    text: Highlights.selectedRank > 0 ? qsTr("Make a Short of #%1").arg(Highlights.selectedRank) : qsTr("Make a Short")
                    enabled: Highlights.shortAvailable && !Highlights.busy && Highlights.selectedRank > 0
                    tooltip: !Highlights.shortAvailable ? qsTr("Shorts from highlights arrive with the next engine update")
                             : Highlights.selectedRank > 0
                               ? qsTr("A vertical 1080x1920 Short of #%1, about %2").arg(Highlights.selectedRank)
                                     .arg(root.clock(root.shortLength(Highlights.selectedRank)))
                               : qsTr("A vertical 1080x1920 Short of the chosen moment, laid out for gameplay")
                    onClicked: Highlights.makeShort(Highlights.selectedRank, shortOptions.options())
                }
                IconButton {
                    id: shortOptionsButton
                    objectName: "highlightsShortOptionsButton"
                    compact: true
                    iconName: "sliders-horizontal"
                    checkable: true
                    checked: shortOptions.opened
                    tooltip: qsTr("Short options: layout, facecam, length, captions")
                    onClicked: {
                        checked = Qt.binding(() => shortOptions.opened)
                        if (shortOptions.opened)
                            shortOptions.close()
                        else
                            shortOptions.open()
                    }
                    HighlightShortOptions {
                        id: shortOptions
                        x: shortOptionsButton.width - width
                        y: -height - Theme.space(2)
                    }
                }
            }
            // 0.4: a Short of every moment the reel would use, in one go (the Short options above apply).
            RowLayout {
                objectName: "highlightsShortsRow"
                Layout.fillWidth: true
                spacing: Theme.space(1)
                PillButton {
                    objectName: "highlightsShortsButton"
                    Layout.fillWidth: true
                    iconName: "layers"
                    text: qsTr("Make Shorts (%1)").arg(Highlights.shortsCount)
                    enabled: Highlights.shortAvailable && !Highlights.busy && Highlights.shortsCount > 1
                    tooltip: !Highlights.shortAvailable ? qsTr("Shorts from highlights arrive with the next engine update")
                             : Highlights.shortsCount <= 1 ? qsTr("One moment: Make a Short makes it")
                             : qsTr("A vertical Short of each accepted moment (else of every moment not rejected), in one undo step; the timeline stays on this sequence")
                    onClicked: Highlights.makeShorts(shortOptions.options())
                }
            }
            // 0.4: after a batch, its Shorts (those still in the project) into the export queue at 1080x1920.
            RowLayout {
                objectName: "highlightsExportRow"
                Layout.fillWidth: true
                visible: Highlights.madeShortsCount > 0
                spacing: Theme.space(1)
                Toggle {
                    id: hevcToggle
                    objectName: "highlightsExportHevc"
                    text: qsTr("HEVC")
                    tooltip: qsTr("HEVC (H.265) instead of H.264: smaller files, slower to encode")
                }
                PillButton {
                    objectName: "highlightsExportShortsButton"
                    Layout.fillWidth: true
                    iconName: "upload"
                    text: qsTr("Export all (%1)").arg(Highlights.madeShortsCount)
                    enabled: !Highlights.busy
                    tooltip: qsTr("Queue each Short made by Make Shorts for export at 1080x1920")
                    onClicked: Highlights.exportShorts(hevcToggle.checked)
                }
            }
        }
    }
}
