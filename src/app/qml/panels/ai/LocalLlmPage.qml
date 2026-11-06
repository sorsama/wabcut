pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Settings > AI > Local models (r07, docs/ai.md "Local model library"): AI that runs on this computer, so nothing you
// ask leaves it. The entry point of the local tab (the Settings window loads it by this name).
//   - The status line: the local assistant (ready, loading, stopped; Stop) and what this computer has to run models.
//   - Downloads (while any): progress, Pause / Resume, Cancel.
//   - Recommended: the curated assistant models, the best one for this computer first; one click downloads and starts.
//   - Browse: Hugging Face's GGUF models with sorting (fits this computer, benchmark score, downloads, likes, trending,
//     newest, updated, smallest) and filters; a model's card picks the quantization and shows its benchmarks.
//   - Transcription: the whisper.cpp model local transcription uses (automatic by default).
//   - Downloaded: the storage folder and every downloaded model (Use, Remove, advanced launch settings).
// Offline mode and the project's no-cloud lock stop searches and downloads; what is downloaded keeps working.
ScrollView {
    id: page
    objectName: "localLlmPage"

    /// 0 Recommended, 1 Browse, 2 Transcription, 3 Downloaded.
    property alias section: tabs.currentIndex
    property string notice: ""

    contentWidth: availableWidth
    ScrollBar.vertical: ThinScrollBar {}

    /// Shows a section with the section tabs at the top of the view (the Settings search, tests).
    function showSection(index: int) {
        tabs.currentIndex = index
        const flick = page.contentItem as Flickable
        if (flick)
            flick.contentY = Math.max(0, Math.min(tabs.y, flick.contentHeight - flick.height))
    }

    // The model list is cached (probing the GPUs is not free): read it again when the page opens.
    Component.onCompleted: {
        AiController.refreshLocalModels()
        LocalModels.refresh()
    }
    onVisibleChanged: if (visible) LocalModels.refresh()

    Connections {
        target: LocalModels
        function onNotice(message: string) {
            page.notice = message
            noticeTimer.restart()
        }
        function onDownloadFinished(id: string, ok: bool, message: string) {
            page.notice = message
            noticeTimer.restart()
        }
    }
    Timer {
        id: noticeTimer
        interval: 8000
        onTriggered: page.notice = ""
    }

    Column {
        id: column
        width: page.availableWidth
        spacing: Theme.space(4)
        bottomPadding: Theme.space(6)

        Column {
            width: parent.width
            spacing: Theme.space(1)

            Text {
                text: qsTr("Local models")
                color: Theme.textPrimary
                font.family: Theme.fontDisplay
                font.pixelSize: Theme.textTitle
                font.weight: Font.DemiBold
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("AI that runs on this computer: your prompts, footage and audio stay here. Download a model once; after that it works offline.")
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }

        // ---- Status ----
        Flow {
            width: parent.width
            spacing: Theme.space(2)

            StatusPill {
                objectName: "localAssistantStatus"
                label: qsTr("Local assistant")
                detail: AiController.localStatus !== "" ? AiController.localStatus : qsTr("Not running")
                dotColor: AiController.localBusy ? Theme.warning : Theme.accentStroke
                width: Math.min(implicitWidth, column.width)
            }
            PillButton {
                objectName: "localAssistantStop"
                compact: true
                variant: "ghost"
                text: qsTr("Stop")
                Accessible.name: qsTr("Stop the local assistant")
                onClicked: AiController.stopLocal()
            }
            StatusPill {
                objectName: "localMachine"
                label: qsTr("This computer")
                detail: LocalModels.machine
                dotColor: Theme.info
                width: Math.min(implicitWidth, column.width)
            }
        }

        Rectangle {
            objectName: "localOfflineBanner"
            visible: LocalModels.offline
            width: parent.width
            implicitHeight: offlineText.implicitHeight + Theme.space(4)
            radius: Theme.radiusField
            color: Theme.raised
            border.width: Theme.hairline
            border.color: Theme.warning
            Text {
                id: offlineText
                anchors.fill: parent
                anchors.margins: Theme.space(2)
                wrapMode: Text.WordWrap
                text: qsTr("Offline mode or the project's no-cloud lock is on: Wabcut does not search or download models now. Downloaded models keep working.")
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }

        Text {
            objectName: "localNotice"
            visible: page.notice !== ""
            width: parent.width
            wrapMode: Text.WordWrap
            text: page.notice
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }

        LocalDownloads {
            width: parent.width
        }

        PillTabs {
            id: tabs
            objectName: "localSections"
            width: Math.min(parent.width, Theme.space(150))
            model: [qsTr("Recommended"), qsTr("Browse"), qsTr("Transcription"),
                    LocalModels.installed.length > 0 ? qsTr("Downloaded (%1)").arg(LocalModels.installed.length) : qsTr("Downloaded")]
            Accessible.name: qsTr("Local model sections")
        }

        Loader {
            width: parent.width
            active: tabs.currentIndex === 0
            visible: active
            sourceComponent: LocalRecommended {
                width: column.width
                onOpenDetails: detail.show()
            }
        }
        Loader {
            width: parent.width
            active: tabs.currentIndex === 1
            visible: active
            sourceComponent: LocalBrowse {
                width: column.width
                onOpenDetails: detail.show()
            }
        }
        Loader {
            width: parent.width
            active: tabs.currentIndex === 2
            visible: active
            sourceComponent: LocalTranscription {
                width: column.width
            }
        }
        Loader {
            width: parent.width
            active: tabs.currentIndex === 3
            visible: active
            sourceComponent: LocalInstalled {
                width: column.width
            }
        }
    }

    LocalModelDetail {
        id: detail
    }
}
