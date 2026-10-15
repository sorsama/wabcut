pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// The Keyboard Trainer (M5j onboarding): Wabcut names an action ("Ripple Delete", "Add Edit", "Mark In") and you press
// its shortcut. Keys are resolved from the active key set (Trainer, over KeymapDispatcher), so imported or edited keys
// train themselves. Rounds of ten from one category or all; a streak, the time to each answer and the echo spark
// on a right answer. The best streak is kept (Preferences.trainerBest). Opened from Help, the palette and the
// welcome.
//
// While a prompt waits, the stage takes every key (it claims ShortcutOverride, so nothing in the editor runs) except
// Tab, which moves to the buttons, and Esc, which closes the trainer.
WabcutDialog {
    id: dialog
    objectName: "keyboardTrainer"

    property string category: "all"
    readonly property bool playing: Trainer.state === "asking" || Trainer.state === "wrong" || Trainer.state === "correct"
    property string message: ""

    function startRound(key) {
        dialog.category = key
        dialog.message = ""
        if (!Trainer.start(key)) {
            dialog.message = qsTr("No playable shortcuts in this category. Esc and Tab stay available for navigation.")
            return
        }
        stage.forceActiveFocus(Qt.OtherFocusReason)
    }
    // Keys the stage keeps for itself while a prompt waits.
    function captures(event) {
        if (!dialog.playing || event.key === Qt.Key_Escape)
            return false
        const plain = (event.modifiers & ~(Qt.ShiftModifier | Qt.KeypadModifier)) === 0
        return !((event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) && plain)
    }
    // "Ctrl+Shift+K" → ["Ctrl", "Shift", "K"]; macOS's "⇧⌘K" stays one cap.
    function caps(text: string): var {
        if (text === "")
            return []
        if (text === "+" || text.endsWith("++"))
            return text === "+" ? ["+"] : text.slice(0, -2).split("+").concat(["+"])
        return text.split("+")
    }

    title: qsTr("Keyboard Trainer")
    // Nine categories side by side (M5n): wider than the default dialog.
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(190), Theme.space(190))

    onAboutToShow: {
        Trainer.stop()
        Trainer.setSavedBest(Preferences.trainerBest)
        dialog.message = ""
        Qt.callLater(() => startButton.forceActiveFocus(Qt.OtherFocusReason))
    }
    onClosed: Trainer.stop()

    Connections {
        target: Trainer
        function onAnsweredCorrectly() {
            spark.play()
            advance.restart()
            if (Trainer.bestStreak > Preferences.trainerBest)
                Preferences.trainerBest = Trainer.bestStreak
        }
        function onStateChanged() {
            if (Trainer.state === "wrong" && !Theme.reduceMotion)
                shake.restart()
        }
    }
    // A right answer shows for a moment, then the next prompt comes.
    Timer {
        id: advance
        interval: Theme.reduceMotion ? 400 : 750
        onTriggered: {
            if (Trainer.state === "correct")
                Trainer.next()
        }
    }

    Text {
        Layout.fillWidth: true
        //: Keyboard Trainer; %1 = the key set's name ("Premiere Pro")
        text: qsTr("Wabcut names an action; press its shortcut. Fast answers and streaks score more. Keys come from your key set: %1.")
                  .arg(Shortcuts.presetName + (Shortcuts.modified ? qsTr(" (edited)") : ""))
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.Wrap
    }

    PillTabs {
        id: categoryTabs
        objectName: "trainerCategories"
        Layout.fillWidth: true
        compact: true
        model: Trainer.categories.map(c => c.label)
        currentIndex: Math.max(0, Trainer.categories.findIndex(c => c.key === dialog.category))
        onActivated: index => {
            const key = Trainer.categories[index].key
            if (dialog.playing)
                dialog.startRound(key)
            else
                dialog.category = key
        }
        Accessible.name: qsTr("Practice category")
    }

    // ---- The stage ----
    Rectangle {
        id: stage
        objectName: "trainerStage"
        Layout.fillWidth: true
        implicitHeight: Theme.space(60)
        radius: Theme.radiusCard
        color: Theme.sunken
        border.width: stage.activeFocus ? Theme.focusRingWidth : Theme.hairline
        border.color: stage.activeFocus ? Theme.focus : Theme.border
        clip: true
        activeFocusOnTab: dialog.playing
        Accessible.role: Accessible.Grouping
        Accessible.name: Trainer.state === "idle" ? qsTr("Keyboard Trainer")
                       : Trainer.state === "finished" ? qsTr("Round complete")
                       : qsTr("Press the shortcut for %1").arg(Trainer.prompt)
        Accessible.description: status.text

        Keys.onShortcutOverride: event => event.accepted = dialog.captures(event)
        Keys.onPressed: event => {
            if (!dialog.captures(event))
                return
            event.accepted = true
            if (!event.isAutoRepeat)
                Trainer.press(event.key, event.modifiers)
        }

        // Idle: the pitch and Start.
        ColumnLayout {
            visible: Trainer.state === "idle"
            anchors.centerIn: parent
            width: parent.width - Theme.space(16)
            spacing: Theme.space(3)

            Row {
                Layout.alignment: Qt.AlignHCenter
                spacing: Theme.space(2)
                Repeater {
                    model: ["J", "K", "L"]
                    delegate: Keycap {
                        required property string modelData
                        text: modelData
                        large: true
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Ten actions, one at a time.")
                color: Theme.textPrimary
                font.family: Theme.fontDisplay
                font.pixelSize: Theme.textTitle
                font.weight: Theme.weightTitle
                wrapMode: Text.Wrap
            }
            Text {
                objectName: "trainerBest"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                visible: Trainer.bestStreak > 0
                text: qsTr("Best streak: %1").arg(Trainer.bestStreak)
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                visible: dialog.message !== ""
                text: dialog.message
                color: Theme.warning
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                wrapMode: Text.Wrap
            }
            PillButton {
                id: startButton
                objectName: "trainerStartButton"
                Layout.alignment: Qt.AlignHCenter
                variant: "primary"
                iconName: "zap"
                text: qsTr("Start")
                onClicked: dialog.startRound(dialog.category)
                Keys.onReturnPressed: dialog.startRound(dialog.category)
                Keys.onEnterPressed: dialog.startRound(dialog.category)
            }
        }

        // Playing: the action, then what happened.
        Item {
            id: promptArea
            visible: dialog.playing
            anchors.fill: parent

            // The spark flashes behind the prompt, so the words and keys stay readable.
            BeamSpark {
                id: spark
                anchors.centerIn: parent
                width: Theme.space(48)
                height: Theme.space(24)
            }

            ColumnLayout {
                id: promptColumn
                anchors.centerIn: parent
                width: parent.width - Theme.space(16)
                spacing: Theme.space(2)
                transform: Translate {
                    id: shakeOffset
                }

                Text {
                    objectName: "trainerPromptGroup"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: Trainer.promptGroup
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Font.Medium
                }
                Text {
                    objectName: "trainerPrompt"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: Trainer.prompt
                    color: Theme.textPrimary
                    font.family: Theme.fontDisplay
                    font.pixelSize: Theme.textDisplay + 6
                    font.weight: Theme.weightTitle

                    wrapMode: Text.Wrap
                }
                // The hint, or the keys that were pressed.
                Row {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredHeight: Theme.space(11)
                    spacing: Theme.space(1)
                    Repeater {
                        model: dialog.caps(Trainer.hintShown ? Trainer.hint : Trainer.lastPressed)
                        delegate: Keycap {
                            required property string modelData
                            text: modelData
                            large: true
                            lit: Trainer.state === "correct"
                        }
                    }
                }
                Text {
                    id: status
                    objectName: "trainerStatus"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: Trainer.state === "correct"
                          //: Keyboard Trainer, after a right answer; %1 = points, %2 = seconds
                          ? qsTr("+%1 · %2 s").arg(Trainer.lastPoints).arg((Trainer.lastMs / 1000).toFixed(1))
                          : Trainer.state === "wrong"
                            ? (Trainer.lastPressedAction !== ""
                               //: %1 = the key pressed, %2 = what it does
                               ? qsTr("%1 is %2. Try again.").arg(Trainer.lastPressed).arg(Trainer.lastPressedAction)
                               : qsTr("%1 does nothing here. Try again.").arg(Trainer.lastPressed))
                            : Trainer.hintShown ? qsTr("Now press it.") : qsTr("Press its shortcut.")
                    color: Trainer.state === "correct" ? Theme.success
                         : Trainer.state === "wrong" ? Theme.warning : Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textMd
                    wrapMode: Text.Wrap
                }
            }

            // A wrong key shakes the prompt.
            SequentialAnimation {
                id: shake
                NumberAnimation { target: shakeOffset; property: "x"; to: -Theme.space(2); duration: 40 }
                NumberAnimation { target: shakeOffset; property: "x"; to: Theme.space(2); duration: 60 }
                NumberAnimation { target: shakeOffset; property: "x"; to: 0; duration: 40 }
            }
        }

        // The round's result.
        ColumnLayout {
            visible: Trainer.state === "finished"
            anchors.centerIn: parent
            width: parent.width - Theme.space(16)
            spacing: Theme.space(2)

            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Round complete")
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.Medium
            }
            Text {
                objectName: "trainerFinalScore"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("%1 points").arg(Trainer.score)
                color: Theme.textPrimary
                font.family: Theme.fontDisplay
                font.pixelSize: Theme.textDisplay + 6
                font.weight: Theme.weightTitle
            }
            Text {
                objectName: "trainerSummary"

                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                //: %1 of %2 prompts answered right on the first try; %3 = average seconds; %4 = best streak
                text: qsTr("%1 of %2 on the first try · %3 s on average · best streak %4")
                          .arg(Trainer.correct).arg(Trainer.roundLength)
                          .arg((Trainer.averageMs / 1000).toFixed(1)).arg(Trainer.bestStreak)
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textMd
                wrapMode: Text.Wrap
            }
            PillButton {
                objectName: "trainerAgainButton"
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: Theme.space(2)
                variant: "primary"
                iconName: "zap"
                text: qsTr("Play again")
                onClicked: dialog.startRound(dialog.category)
            }
        }
    }

    // Round, streak and score.
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(4)
        visible: dialog.playing

        Text {
            objectName: "trainerRound"
            text: qsTr("%1 of %2").arg(Trainer.round).arg(Trainer.roundLength)
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Item { Layout.fillWidth: true }
        Row {
            spacing: Theme.space(1)
            Icon {
                anchors.verticalCenter: parent.verticalCenter
                name: "zap"
                size: Theme.iconSm
                color: Trainer.streak > 1 ? Theme.accentStroke : Theme.textTertiary
            }
            Text {
                objectName: "trainerStreak"
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Streak %1").arg(Trainer.streak)
                color: Trainer.streak > 1 ? Theme.textPrimary : Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
                font.weight: Font.Medium
            }
        }
        Text {
            objectName: "trainerScore"
            text: qsTr("%1 points").arg(Trainer.score)
            color: Theme.textSecondary
            font.family: Theme.fontMono
            font.pixelSize: Theme.textSm
        }
    }

    footerData: [
        PillButton {
            objectName: "trainerHintButton"
            visible: dialog.playing
            enabled: Trainer.state !== "correct" && !Trainer.hintShown
            variant: "ghost"
            iconName: "circle-help"
            text: qsTr("Show the key")
            tooltip: qsTr("Half points for this one")
            onClicked: {
                Trainer.showHint()
                stage.forceActiveFocus(Qt.OtherFocusReason)
            }
        },
        PillButton {
            objectName: "trainerSkipButton"
            visible: dialog.playing
            enabled: Trainer.state !== "correct"
            variant: "ghost"
            text: qsTr("Skip")
            onClicked: {
                Trainer.next()
                stage.forceActiveFocus(Qt.OtherFocusReason)
            }
        },
        PillButton {
            objectName: "trainerCloseButton"
            variant: "secondary"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    ]
}
