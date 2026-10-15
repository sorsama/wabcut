pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The editing assistant (AgentController; docs/ai.md "Editing assistant"). Opened from the command palette's
// "Ask AI…" row. A floating, non-modal panel at the top right, so the timeline below stays visible: it shows the
// staged edit as a read-only preview (cuts struck through, new content as ghost clips) while the panel lists what
// the assistant did, step by step, and the verification report. Nothing changes the project until Accept, which
// applies the whole edit as one undo step.
//
//   header      title, data-flow badge (provider · model, or "On this computer"), close
//   request     prompt box (Enter sends, Shift+Enter breaks the line), Ask / Stop, quick actions
//   progress    phase, the model's reasoning summary, steps as edits ("Remove pauses longer than 1 s")
//   result      ✓/✗ checks, what changes, the model's answer, cost; Reject · Refine · Accept
Popup {
    id: root
    objectName: "assistantPanel"

    readonly property bool running: AgentController.busy
    readonly property bool hasResult: AgentController.state === "result" || AgentController.state === "applied"
    property bool refining: false

    parent: Overlay.overlay
    x: parent ? parent.width - width - Theme.space(4) : 0
    y: Theme.space(14)
    width: Math.min(Theme.space(112), parent ? parent.width * 0.36 : Theme.space(112))
    height: Math.min(implicitHeight, parent ? parent.height * 0.62 : implicitHeight)
    modal: false
    dim: false
    focus: true
    padding: Theme.space(4)
    closePolicy: Popup.CloseOnEscape

    onOpened: promptField.forceActiveFocus()
    onClosed: AgentController.close()

    Connections {
        target: AgentController
        function onPanelOpenChanged() {
            if (AgentController.panelOpen)
                root.open()
            else
                root.close()
        }
        function onStateChanged() {
            if (AgentController.state === "consent")
                consentDialog.ask(AgentController.consentPrompt)
            if (AgentController.state !== "result")
                root.refining = false
        }
    }

    AiConsentDialog {
        id: consentDialog
        onAllowed: always => AgentController.answerConsent(true, always)
        onDeclined: AgentController.answerConsent(false, false)
    }

    background: Rectangle {
        radius: Theme.radiusPanel
        color: Theme.panel
        border.width: Theme.hairline
        border.color: Theme.borderStrong
    }

    component BodyText: Text {
        color: Theme.textPrimary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
        wrapMode: Text.WordWrap
    }

    component SectionCaption: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
        font.letterSpacing: Theme.labelTracking
    }

    contentItem: ColumnLayout {
        spacing: Theme.space(3)

        // ---- Header (fixed) ----
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)

            Icon {
                name: "sparkles"
                color: Theme.accentStroke
            }
            BodyText {
                text: qsTr("Assistant")
                font.pixelSize: Theme.textTitle
                font.weight: Font.DemiBold
                wrapMode: Text.NoWrap
            }
            Rectangle {
                id: flowBadge
                objectName: "assistantDataFlow"
                readonly property var flow: AgentController.dataFlow
                visible: (flow.model || "") !== ""
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: badgeText.implicitWidth + Theme.space(4)
                implicitHeight: badgeText.implicitHeight + Theme.space(1)
                radius: height / 2
                color: flow.local ? Qt.alpha(Theme.success, 0.14) : Theme.hover
                border.width: Theme.hairline
                border.color: flow.local ? Qt.alpha(Theme.success, 0.5) : Theme.border
                Accessible.role: Accessible.StaticText
                Accessible.name: badgeText.text

                Text {
                    id: badgeText
                    anchors.centerIn: parent
                    text: flowBadge.flow.local ? qsTr("%1 · on this computer").arg(flowBadge.flow.model)
                                               : qsTr("%1 · %2").arg(flowBadge.flow.model).arg(flowBadge.flow.provider)
                    color: flowBadge.flow.local ? Theme.success : Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.weight: Font.DemiBold
                }
                HoverHandler {
                    id: badgeHover
                }
                Tooltip {
                    visible: badgeHover.hovered
                    text: qsTr("Sends to %1: %2").arg(flowBadge.flow.destination).arg(flowBadge.flow.sends)
                }
            }
            Item {
                Layout.fillWidth: true
            }
            IconButton {
                iconName: "x"
                tooltip: qsTr("Close")
                onClicked: root.close()
            }
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: body.implicitHeight
            implicitHeight: body.implicitHeight
            contentHeight: body.implicitHeight
            // While the steps stream in and when the report arrives, keep the newest content in view.
            readonly property bool pinToEnd: root.running || AgentController.state === "result"
            function showEnd() {
                if (pinToEnd)
                    contentY = Math.max(0, contentHeight - height)
            }
            onContentHeightChanged: showEnd()
            onHeightChanged: showEnd()
            onPinToEndChanged: showEnd()
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}

            ColumnLayout {
                id: body
                width: flick.width
                spacing: Theme.space(3)
                Accessible.role: Accessible.Pane
                Accessible.name: qsTr("Assistant")

                Text {
                    objectName: "assistantDataFlowText"
                    Layout.fillWidth: true
                    visible: (AgentController.dataFlow.model || "") !== ""
                    wrapMode: Text.WordWrap
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    text: AgentController.dataFlow.local
                          ? qsTr("Runs on this computer: nothing leaves it.")
                          : qsTr("Sends to %1: your request, a summary of the sequence and the transcript excerpts it reads. Never media files.")
                                .arg(AgentController.dataFlow.destination)
                }

                // ---- Request ----
                ScrollView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(Math.max(promptField.implicitHeight, Theme.space(16)), Theme.space(30))
                    enabled: !root.running

                    TextArea {
                        id: promptField
                        objectName: "assistantPrompt"
                        text: AgentController.prompt
                        placeholderText: root.refining ? qsTr("What should change? e.g. “keep the intro”")
                                                       : qsTr("Describe the edit, e.g. “Remove silences longer than 1 s”")
                        placeholderTextColor: Theme.textTertiary
                        color: Theme.textPrimary
                        selectionColor: Theme.accentFill
                        selectedTextColor: Theme.accentOnFill
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        wrapMode: TextArea.Wrap
                        padding: Theme.space(3)
                        selectByMouse: true
                        onTextChanged: if (!root.refining) AgentController.prompt = text
                        Keys.onShortcutOverride: event => event.accepted = true
                        Keys.onReturnPressed: event => {
                            if (event.modifiers & Qt.ShiftModifier) {
                                event.accepted = false
                                return
                            }
                            root.submit()
                        }
                        background: Rectangle {
                            radius: Theme.radiusField
                            color: Theme.raised
                            border.width: promptField.activeFocus ? Theme.focusRingWidth : Theme.hairline
                            border.color: promptField.activeFocus ? Theme.focus : Theme.borderStrong
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.running
                        spacing: Theme.space(1)
                        Text {
                            Layout.fillWidth: true
                            text: AgentController.phase
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                            elide: Text.ElideRight
                        }
                        // The AI-working light while the assistant works: the phase is named just above, so a
                        // thin sliding bar rather than a labelled loader pill (design language v3).
                        BeamProgress {
                            objectName: "assistantProgress"
                            Layout.fillWidth: true
                            indeterminate: true
                            loader: ""

                            label: AgentController.phase
                        }
                    }
                    Item {
                        Layout.fillWidth: true
                        visible: !root.running
                    }
                    PillButton {
                        objectName: "assistantStop"
                        visible: root.running
                        text: qsTr("Stop")
                        variant: "secondary"
                        onClicked: AgentController.cancel()
                    }
                    PillButton {
                        objectName: "assistantAsk"
                        visible: !root.running
                        enabled: promptField.text.trim() !== ""
                        text: root.refining ? qsTr("Refine") : qsTr("Ask")
                        iconName: "sparkles"
                        variant: "primary"
                        onClicked: root.submit()
                    }
                }

                Flow {
                    objectName: "assistantQuickActions"
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    visible: !root.running && !root.refining && AgentController.state !== "result"

                    Repeater {
                        model: AgentController.quickActions
                        delegate: PillButton {
                            required property var modelData
                            compact: true
                            variant: "ghost"
                            text: modelData.title.replace("{value}", modelData.value).replace("{track}", modelData.track)
                            tooltip: modelData.description
                            onClicked: AgentController.runQuickAction(modelData.id, modelData.value, modelData.track)
                        }
                    }
                }

                // ---- Progress ----
                Text {
                    objectName: "assistantReasoning"
                    Layout.fillWidth: true
                    visible: text !== "" && root.running
                    text: AgentController.reasoning
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.italic: true
                    wrapMode: Text.WordWrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                }

                SectionCaption {
                    visible: stepsRepeater.count > 0
                    text: qsTr("Steps")
                }

                Repeater {
                    id: stepsRepeater
                    model: AgentController.steps
                    delegate: RowLayout {
                        id: step
                        required property var modelData
                        readonly property color tint: modelData.kind === "error" ? Theme.danger
                                                    : modelData.kind === "edit" ? Theme.accentStroke
                                                    : modelData.kind === "verify" ? (modelData.ok ? Theme.success : Theme.danger)
                                                    : Theme.textTertiary
                        Layout.fillWidth: true
                        spacing: Theme.space(2)

                        Item {
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 2
                            implicitWidth: Theme.iconSm
                            implicitHeight: Theme.iconSm
                            Icon {
                                anchors.centerIn: parent
                                visible: step.modelData.kind !== "running"
                                size: Theme.iconSm
                                color: step.tint
                                name: step.modelData.kind === "read" ? "eye"
                                    : step.modelData.kind === "edit" ? "scissors"
                                    : step.modelData.kind === "error" ? "x"
                                    : step.modelData.kind === "verify" ? (step.modelData.ok ? "check" : "x")
                                    : "pencil"
                            }
                            // The step still running: the echo mark's chase loader (design language v3).
                            EchoLoader {
                                anchors.centerIn: parent
                                visible: step.modelData.kind === "running"
                                size: Theme.space(5)
                                variant: "chase"
                            }

                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            BodyText {
                                Layout.fillWidth: true
                                text: step.modelData.title
                                color: step.modelData.kind === "error" ? Theme.danger : Theme.textPrimary
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: text !== "" && step.modelData.kind !== "verify"
                                text: step.modelData.detail
                                color: Theme.textTertiary
                                font.family: Theme.fontSans
                                font.pixelSize: Theme.textSm
                                wrapMode: Text.WordWrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                // ---- Result ----
                Rectangle {
                    objectName: "assistantReport"
                    Layout.fillWidth: true
                    visible: AgentController.state === "result" && AgentController.checks.length > 0
                    implicitHeight: report.implicitHeight + Theme.space(6)
                    radius: Theme.radiusField
                    color: Theme.raised
                    border.width: Theme.hairline
                    border.color: AgentController.checksPassed ? Qt.alpha(Theme.success, 0.45) : Qt.alpha(Theme.danger, 0.55)

                    ColumnLayout {
                        id: report
                        anchors.fill: parent
                        anchors.margins: Theme.space(3)
                        spacing: Theme.space(2)

                        RowLayout {
                            spacing: Theme.space(2)
                            Icon {
                                name: AgentController.checksPassed ? "check" : "x"
                                color: AgentController.checksPassed ? Theme.success : Theme.danger
                            }
                            BodyText {
                                objectName: "assistantStatus"
                                Layout.fillWidth: true
                                text: AgentController.statusText
                                font.weight: Font.DemiBold
                            }
                        }
                        Repeater {
                            model: AgentController.checks
                            delegate: RowLayout {
                                id: check
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: Theme.space(2)
                                Text {
                                    Layout.alignment: Qt.AlignTop
                                    text: check.modelData.passed ? "✓" : "✗"
                                    color: check.modelData.passed ? Theme.success : (check.modelData.blocking ? Theme.danger : Theme.warning)
                                    font.family: Theme.fontSans
                                    font.pixelSize: Theme.textMd
                                    font.weight: Font.Bold
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    BodyText {
                                        Layout.fillWidth: true
                                        text: check.modelData.label
                                        color: check.modelData.passed ? Theme.textSecondary : Theme.textPrimary
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        visible: !check.modelData.passed && check.modelData.detail !== ""
                                        text: check.modelData.detail
                                        color: Theme.danger
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.textSm
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }
                    }
                }

                BodyText {
                    objectName: "assistantChanges"
                    Layout.fillWidth: true
                    visible: AgentController.state === "result" && text !== ""
                    text: AgentController.changeSummary !== "" ? qsTr("Changes: %1").arg(AgentController.changeSummary) : ""
                    color: Theme.textSecondary
                    font.pixelSize: Theme.textSm
                }

                BodyText {
                    objectName: "assistantAnswer"
                    Layout.fillWidth: true
                    visible: text !== "" && (root.running || root.hasResult)
                    text: AgentController.answer
                }

                BodyText {
                    objectName: "assistantMessage"
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: AgentController.stale && AgentController.state === "result"
                          ? (AgentController.message !== "" ? AgentController.message
                                                            : qsTr("The timeline changed since this preview. Accept checks the edit again on it."))
                          : AgentController.message
                    color: AgentController.state === "applied" ? Theme.success : Theme.warning
                    font.pixelSize: Theme.textSm
                }
            }
        }

        // ---- Actions: always in view, under the scrolling content ----
        RowLayout {
            Layout.fillWidth: true
            visible: AgentController.state === "result"
            spacing: Theme.space(2)

            Text {
                objectName: "assistantUsage"
                Layout.fillWidth: true
                text: AgentController.usageText
                color: Theme.textTertiary
                font.family: Theme.fontMono
                font.pixelSize: Theme.textXs
                elide: Text.ElideRight
            }
            PillButton {
                objectName: "assistantReject"
                text: qsTr("Reject")
                variant: "ghost"
                onClicked: {
                    root.refining = false
                    promptField.text = Qt.binding(() => AgentController.prompt)
                    AgentController.reject()
                }
            }
            PillButton {
                objectName: "assistantRefine"
                visible: AgentController.canRefine && !root.refining
                text: qsTr("Refine")
                variant: "secondary"
                onClicked: {
                    root.refining = true
                    promptField.text = ""
                    promptField.forceActiveFocus()
                }
            }
            PillButton {
                objectName: "assistantAccept"
                enabled: AgentController.canAccept
                text: qsTr("Accept")
                iconName: "check"
                variant: "primary"
                onClicked: AgentController.accept()
            }
        }
    }

    function submit() {
        const text = promptField.text.trim()
        if (text === "" || root.running)
            return
        if (root.refining) {
            root.refining = false
            promptField.text = Qt.binding(() => AgentController.prompt) // follow the request again
            AgentController.refine(text)
        } else {
            AgentController.ask(text)
        }
    }
}
