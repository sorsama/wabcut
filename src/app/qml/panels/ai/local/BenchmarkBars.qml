pragma ComponentBehavior: Bound
import QtQuick

// Published benchmark results of a local model (Settings > AI > Local models, a model's card): one bar per result,
// 0 to 100, with the test's name, what it measures, the mode it was run in and the number. `scores`:
// [{ metric, name, description, value, mode, note, source }]. The source line under the bars names where the numbers
// come from; without scores the card says "No published score".
Column {
    id: root

    property var scores: []
    property string source: ""
    property string checked: ""
    property string family: ""
    property string note: ""

    spacing: Theme.space(2)

    Text {
        visible: root.scores.length === 0
        width: parent.width
        wrapMode: Text.WordWrap
        text: qsTr("No published score. Fine-tuned and merged models do not inherit their base model's results.")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    Repeater {
        model: root.scores

        delegate: Column {
            id: bar
            required property var modelData
            required property int index
            objectName: "benchmarkBar_" + modelData.metric
            width: root.width
            spacing: Theme.space(1)
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("%1: %2").arg(modelData.name).arg(modelData.value.toFixed(1))

            Item {
                width: parent.width
                height: Math.max(barName.implicitHeight, barValue.implicitHeight)

                Text {
                    id: barName
                    anchors.left: parent.left
                    anchors.right: barValue.left
                    anchors.rightMargin: Theme.space(2)
                    elide: Text.ElideRight
                    text: bar.modelData.mode !== "" ? qsTr("%1 · %2").arg(bar.modelData.name).arg(bar.modelData.mode) : bar.modelData.name
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                    font.weight: Font.DemiBold
                }
                Text {
                    id: barValue
                    anchors.right: parent.right
                    text: bar.modelData.value.toFixed(1)
                    color: Theme.textPrimary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.textSm
                }
            }

            Rectangle {
                width: parent.width
                height: Theme.space(2)
                radius: height / 2
                color: Theme.well

                Rectangle {
                    width: Math.max(height, parent.width * Math.max(0, Math.min(100, bar.modelData.value)) / 100)
                    height: parent.height
                    radius: height / 2
                    color: Theme.accentFill
                }
            }

            Text {
                visible: text !== ""
                width: parent.width
                wrapMode: Text.WordWrap
                text: bar.modelData.description
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
    }

    Text {
        visible: root.source !== ""
        width: parent.width
        wrapMode: Text.WordWrap
        textFormat: Text.StyledText
        linkColor: Theme.accentStroke
        text: qsTr("Published by the model's makers%1, checked %2: <a href=\"%3\">source</a>")
                  .arg(root.family !== "" ? qsTr(" for %1").arg(root.family) : "")
                  .arg(root.checked !== "" ? root.checked : qsTr("when the list was made"))
                  .arg(root.source)
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        onLinkActivated: link => Qt.openUrlExternally(link)
        Accessible.role: Accessible.Link
        Accessible.name: qsTr("Benchmark source")

        HoverHandler {
            cursorShape: parent.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor
        }
    }

    Text {
        visible: root.note !== ""
        width: parent.width
        wrapMode: Text.WordWrap
        text: root.note
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
    }
}
