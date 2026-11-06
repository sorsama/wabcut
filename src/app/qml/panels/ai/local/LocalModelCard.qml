pragma ComponentBehavior: Bound
import QtQuick

// One model of the local model library (Settings > AI > Local models): its name and publisher, tags (quantization,
// size, parameters, licence, Installed / In use / Recommended), what it is good for, its benchmark score, how it would
// run on this computer (GPU, CPU, or not at all, with why) and, while it downloads, its progress. Everything wraps, so
// nothing runs off a narrow window. `row` is a LocalModels row (recommended or results).
Rectangle {
    id: card

    required property var row
    /// Text of the main action ("" = none) and whether it is the accent button.
    property string primaryText: ""
    property bool primaryEnabled: true
    property string secondaryText: qsTr("Details")
    signal primary()
    signal secondary()

    readonly property var download: LocalModels.downloads.find(d => d.id === card.row.id
                                                              || (card.row.kind === "hub" && d.id.startsWith("hf:" + card.row.repo + ":"))) ?? null
    readonly property color fitColor: card.row.fit === "gpu" ? Theme.success
                                    : card.row.fit === "cpu" ? Theme.warning
                                    : card.row.fit === "no" ? Theme.danger
                                    : Theme.textTertiary
    readonly property string fitLabel: card.row.fit === "gpu" ? qsTr("Runs on the GPU")
                                     : card.row.fit === "cpu" ? qsTr("Runs on the CPU (slower)")
                                     : card.row.fit === "no" ? qsTr("Too big for this computer now")
                                     : qsTr("Fit unknown")

    objectName: "localModelCard_" + card.row.id
    implicitHeight: body.implicitHeight + Theme.space(8)
    radius: Theme.radiusCard
    color: Theme.section
    border.width: card.row.recommended === true ? 2 : Theme.hairline
    border.color: card.row.recommended === true ? Theme.accentStroke : Theme.border
    Accessible.role: Accessible.Grouping
    Accessible.name: card.row.name

    Column {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space(4)
        spacing: Theme.space(2)

        // Name and score.
        Item {
            width: parent.width
            height: Math.max(titleColumn.implicitHeight, scorePill.height)

            Column {
                id: titleColumn
                anchors.left: parent.left
                anchors.right: scorePill.left
                anchors.rightMargin: Theme.space(3)
                spacing: Theme.space(0.5)

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: card.row.name
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textLg
                    font.weight: Font.Bold
                    Accessible.role: Accessible.Heading
                    Accessible.name: text
                }
                Text {
                    width: parent.width
                    elide: Text.ElideMiddle
                    text: card.row.kind === "hub" ? card.row.repo : qsTr("%1 · tested with Wabcut").arg(card.row.repo)
                    color: Theme.textTertiary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textXs
                }
            }

            Rectangle {
                id: scorePill
                objectName: "localModelScore"
                anchors.right: parent.right
                anchors.top: parent.top
                width: scoreRow.implicitWidth + Theme.space(4)
                height: Theme.controlHeightSmall
                radius: height / 2
                color: card.row.scoreText !== "" ? Theme.accentFill : Theme.raised
                border.width: card.row.scoreText !== "" ? 0 : Theme.hairline
                border.color: Theme.border
                Accessible.role: Accessible.StaticText
                Accessible.name: card.row.scoreText !== "" ? qsTr("Benchmark score %1").arg(card.row.scoreText) : qsTr("No published score")

                Row {
                    id: scoreRow
                    anchors.centerIn: parent
                    spacing: Theme.space(1)
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: card.row.scoreText !== "" ? qsTr("Score") : qsTr("No published score")
                        color: card.row.scoreText !== "" ? Theme.accentOnFill : Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                        font.weight: Font.DemiBold
                    }
                    Text {
                        visible: card.row.scoreText !== ""
                        anchors.verticalCenter: parent.verticalCenter
                        text: card.row.scoreText
                        color: Theme.accentOnFill
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.textSm
                        font.weight: Font.Bold
                    }
                }
            }
        }

        // Tags.
        Flow {
            width: parent.width
            spacing: Theme.space(1.5)

            LocalTag {
                visible: card.row.recommended === true
                text: qsTr("Best for this computer")
                tone: "accent"
            }
            LocalTag {
                visible: card.row.inUse === true
                text: qsTr("Assistant's model")
                tone: "success"
            }
            LocalTag {
                visible: card.row.installed === true
                text: qsTr("Downloaded")
                tone: "success"
            }
            LocalTag {
                visible: card.row.publisherKind === "official"
                text: qsTr("Official")
            }
            LocalTag {
                visible: card.row.publisherKind === "quantizer"
                text: qsTr("Well-known publisher")
            }
            LocalTag {
                visible: (card.row.quant ?? "") !== ""
                text: card.row.quant ?? ""
            }
            LocalTag {
                visible: (card.row.size ?? "") !== ""
                text: card.row.size ?? ""
            }
            LocalTag {
                visible: (card.row.params ?? "") !== ""
                text: (card.row.activeParams ?? "") !== "" ? qsTr("%1 parameters (%2 active)").arg(card.row.params).arg(card.row.activeParams)
                                                           : qsTr("%1 parameters").arg(card.row.params)
            }
            LocalTag {
                text: card.row.licenseName
                tone: card.row.permissive === true ? "neutral" : "warning"
            }
            LocalTag {
                visible: card.row.gated === true
                text: qsTr("Gated: needs a Hugging Face token")
                tone: "warning"
            }
            LocalTag {
                visible: card.row.kind === "curated" && card.row.toolCalls === false
                text: qsTr("Chat only: cannot edit")
                tone: "warning"
            }
        }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            text: card.row.kind === "hub" ? qsTr("%1 · %2 downloads · %3 likes · updated %4")
                                                .arg(card.row.taskNames).arg(card.row.downloadsText).arg(card.row.likesText).arg(card.row.updated)
                                          : (card.row.note ?? card.row.taskNames)
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        // How it runs here.
        Row {
            width: parent.width
            spacing: Theme.space(1.5)

            Rectangle {
                y: (fitText.font.pixelSize - height) / 2 + Theme.space(0.5)
                width: Theme.dotSize
                height: Theme.dotSize
                radius: width / 2
                color: card.fitColor
            }
            Text {
                id: fitText
                objectName: "localModelFit"
                width: parent.width - Theme.dotSize - Theme.space(1.5)
                wrapMode: Text.WordWrap
                text: (card.row.fitText ?? "") !== "" ? qsTr("%1: %2").arg(card.fitLabel).arg(card.row.fitText) : card.fitLabel
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }

        // Download progress.
        Column {
            visible: card.download !== null
            width: parent.width
            spacing: Theme.space(1)

            BeamProgress {
                width: parent.width
                value: card.download ? card.download.progress : 0
                active: card.download !== null && card.download.status === "running"
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: !card.download ? ""
                      : card.download.status === "failed" ? card.download.error
                      : card.download.status === "paused" ? qsTr("Paused · %1").arg(card.download.receivedText)
                      : card.download.status === "queued" ? qsTr("Waiting to download")
                      : [card.download.receivedText, card.download.speedText, card.download.etaText].filter(s => s !== "").join(" · ")
                color: card.download && card.download.status === "failed" ? Theme.dangerText : Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }

        Flow {
            width: parent.width
            spacing: Theme.space(2)

            PillButton {
                visible: card.primaryText !== ""
                objectName: "localModelPrimary"
                variant: "primary"
                text: card.primaryText
                enabled: card.primaryEnabled
                Accessible.name: card.primaryText + " " + card.row.name
                onClicked: card.primary()
            }
            PillButton {
                visible: card.secondaryText !== ""
                objectName: "localModelDetails"
                text: card.secondaryText
                Accessible.name: card.secondaryText + " " + card.row.name
                onClicked: card.secondary()
            }
        }
    }
}
