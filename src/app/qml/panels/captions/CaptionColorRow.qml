pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// One colour of the caption style (M5m): its label, a palette of swatches (the brand kit's named colours first, then
// white, ink, the caption yellow and the clip colours) and the hex value, typed. `picked(color)` asks for "#RRGGBB";
// `pickedBrand(name)` links the colour to the kit's colour of that name (C.13: a new kit re-themes it). The panel
// writes both (Captions.setStyle). While linked, the row says so and `value` is the kit's colour.
ColumnLayout {
    id: root

    property string label: ""
    property string value: Theme.contentWhite
    property string key: ""
    /// The brand kit colour this one follows ("" = its own).
    property string brandRef: ""

    signal picked(string color)
    signal pickedBrand(string name)

    readonly property var swatches: [Theme.contentWhite, Theme.contentInk, String(Theme.videoBlack), Theme.captionHighlight]
        .concat(Theme.clipColors)
    readonly property var brandSwatches: Brand.colors

    spacing: Theme.space(1)

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        Text {
            Layout.fillWidth: true
            text: root.label
            elide: Text.ElideRight
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        // Linked to the brand kit: its colour's name.
        Rectangle {
            objectName: "captionBrandLink_" + root.key
            visible: root.brandRef !== ""
            implicitWidth: brandText.implicitWidth + Theme.space(2)
            implicitHeight: brandText.implicitHeight + Theme.space(0.5)
            radius: height / 2
            color: Theme.accentWash
            border.width: Theme.hairline
            border.color: Theme.accentStroke
            Accessible.role: Accessible.StaticText
            Accessible.name: brandText.text
            Text {
                id: brandText
                anchors.centerIn: parent
                //: The caption colour follows the brand kit's colour of this name
                text: qsTr("Brand: %1").arg(root.brandRef)
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textXs
            }
        }
        Rectangle {
            implicitWidth: Theme.swatchSize
            implicitHeight: Theme.swatchSize
            radius: Theme.radiusBadge
            color: root.value
            border.width: Theme.hairline
            border.color: Theme.borderStrong
        }
        FieldInput {
            id: hexField
            objectName: "captionColor_" + root.key
            Layout.preferredWidth: Theme.space(22)
            text: root.value
            font.family: Theme.fontMono
            Accessible.name: qsTr("%1, hex colour").arg(root.label)
            onEditingFinished: {
                const t = text.trim()
                const hex = t.startsWith("#") ? t : "#" + t
                // A typed colour is the style's own: it also unlinks a brand colour.
                if (/^#[0-9a-fA-F]{6}$/.test(hex) && (hex.toUpperCase() !== root.value.toUpperCase() || root.brandRef !== ""))
                    root.picked(hex.toUpperCase())
                else
                    text = root.value
            }
        }
    }
    Flow {
        Layout.fillWidth: true
        spacing: Theme.space(1)
        // The brand kit's colours, by name.
        Repeater {
            model: root.brandSwatches
            delegate: Swatch {
                required property var modelData
                objectName: "captionBrandColor_" + root.key + "_" + modelData.name
                swatchColor: modelData.color
                checkable: false
                checked: root.brandRef === modelData.name
                //: A caption colour swatch from the brand kit, e.g. "Fill: brand colour Primary (#6A40FA)"
                tooltip: qsTr("%1: brand colour %2 (%3)").arg(root.label).arg(modelData.name).arg(modelData.hex)
                Accessible.name: tooltip
                onClicked: root.pickedBrand(modelData.name)
            }
        }
        Rectangle {
            visible: root.brandSwatches.length > 0
            width: Theme.hairline
            height: Theme.swatchSize
            color: Theme.border
        }
        Repeater {
            model: root.swatches
            delegate: Swatch {
                required property var modelData
                readonly property string hex: String(modelData).toUpperCase()
                swatchColor: modelData
                checkable: false
                checked: root.brandRef === "" && hex === root.value.toUpperCase()
                tooltip: qsTr("%1 %2").arg(root.label).arg(hex)
                onClicked: root.picked(hex)
            }
        }
    }
}
