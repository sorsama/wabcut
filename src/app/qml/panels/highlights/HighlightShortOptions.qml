pragma ComponentBehavior: Bound
import QtQuick

// Highlights > Short options (M5o, gaming): how highlights.toShort (on layout.apply) makes a 1080x1920 Short of the
// chosen moment: the layout (auto: a split when there is a facecam, else the blurred fill; the split's gameplay share
// and which side the facecam takes; the fill's zoom), the facecam (auto: the clip above the gameplay, or the region
// drawn in the layout picker when the webcam is inside the capture), captions and the hook title.
Popover {
    id: root
    objectName: "highlightShortOptions"

    title: qsTr("Short options")
    transformOrigin: Item.BottomRight

    readonly property var layouts: ["auto", "verticalSplit", "verticalBlurFill"]
    readonly property string layout: layouts[layoutTabs.currentIndex]
    /// The layout picker has a facecam region (Layouts.facecamRegion: fractions of the gameplay frame).
    readonly property bool regionAvailable: Object.keys(Layouts.facecamRegion).length > 0

    /// highlights.toShort's options, for Highlights.makeShort().
    function options() {
        const o = { layout: root.layout, captions: captionsToggle.checked, hookTitle: hookToggle.checked }
        if (root.layout !== "verticalBlurFill") {
            o.split = splitField.value / 100
            o.facecamOnTop = topToggle.checked
        }
        if (root.layout !== "verticalSplit")
            o.zoom = zoomField.value
        if (regionToggle.checked && root.regionAvailable)
            o.facecamRegion = Layouts.facecamRegion
        return o
    }

    component GroupLabel: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.Medium
    }
    component Hint: Text {
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textXs
        wrapMode: Text.Wrap
    }

    Column {
        width: Theme.space(72)
        spacing: Theme.space(2)

        GroupLabel { text: qsTr("Layout") }
        PillTabs {
            id: layoutTabs
            objectName: "highlightsShortLayout"
            width: parent.width
            compact: true
            model: [{ label: qsTr("Auto") }, { label: qsTr("Split") }, { label: qsTr("Blurred fill") }]
            currentIndex: 0
            Accessible.name: qsTr("Layout")
        }
        Hint {
            objectName: "highlightsShortLayoutHint"
            width: parent.width
            text: root.layout === "verticalSplit" ? qsTr("Gameplay and facecam stacked, one above the other.")
                  : root.layout === "verticalBlurFill" ? qsTr("The whole game view over a blurred copy of itself.")
                  : qsTr("A split when there is a facecam, else the blurred fill.")
        }

        // The split: the gameplay's share of the height, the facecam's side.
        Row {
            objectName: "highlightsShortSplitOptions"
            visible: root.layout !== "verticalBlurFill"
            width: parent.width
            spacing: Theme.space(2)
            GroupLabel {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Split")
            }
            NumberField {
                id: splitField
                objectName: "highlightsShortSplit"
                anchors.verticalCenter: parent.verticalCenter
                from: 20
                to: 80
                stepSize: 5
                value: 60
                suffix: qsTr("%")
                Accessible.name: qsTr("Split: the gameplay's share of the height")
            }
            Toggle {
                id: topToggle
                objectName: "highlightsShortFacecamOnTop"
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Facecam on top")
            }
        }
        // The blurred fill: the gameplay's scale over fit-to-width.
        Row {
            objectName: "highlightsShortZoomOptions"
            visible: root.layout !== "verticalSplit"
            width: parent.width
            spacing: Theme.space(2)
            GroupLabel {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Zoom")
            }
            NumberField {
                id: zoomField
                objectName: "highlightsShortZoom"
                anchors.verticalCenter: parent.verticalCenter
                from: 1
                to: 4
                stepSize: 0.1
                decimals: 1
                value: 1
                suffix: qsTr("×")
                Accessible.name: qsTr("Zoom: the gameplay's scale in the blurred fill")
            }
        }

        GroupLabel { text: qsTr("Facecam") }
        Hint {
            objectName: "highlightsShortFacecamHint"
            width: parent.width
            text: regionToggle.checked && root.regionAvailable
                  ? qsTr("The region drawn in the layout picker: the webcam inside the capture.")
                  : qsTr("Auto: the clip above the gameplay, when there is one.")
        }
        Toggle {
            id: regionToggle
            objectName: "highlightsShortUseRegion"
            visible: root.regionAvailable
            text: qsTr("Use the facecam region from the layout picker")
        }

        Toggle {
            id: captionsToggle
            objectName: "highlightsShortCaptions"
            text: qsTr("Captions")
            checked: true
        }
        Toggle {
            id: hookToggle
            objectName: "highlightsShortHook"
            text: qsTr("Hook title")
            checked: true
            tooltip: qsTr("The moment's title over the first seconds")
        }
    }
}
