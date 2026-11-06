pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// ● STOCK MEDIA (panel key "stock", 0.7): free videos and photos from Pexels and Pixabay, searched with the user's own key.
//   Each result says who made it and where it is from; Add to project downloads the size chosen, imports it into the "Stock" bin
//   (one undo step) and records the licence, the page and the credit line under Rights and Clearance. Offline mode and the
//   project's no-cloud lock switch searching and downloading off, with the reason. The service's name and a link to its licence are
//   always shown (both services ask for that in return for the free use of their API).
// Bridge: Stock (StockBridge).
Panel {
    id: root
    objectName: "stockPanel"

    title: qsTr("Stock Media")

    property bool keyOpen: !Stock.hasKey

    function focusContent() {
        queryField.forceActiveFocus(Qt.TabFocusReason)
    }

    readonly property var providerRow: Stock.providers.find(p => p.id === Stock.provider) ?? ({})

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.space(3)

        Flow {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            PillTabs {
                objectName: "stockProviderTabs"
                compact: true
                Accessible.name: qsTr("Service")
                model: ["Pexels", "Pixabay"]
                currentIndex: Stock.provider === "pixabay" ? 1 : 0
                onActivated: index => Stock.provider = index === 1 ? "pixabay" : "pexels"
            }
            PillTabs {
                objectName: "stockKindTabs"
                compact: true
                Accessible.name: qsTr("Kind")
                model: [qsTr("Videos"), qsTr("Photos")]
                currentIndex: Stock.kind === "photo" ? 1 : 0
                onActivated: index => Stock.kind = index === 1 ? "photo" : "video"
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            FieldInput {
                id: queryField
                objectName: "stockQuery"
                Layout.fillWidth: true
                iconName: "search"
                placeholderText: Stock.kind === "photo" ? qsTr("Search free photos") : qsTr("Search free videos")
                clearOnEscape: true
                maximumLength: 100
                Accessible.name: qsTr("Search stock media")
                onAccepted: Stock.search(text)
            }
            PillButton {
                objectName: "stockSearch"
                variant: "primary"
                compact: true
                text: qsTr("Search")
                enabled: !Stock.busy && queryField.text.trim() !== "" && Stock.blockedReason === ""
                disabledReason: Stock.blockedReason
                onClicked: Stock.search(queryField.text)
            }
        }

        // Where the results come from (the services ask for a visible mention and a link in return for the free API).
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.space(1)
            Text {
                objectName: "stockSource"
                Layout.fillWidth: true
                text: qsTr("Results from %1. %2").arg(root.providerRow.name ?? "").arg(root.providerRow.licence ?? "")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.textSecondary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(2)
                PillButton {
                    objectName: "stockLicence"
                    variant: "ghost"
                    compact: true
                    text: qsTr("Licence")
                    iconName: "external-link"
                    Accessible.name: qsTr("Open the %1 licence").arg(root.providerRow.licence ?? "")
                    onClicked: Stock.openLicence(Stock.provider)
                }
                PillButton {
                    objectName: "stockKeyToggle"
                    variant: "ghost"
                    compact: true
                    text: Stock.hasKey ? qsTr("Key") : qsTr("Add key")
                    iconName: "settings"
                    onClicked: root.keyOpen = !root.keyOpen
                }
            }
        }

        // The user's key for the chosen service.
        Rectangle {
            objectName: "stockKeyBox"
            Layout.fillWidth: true
            visible: root.keyOpen
            implicitHeight: keyColumn.implicitHeight + Theme.space(4)
            radius: Theme.radiusField
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: Theme.border
            ColumnLayout {
                id: keyColumn
                anchors.fill: parent
                anchors.margins: Theme.space(2)
                spacing: Theme.space(2)
                Text {
                    Layout.fillWidth: true
                    text: Stock.hasKey ? qsTr("Your %1 key is saved on this computer. Paste a new one to replace it.").arg(root.providerRow.name ?? "")
                                       : qsTr("%1 needs your own key. It is free: make an account there, copy the key, paste it here. It is kept in your system's key store, never in a project.").arg(root.providerRow.name ?? "")
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.textSecondary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    FieldInput {
                        id: keyField
                        objectName: "stockKeyField"
                        Layout.fillWidth: true
                        echoMode: TextInput.Password
                        placeholderText: qsTr("%1 key").arg(root.providerRow.name ?? "")
                        Accessible.name: qsTr("Stock service key")
                        Accessible.description: qsTr("%1 key").arg(root.providerRow.name ?? "")
                        onAccepted: saveButton.clicked()
                    }
                    PillButton {
                        id: saveButton
                        objectName: "stockKeySave"
                        variant: "primary"
                        compact: true
                        text: qsTr("Save")
                        enabled: keyField.text.trim() !== "" && !Stock.busy
                        onClicked: {
                            Stock.saveKey(Stock.provider, keyField.text)
                            keyField.text = ""
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    PillButton {
                        objectName: "stockKeyGet"
                        variant: "ghost"
                        compact: true
                        text: qsTr("Get a free key")
                        iconName: "external-link"
                        onClicked: Stock.openKeyPage(Stock.provider)
                    }
                    PillButton {
                        objectName: "stockKeyRemove"
                        variant: "ghost"
                        compact: true
                        text: qsTr("Remove the key")
                        visible: Stock.hasKey
                        onClicked: Stock.removeKey(Stock.provider)
                    }
                }
            }
        }

        Rectangle {
            objectName: "stockBlocked"
            Layout.fillWidth: true
            visible: Stock.blockedReason !== ""
            implicitHeight: blockedText.implicitHeight + Theme.space(4)
            radius: Theme.radiusField
            color: Theme.sunken
            border.width: Theme.hairline
            border.color: Theme.warning
            Text {
                id: blockedText
                anchors.fill: parent
                anchors.margins: Theme.space(2)
                text: Stock.blockedReason
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
        }

        Text {
            objectName: "stockMessage"
            Layout.fillWidth: true
            visible: Stock.message !== ""
            text: Stock.message
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Stock.messageIsError ? Theme.warning : Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            Accessible.role: Accessible.StaticText
        }

        // A download in progress.
        RowLayout {
            objectName: "stockDownload"
            Layout.fillWidth: true
            visible: Stock.downloading
            spacing: Theme.space(2)
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Theme.space(1.5)
                radius: height / 2
                color: Theme.well
                Rectangle {
                    width: parent.width * Math.max(0, Math.min(1, Stock.progress))
                    height: parent.height
                    radius: height / 2
                    color: Theme.accentFill
                }
            }
            PillButton {
                objectName: "stockCancel"
                variant: "ghost"
                compact: true
                text: qsTr("Cancel")
                onClicked: Stock.cancel()
            }
        }

        Flickable {
            id: scroller
            objectName: "stockResults"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: grid.implicitHeight + moreRow.implicitHeight + Theme.space(2)
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ThinScrollBar {}
            visible: Stock.results.length > 0

            Flow {
                id: grid
                width: scroller.width - Theme.space(3)
                spacing: Theme.space(3)
                readonly property int columns: Math.max(1, Math.floor((width + spacing) / (Theme.space(44) + spacing)))
                Repeater {
                    model: Stock.results
                    delegate: StockCard {
                        required property var modelData
                        item: modelData
                        width: Math.floor((grid.width - (grid.columns - 1) * grid.spacing) / grid.columns)
                    }
                }
            }
            RowLayout {
                id: moreRow
                anchors.top: grid.bottom
                anchors.topMargin: Theme.space(2)
                width: scroller.width - Theme.space(3)
                visible: Stock.hasMore
                PillButton {
                    objectName: "stockMore"
                    Layout.alignment: Qt.AlignHCenter
                    compact: true
                    text: qsTr("Show more")
                    enabled: !Stock.busy
                    onClicked: Stock.loadMore()
                }
            }
        }

        EmptyState {
            objectName: "stockEmpty"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            visible: Stock.results.length === 0 && Stock.message === "" && !Stock.busy
            illustration: "search"
            text: qsTr("Search free videos and photos. Add one to your project and its licence and credit line are kept for the credits.")
        }
        Item {
            Layout.fillHeight: true
            visible: Stock.results.length === 0
        }
    }
}
