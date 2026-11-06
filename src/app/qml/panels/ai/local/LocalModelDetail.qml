pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// A model's card in the local model library (LocalModels.detail): name and publisher, facts (parameters, context,
// architecture, downloads, likes, updated, licence), the quantizations with their download size and how each would run
// on this computer, the vision projector, the published benchmarks with their source, and the licence: a licence
// outside the open ones (MIT, Apache 2.0, BSD, ...) is named and must be accepted before the download. Download,
// Download and use, Use (when it is there), the Hugging Face page. Scrolls inside the window at any size.
Popup {
    id: root
    objectName: "localModelDetail"

    readonly property var d: LocalModels.detail
    property string quant: ""
    property bool accepted: false
    property bool withVision: true
    property string message: ""

    readonly property var variant: (root.d.variants ?? []).find(v => v.quant === root.quant) ?? null
    readonly property bool installed: root.d.kind === "hub" ? (root.variant?.installed ?? false) : root.d.installed === true
    readonly property var download: LocalModels.downloads.find(x => x.id === (root.d.kind === "hub" ? (root.variant?.id ?? "") : (root.d.id ?? ""))) ?? null

    function show() {
        root.message = ""
        root.accepted = false
        root.withVision = true
        open()
    }

    function startDownload(use: bool) {
        let error = ""
        if (root.d.kind === "hub")
            error = LocalModels.download(root.d.repo, root.quant, root.accepted, root.withVision, use)
        else if (root.d.kind === "curated")
            error = LocalModels.downloadCurated(root.d.id, use)
        root.message = error
    }

    // The default quantization each time another model opens.
    Connections {
        target: LocalModels
        function onDetailChanged() {
            if (!(root.d.variants ?? []).some(v => v.quant === root.quant))
                root.quant = root.d.defaultQuant ?? ""
        }
    }

    parent: Overlay.overlay
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    width: Math.min(Theme.space(190), (parent ? parent.width : Theme.windowDefaultWidth) - Theme.space(8))
    height: Math.min(implicitHeight, (parent ? parent.height : Theme.windowDefaultHeight) - Theme.space(8))
    modal: true
    focus: true
    padding: Theme.space(5)
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onClosed: LocalModels.closeModel()

    Overlay.modal: Rectangle {
        color: Theme.scrim
    }

    background: Rectangle {
        radius: Theme.radiusPanel
        color: Theme.panel
        border.width: Theme.hairline
        border.color: Theme.borderStrong
    }

    // The card scrolls; its actions stay at the bottom.
    contentItem: Item {
        implicitWidth: content.implicitWidth
        implicitHeight: content.implicitHeight + footer.implicitHeight + Theme.space(3)
        Accessible.role: Accessible.Dialog
        Accessible.name: root.d.name ?? qsTr("Model")

        Flickable {
            id: flick
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: footer.top
            anchors.bottomMargin: Theme.space(3)
            contentHeight: content.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            ScrollBar.vertical: ThinScrollBar {}

            Column {
                id: content
                width: flick.width - Theme.space(2)
                spacing: Theme.space(4)

                // ---- Title ----
                Item {
                    width: parent.width
                    height: Math.max(titleColumn.implicitHeight, closeButton.height)

                    Column {
                        id: titleColumn
                        anchors.left: parent.left
                        anchors.right: closeButton.left
                        anchors.rightMargin: Theme.space(3)
                        spacing: Theme.space(1)

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: root.d.name ?? ""
                            color: Theme.textPrimary
                            font.family: Theme.fontDisplay
                            font.pixelSize: Theme.textTitle
                            font.weight: Font.DemiBold
                            Accessible.role: Accessible.Heading
                            Accessible.name: text
                        }
                        Text {
                            width: parent.width
                            wrapMode: Text.WrapAnywhere
                            text: (root.d.repo ?? "") + ((root.d.revision ?? "") !== "" ? " @ " + root.d.revision : "")
                            color: Theme.textTertiary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                    }
                    IconButton {
                        id: closeButton
                        objectName: "localDetailClose"
                        anchors.right: parent.right
                        iconName: "x"
                        tooltip: qsTr("Close")
                        Accessible.name: qsTr("Close")
                        onClicked: root.close()
                    }
                }

                LoaderPill {
                    visible: root.d.loading === true
                    text: qsTr("Reading the model from Hugging Face")
                }

                Text {
                    objectName: "localDetailError"
                    visible: (root.d.error ?? "") !== ""
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: root.d.error ?? ""
                    color: Theme.warningText
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }

                // ---- Facts ----
                Flow {
                    width: parent.width
                    spacing: Theme.space(1.5)

                    LocalTag {
                        visible: (root.d.publisherKind ?? "") === "official"
                        text: qsTr("Official")
                    }
                    LocalTag {
                        visible: (root.d.publisherKind ?? "") === "quantizer"
                        text: qsTr("Well-known publisher")
                    }
                    LocalTag {
                        visible: (root.d.params ?? "") !== ""
                        text: qsTr("%1 parameters").arg(root.d.params ?? "")
                    }
                    LocalTag {
                        visible: (root.d.contextLength ?? 0) > 0
                        text: qsTr("%1K context").arg(Math.round((root.d.contextLength ?? 0) / 1024))
                    }
                    LocalTag {
                        visible: (root.d.architecture ?? "") !== ""
                        text: root.d.architecture ?? ""
                    }
                    LocalTag {
                        visible: (root.d.downloadsText ?? "") !== ""
                        text: qsTr("%1 downloads").arg(root.d.downloadsText ?? "")
                    }
                    LocalTag {
                        visible: (root.d.likesText ?? "") !== ""
                        text: qsTr("%1 likes").arg(root.d.likesText ?? "")
                    }
                    LocalTag {
                        visible: (root.d.updated ?? "") !== ""
                        text: qsTr("Updated %1").arg(root.d.updated ?? "")
                    }
                    LocalTag {
                        visible: (root.d.taskNames ?? "") !== ""
                        text: root.d.taskNames ?? ""
                    }
                }

                Text {
                    visible: (root.d.note ?? "") !== ""
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: root.d.note ?? ""
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                Text {
                    visible: (root.d.baseModel ?? "") !== "" && (root.d.relation ?? "") !== "" && root.d.relation !== "quantized"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("Based on %1 (%2): its own results may differ from the original's.").arg(root.d.baseModel ?? "").arg(root.d.relation ?? "")
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }

                // ---- Quantizations ----
                Column {
                    visible: (root.d.variants ?? []).length > 0
                    width: parent.width
                    spacing: Theme.space(2)

                    Text {
                        text: qsTr("Quantization")
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.Bold
                        Accessible.role: Accessible.Heading
                        Accessible.name: text
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: qsTr("Fewer bits: a smaller download that needs less memory, and a little less accuracy. Q4_K_M is a good balance.")
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }

                    Repeater {
                        model: root.d.variants ?? []

                        delegate: AbstractButton {
                            id: option
                            required property var modelData
                            readonly property bool current: modelData.quant === root.quant

                            objectName: "localVariant_" + modelData.quant
                            width: content.width
                            implicitHeight: optionColumn.implicitHeight + Theme.space(3)
                            hoverEnabled: true
                            Accessible.role: Accessible.RadioButton
                            Accessible.checkable: true
                            Accessible.checked: option.current
                            Accessible.name: qsTr("%1, %2").arg(modelData.quant).arg(modelData.size)
                            onClicked: root.quant = modelData.quant

                            background: Rectangle {
                                radius: Theme.radiusField
                                color: option.current ? Theme.hover : option.hovered ? Theme.raised : "transparent"
                                border.width: option.current ? 2 : Theme.hairline
                                border.color: option.current ? Theme.accentStroke : Theme.border
                                FocusRing {
                                    targetRadius: Theme.radiusField
                                    visible: option.visualFocus
                                }
                            }
                            contentItem: Column {
                                id: optionColumn
                                leftPadding: Theme.space(3)
                                rightPadding: Theme.space(3)
                                topPadding: Theme.space(1.5)
                                spacing: Theme.space(0.5)

                                Flow {
                                    width: option.width - Theme.space(6)
                                    spacing: Theme.space(2)
                                    Text {
                                        height: Theme.space(5.5)
                                        verticalAlignment: Text.AlignVCenter
                                        text: option.modelData.quant
                                        color: Theme.textPrimary
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.textSm
                                        font.weight: Font.Bold
                                    }
                                    Text {
                                        height: Theme.space(5.5)
                                        verticalAlignment: Text.AlignVCenter
                                        text: option.modelData.size + (option.modelData.files > 1 ? qsTr(" in %1 files").arg(option.modelData.files) : "")
                                        color: Theme.textSecondary
                                        font.family: Theme.fontSans
                                        font.pixelSize: Theme.textSm
                                    }
                                    LocalTag {
                                        visible: option.modelData.isDefault
                                        text: qsTr("Suggested")
                                    }
                                    LocalTag {
                                        visible: option.modelData.installed
                                        text: qsTr("Downloaded")
                                        tone: "success"
                                    }
                                    LocalTag {
                                        text: option.modelData.fit === "gpu" ? qsTr("GPU") : option.modelData.fit === "cpu" ? qsTr("CPU, slower") : qsTr("Too big")
                                        dot: option.modelData.fit === "gpu" ? Theme.success : option.modelData.fit === "cpu" ? Theme.warning : Theme.danger
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        objectName: "localDetailFit"
                        visible: root.variant !== null
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: !root.variant ? ""
                              : qsTr("On this computer: %1").arg(root.variant.fitText) + "
"
                                + (root.variant.vram !== "" ? qsTr("Needs about %1 of GPU memory and %2 of RAM.").arg(root.variant.vram).arg(root.variant.ram)
                                                            : qsTr("Needs about %1 of RAM.").arg(root.variant.ram))
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }

                    Row {
                        visible: root.d.projector !== undefined && root.d.projector !== null && root.d.kind === "hub"
                        spacing: Theme.space(2)
                        Toggle {
                            objectName: "localWithVision"
                            checked: root.withVision
                            Accessible.name: qsTr("Also download the image reader")
                            onToggled: root.withVision = checked
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Also download the image reader (%1): the assistant can look at frames").arg(root.d.projector ? root.d.projector.size : "")
                            color: Theme.textSecondary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                    }
                }

                // ---- Benchmarks ----
                Column {
                    width: parent.width
                    spacing: Theme.space(2)

                    Text {
                        text: qsTr("Published benchmarks")
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.Bold
                        Accessible.role: Accessible.Heading
                        Accessible.name: text
                    }
                    BenchmarkBars {
                        objectName: "localDetailBenchmarks"
                        width: parent.width
                        scores: root.d.scores ?? []
                        source: root.d.scoreSource ?? ""
                        checked: root.d.scoreDate ?? ""
                        family: root.d.scoreFamily ?? ""
                        note: root.d.scoreNote ?? ""
                    }
                }

                // ---- Licence ----
                Column {
                    width: parent.width
                    spacing: Theme.space(2)

                    Text {
                        text: qsTr("Licence")
                        color: Theme.textPrimary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textMd
                        font.weight: Font.Bold
                        Accessible.role: Accessible.Heading
                        Accessible.name: text
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: root.d.licenseNeedsAcceptance === true
                              ? qsTr("%1. This is not one of the open licences Wabcut downloads without asking: read its terms on the model's page before you use it.").arg(root.d.licenseName ?? "")
                              : (root.d.card ?? "") !== "" ? root.d.card : (root.d.licenseName ?? "")
                        color: Theme.textSecondary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                    Row {
                        visible: root.d.licenseNeedsAcceptance === true && !root.installed
                        spacing: Theme.space(2)
                        Toggle {
                            objectName: "localAcceptLicense"
                            checked: root.accepted
                            Accessible.name: qsTr("I accept the licence")
                            onToggled: root.accepted = checked
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("I read and accept %1").arg(root.d.licenseName ?? "")
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                    }
                    Text {
                        visible: root.d.gated === true
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: LocalModels.hasHubToken ? qsTr("Gated model: your Hugging Face access token is used for it. Accept its terms on its page first.")
                                                      : qsTr("Gated model: accept its terms on its Hugging Face page, then add your Hugging Face access token in Settings > AI > Providers.")
                        color: Theme.warningText
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textSm
                    }
                }

                // ---- Progress and actions ----
                Column {
                    visible: root.download !== null
                    width: parent.width
                    spacing: Theme.space(1)
                    BeamProgress {
                        width: parent.width
                        value: root.download ? root.download.progress : 0
                        active: root.download !== null && root.download.status === "running"
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: !root.download ? "" : root.download.status === "failed" ? root.download.error
                              : [root.download.receivedText, root.download.speedText, root.download.etaText].filter(s => s !== "").join(" · ")
                        color: root.download && root.download.status === "failed" ? Theme.dangerText : Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                }

            }
        }

        Column {
            id: footer
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            spacing: Theme.space(2)

            Text {
                objectName: "localDetailMessage"
                visible: root.message !== ""
                width: parent.width
                wrapMode: Text.WordWrap
                text: root.message
                color: Theme.warningText
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }

            Flow {
                width: parent.width
                spacing: Theme.space(2)

                readonly property bool busy: root.download !== null && root.download.status !== "failed" && root.download.status !== "done"
                readonly property bool canDownload: !root.installed && !busy && (root.d.kind === "curated" || root.variant !== null)
                                                    && !LocalModels.offline
                                                    && (root.d.licenseNeedsAcceptance !== true || root.accepted)

                PillButton {
                    objectName: "localDetailDownloadUse"
                    visible: !root.installed
                    variant: "primary"
                    iconName: "download"
                    text: qsTr("Download and use")
                    enabled: parent.canDownload
                    disabledReason: LocalModels.offline ? qsTr("Offline mode is on")
                                    : root.d.licenseNeedsAcceptance === true && !root.accepted ? qsTr("Accept the licence first")
                                    : parent.busy ? qsTr("Downloading") : ""
                    onClicked: root.startDownload(true)
                }
                PillButton {
                    objectName: "localDetailDownload"
                    visible: !root.installed
                    text: qsTr("Download only")
                    enabled: parent.canDownload
                    onClicked: root.startDownload(false)
                }
                PillButton {
                    objectName: "localDetailUse"
                    visible: root.installed
                    variant: "primary"
                    text: qsTr("Use for assistant")
                    onClicked: {
                        root.message = LocalModels.useForAssistant(root.d.kind === "hub" ? root.variant.id : root.d.id)
                        if (root.message === "")
                            root.close()
                    }
                }
                PillButton {
                    objectName: "localDetailHub"
                    visible: (root.d.hubUrl ?? "") !== ""
                    variant: "ghost"
                    iconName: "external-link"
                    text: qsTr("Hugging Face page")
                    onClicked: Qt.openUrlExternally(root.d.hubUrl)
                }
            }
        }
    }
}
