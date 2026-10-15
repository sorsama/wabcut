pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// A colour of a graphic (Essential Graphics): its label, the hex value (typed), the brand kit's named colours first
// (choosing one keeps the reference: a new kit re-themes it) and then the palette. `value` is Graphics' colour map
// { color: "#RRGGBB", alpha, brand }; `picked` asks for "#RRGGBB" or "brand:<name>".
ColumnLayout {
    id: root

    property string label: ""
    property var value: ({ color: Theme.contentWhite, alpha: 1, brand: "" })
    property string key: ""

    signal picked(string color)

    readonly property string hex: String(root.value.color ?? Theme.contentWhite).toUpperCase()
    readonly property string brandName: String(root.value.brand ?? "")
    readonly property var brandColors: Graphics.brand.colors ?? []
    readonly property var swatches: [Theme.contentWhite, Theme.contentInk, String(Theme.videoBlack), Theme.captionHighlight]
        .concat(Theme.clipColors)

    Layout.fillWidth: true
    spacing: Theme.space(1)

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        Text {
            Layout.fillWidth: true
            text: root.brandName !== "" ? qsTr("%1 · %2").arg(root.label).arg(root.brandName) : root.label
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        Rectangle {
            implicitWidth: Theme.swatchSize
            implicitHeight: Theme.swatchSize
            radius: Theme.radiusBadge
            color: root.hex
            opacity: Math.max(0.25, Number(root.value.alpha ?? 1))
            border.width: Theme.hairline
            border.color: Theme.borderStrong
        }
        FieldInput {
            objectName: "graphicColor_" + root.key
            Layout.preferredWidth: Theme.space(22)
            text: root.hex
            font.family: Theme.fontMono
            Accessible.name: qsTr("%1, hex colour").arg(root.label)
            onEditingFinished: {
                const t = text.trim()
                const hex = t.startsWith("#") ? t : "#" + t
                if (/^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$/.test(hex) && hex.toUpperCase() !== root.hex)
                    root.picked(hex.toUpperCase())
                else
                    text = root.hex
            }
        }
    }
    Flow {
        Layout.fillWidth: true
        spacing: Theme.space(1)
        // The brand kit's colours, by name.
        Repeater {
            model: root.brandColors.length
            delegate: Swatch {
                required property int index
                readonly property var brandColor: root.brandColors[index] ?? ({ name: "", color: Theme.contentWhite })
                objectName: "graphicBrandSwatch_" + root.key + "_" + brandColor.name
                swatchColor: brandColor.color
                checkable: false
                checked: root.brandName === brandColor.name
                tooltip: qsTr("Brand colour %1").arg(brandColor.name)
                Accessible.name: tooltip
                onClicked: root.picked("brand:" + brandColor.name)
            }
        }
        Repeater {
            model: root.swatches
            delegate: Swatch {
                required property var modelData
                readonly property string swatchHex: String(modelData).toUpperCase()
                swatchColor: modelData
                checkable: false
                checked: root.brandName === "" && swatchHex === root.hex
                tooltip: qsTr("%1 %2").arg(root.label).arg(swatchHex)
                Accessible.name: tooltip
                onClicked: root.picked(swatchHex)
            }
        }
    }
}
