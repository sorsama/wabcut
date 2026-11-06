pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// One stock result (0.7): its picture, who made it and where it is from, the sizes it can be added in, and the Add button. All text
// that came from the service is shown as plain text.
Rectangle {
    id: card

    /// The result's row (StockBridge.results).
    required property var item
    property int fileRow: item.preferredFile
    readonly property var chosenFile: item.files.length > 0 ? item.files[Math.min(fileRow, item.files.length - 1)] : null

    objectName: "stockCard_" + item.key
    implicitHeight: column.implicitHeight + Theme.space(4)
    radius: Theme.radiusField
    color: Theme.raised
    border.width: Theme.hairline
    border.color: Theme.border
    Accessible.role: Accessible.ListItem
    Accessible.name: item.credit

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: Theme.space(2)
        spacing: Theme.space(2)

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.round(width * 9 / 16)
            radius: Theme.radiusField
            color: Theme.sunken
            clip: true
            Image {
                objectName: "stockThumb"
                anchors.fill: parent
                source: card.item.thumb + "?" + Stock.thumbSerial
                asynchronous: true
                cache: false
                fillMode: Image.PreserveAspectCrop
                Accessible.ignored: true
            }
            Rectangle {
                visible: card.item.durationText !== ""
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Theme.space(1.5)
                radius: height / 2
                color: Theme.scrimModal
                implicitWidth: durationLabel.implicitWidth + Theme.space(3)
                implicitHeight: durationLabel.implicitHeight + Theme.space(1)
                Text {
                    id: durationLabel
                    anchors.centerIn: parent
                    text: card.item.durationText
                    color: Theme.scrimText
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                }
            }
        }

        Text {
            objectName: "stockCredit"
            Layout.fillWidth: true
            text: card.item.credit
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.textPrimary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Font.DemiBold
        }
        Text {
            Layout.fillWidth: true
            visible: card.item.title !== ""
            text: card.item.title
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        PillTabs {
            objectName: "stockSizes"
            Layout.fillWidth: true
            visible: card.item.files.length > 1
            compact: true
            Accessible.name: qsTr("Size to add")
            model: card.item.files.map(f => f.shortLabel)
            currentIndex: card.fileRow
            onActivated: index => card.fileRow = index
        }
        Text {
            Layout.fillWidth: true
            text: card.chosenFile ? card.chosenFile.label : ""
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space(2)
            PillButton {
                objectName: "stockAdd"
                Layout.fillWidth: true
                variant: "primary"
                compact: true
                text: qsTr("Add to project")
                iconName: "download"
                enabled: card.chosenFile !== null && Stock.canAdd && !Stock.busy && Stock.blockedReason === ""
                disabledReason: Stock.blockedReason !== "" ? Stock.blockedReason : !Stock.canAdd ? qsTr("Open or start a project to add media to.") : ""
                Accessible.name: qsTr("Add %1 to the project").arg(card.item.credit)
                onClicked: Stock.add(card.item.key, card.chosenFile.index)
            }
            IconButton {
                objectName: "stockOpenPage"
                iconName: "external-link"
                compact: true
                enabled: card.item.pageUrl !== ""
                tooltip: qsTr("Open its page on %1").arg(card.item.providerName)
                Accessible.name: qsTr("Open the page of %1 on %2").arg(card.item.credit).arg(card.item.providerName)
                onClicked: Stock.openPage(card.item.key)
            }
        }
    }
}
