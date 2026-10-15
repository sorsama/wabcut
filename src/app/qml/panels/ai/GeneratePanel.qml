pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
Panel {
    id: root
    title: root.extendClip ? qsTr("Generative Extend") : qsTr("Generate")
    objectName: "generatePanel"
    function focusContent() { prompt.forceActiveFocus() }
    property string extendClip: ""
    property bool cloudAllowed: !AiController.offlineMode && !AiController.projectLock
    property var selectedModel: provider.currentIndex >= 0 ? GenerateController.models[provider.currentIndex] : null
    property string effectivePrompt: extendClip && prompt.text.trim() === "" ? qsTr("Continue this shot naturally.") : prompt.text
    property var quote: cloudAllowed && selectedModel ? GenerateController.estimate(selectedModel.id, effectivePrompt, seconds.value, aspect.currentText) : ({ok: false, error: qsTr("Offline / no-cloud is enabled")})
    onQuoteChanged: approved.checked = false
    onExtendClipChanged: { approved.checked = false; root.prepareUpload() }
    // Extend uploads one picture: render it now (as an export would show it, redactions applied) and show it before
    // consent; consent covers exactly that picture (review wave 4, P1-1).
    property string uploadError: ""
    function prepareUpload() {
        acknowledged.checked = false
        uploadError = extendClip !== "" ? GenerateController.prepareExtend(extendClip, side.currentIndex === 1) : ""
    }
    Connections {
        target: GenerateController
        function onProposalRequested(p) {
            root.extendClip = p.clip; prompt.text = p.prompt; seconds.value = p.seconds;
            aspect.currentIndex = p.aspect === "9:16" ? 1 : 0; side.currentIndex = p.head ? 1 : 0;
            place.checked = p.at >= 0; at.value = Math.max(0, p.at);
            for (let i = 0; i < GenerateController.models.length; ++i) {
                if (GenerateController.models[i].image === (p.operation === "image")) { provider.currentIndex = i; break; }
            }
            approved.checked = false;
        }
    }
    ScrollView {
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width
            spacing: Theme.space(3)
            Label { text: qsTr("Your key · estimate before every run"); color: Theme.textSecondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
            ComboBox { id: provider; Layout.fillWidth: true; model: GenerateController.models; textRole: "name"
                HoverHandler { enabled: provider.enabled; cursorShape: Qt.PointingHandCursor } }
            TextArea { id: prompt; objectName: "generationPrompt"; Layout.fillWidth: true; Layout.preferredHeight: Theme.space(18); placeholderText: qsTr("Describe the shot or background"); wrapMode: TextEdit.Wrap; color: Theme.textPrimary; font.family: Theme.fontSans; font.pixelSize: Theme.textMd; padding: Theme.space(3); background: Rectangle { color: Theme.raised; radius: Theme.radiusField; border.color: prompt.activeFocus ? Theme.focus : Theme.borderStrong } }
            GridLayout {
                columns: 2
                Layout.fillWidth: true
                Label { text: qsTr("Seconds"); color: Theme.textSecondary }
                SpinBox { id: seconds; Layout.fillWidth: true; from: 1; to: 10; value: 4; enabled: !root.selectedModel || !root.selectedModel.image }
                Label { text: qsTr("Aspect"); color: Theme.textSecondary }
                ComboBox { id: aspect; Layout.fillWidth: true; model: root.selectedModel && root.selectedModel.image ? ["16:9", "9:16", "1:1"] : ["16:9", "9:16"]
                    HoverHandler { enabled: aspect.enabled; cursorShape: Qt.PointingHandCursor } }
                Label { visible: root.extendClip !== ""; text: qsTr("Extend"); color: Theme.textSecondary }
                ComboBox { id: side; Layout.fillWidth: true; visible: root.extendClip !== ""; model: [qsTr("Tail"), qsTr("Head")]; onCurrentIndexChanged: { approved.checked = false; root.prepareUpload() }
                    HoverHandler { enabled: side.enabled; cursorShape: Qt.PointingHandCursor } }
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.extendClip === "" && (!root.selectedModel || !root.selectedModel.image)
                CheckBox { id: place; onCheckedChanged: approved.checked = false; text: qsTr("Place over timeline")
                    HoverHandler { enabled: place.enabled; cursorShape: Qt.PointingHandCursor } }
                RowLayout {
                    visible: place.checked
                    Label { text: qsTr("At second"); color: Theme.textSecondary }
                    SpinBox { id: at; Layout.fillWidth: true; from: 0; to: 86400; value: 42; onValueChanged: approved.checked = false; enabled: place.checked }
                }
            }
            Rectangle {
                objectName: "generationCostEstimate"
                Layout.fillWidth: true
                implicitHeight: priceColumn.implicitHeight + Theme.space(6)
                color: Theme.raised; radius: Theme.radiusField; border.color: Theme.border; border.width: Theme.hairline
                ColumnLayout {
                    id: priceColumn
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.space(3) }
                    Label { text: root.quote.ok ? qsTr("Estimated cost: %1").arg(root.quote.cost) : (root.quote.error || qsTr("Enter a prompt")); color: Theme.textPrimary; font.pixelSize: Theme.textLg; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    Label { text: root.quote.ok ? qsTr("Hard limit for this run: %1").arg(root.quote.cost) : ""; color: Theme.textSecondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    Label { text: root.quote.dataPolicy || ""; wrapMode: Text.Wrap; Layout.fillWidth: true; color: Theme.textSecondary }
                    Label { text: root.extendClip && side.currentIndex === 1 ? qsTr("Head extension is generated outward from the boundary, then reversed.") : ""; visible: text !== ""; wrapMode: Text.Wrap; Layout.fillWidth: true; color: Theme.textSecondary }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.extendClip !== ""
                spacing: Theme.space(2)
                Label { text: qsTr("This picture will be sent:"); color: Theme.textSecondary; Layout.fillWidth: true }
                Image {
                    objectName: "generationUploadPreview"
                    Layout.fillWidth: true
                    Layout.preferredHeight: width * 9 / 16
                    fillMode: Image.PreserveAspectFit
                    source: GenerateController.uploadPreview
                    cache: false
                }
                Label { text: root.uploadError; visible: root.uploadError !== ""; color: Theme.danger; wrapMode: Text.Wrap; Layout.fillWidth: true }
                CheckBox {
                    id: acknowledged
                    objectName: "generationRedactionAck"
                    visible: GenerateController.redactionNotice !== ""
                    text: GenerateController.redactionNotice
                    onCheckedChanged: approved.checked = false
                    Layout.fillWidth: true
                    contentItem: Text {
                        text: acknowledged.text; font: acknowledged.font; color: Theme.textPrimary
                        leftPadding: acknowledged.indicator.width + acknowledged.spacing
                        wrapMode: Text.WordWrap; verticalAlignment: Text.AlignVCenter
                    }
                    HoverHandler { enabled: acknowledged.enabled; cursorShape: Qt.PointingHandCursor }
                }
            }
            CheckBox {
                id: approved
                objectName: "generationConsent"
                text: root.extendClip !== "" ? qsTr("Send this prompt and the picture above; approve this cost")
                                             : qsTr("Send this prompt; approve this cost")
                enabled: root.quote.ok && (root.extendClip === "" || (GenerateController.uploadPreview.toString() !== ""
                                                                      && (GenerateController.redactionNotice === "" || acknowledged.checked)))
                Layout.fillWidth: true
                contentItem: Text {
                    text: approved.text; font: approved.font; color: Theme.textPrimary
                    leftPadding: approved.indicator.width + approved.spacing
                    wrapMode: Text.WordWrap; verticalAlignment: Text.AlignVCenter
                }
                HoverHandler { enabled: approved.enabled; cursorShape: Qt.PointingHandCursor }
            }
            ColumnLayout {
                Layout.fillWidth: true
                PillButton { Layout.fillWidth: true; text: qsTr("Provider terms"); enabled: !!root.quote.terms; onClicked: Qt.openUrlExternally(root.quote.terms) }
                PillButton { Layout.fillWidth: true; objectName: "generationRun"; variant: "primary"; text: qsTr("Generate preview"); enabled: root.quote.ok && approved.checked && !GenerateController.busy;
                    onClicked: GenerateController.run(root.selectedModel.id, root.effectivePrompt, seconds.value, aspect.currentText, root.quote.dollars, approved.checked, place.checked ? at.value : -1, root.extendClip, side.currentIndex === 1, acknowledged.checked) }
                PillButton { Layout.fillWidth: true; text: qsTr("Cancel"); visible: GenerateController.busy; onClicked: GenerateController.cancel() }
            }
            BeamProgress { Layout.fillWidth: true; value: GenerateController.progress; active: GenerateController.busy; visible: GenerateController.busy || GenerateController.staged }
            Label { text: GenerateController.message; color: Theme.textSecondary; wrapMode: Text.Wrap; Layout.fillWidth: true }
            Repeater {
                model: GenerateController.results
                delegate: ColumnLayout {
                    id: resultCard
                    required property var modelData
                    Layout.fillWidth: true
                    Image { source: resultCard.modelData.preview; Layout.fillWidth: true; Layout.preferredHeight: Theme.space(45); fillMode: Image.PreserveAspectFit }
                    Label { text: qsTr("AI-generated · %1").arg(resultCard.modelData.cost); color: Theme.accentStroke }
                    PillButton { text: qsTr("Open preview"); onClicked: Qt.openUrlExternally(resultCard.modelData.file) }
                }
            }
            RowLayout {
                visible: GenerateController.staged
                PillButton { objectName: "generationAccept"; variant: "primary"; text: qsTr("Accept"); onClicked: GenerateController.accept() }
                PillButton { text: qsTr("Discard"); onClicked: GenerateController.discard() }
            }
        }
    }
    // 0.1.3: AI turned off in Settings > AI: the panel says so instead of offering what it cannot run.
    Rectangle {
        objectName: "generateAiOff"
        anchors.fill: parent
        visible: !Preferences.aiEnabled
        color: Theme.panel
        radius: Theme.radiusPanel
        // Takes the clicks the panel below would get.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
        }
        Text {
            anchors.centerIn: parent
            width: Math.min(parent.width - Theme.space(8), Theme.space(80))
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("AI features are turned off. Turn them on in Settings > AI to generate clips.")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
    }
}
