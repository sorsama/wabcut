pragma ComponentBehavior: Bound
import QtQuick

// Highlights > Short options (M5o, gaming): how highlights.toShort (on layout.apply) makes a 1080x1920 Short of the
// chosen moment: the layout (auto: a split when there is a facecam, else the blurred fill; the split's gameplay share
// and which side the facecam takes; the fill's zoom), the facecam (auto: the clip above the gameplay, or the region
// drawn in the layout picker when the webcam is inside the capture), captions and the hook title.
// 0.4: the caption style (a caption preset, "Shorts" by default: big outlined words above the platforms' buttons) and
// the length - Lead-in (the build-up before the moment), Tail (the reaction after) and At least (a minimum length, the
// missing time two thirds before) - all remembered (Preferences.highlightsShort*). Make Shorts (N) uses the same options.
// In its own file, importing only QtQuick, so the Wabcut ContextMenu (the caption style menu) is not shadowed.
Popover {
    id: root
    objectName: "highlightShortOptions"

    title: qsTr("Short options")
    transformOrigin: Item.BottomRight

    readonly property var layouts: ["auto", "verticalSplit", "verticalBlurFill"]
    readonly property string layout: layouts[layoutTabs.currentIndex]
    /// The layout picker has a facecam region (Layouts.facecamRegion: fractions of the gameplay frame).
    readonly property bool regionAvailable: Object.keys(Layouts.facecamRegion).length > 0
    /// "At least": 0 (Off) or seconds.
    readonly property var minLengths: [0, 15, 30, 45, 60]

    /// highlights.toShort's options, for Highlights.makeShort() / makeShorts() / shortParams().
    function options() {
        const o = { layout: root.layout, captions: captionsToggle.checked, hookTitle: hookToggle.checked,
                    handleBefore: Preferences.highlightsShortLeadIn, handleAfter: Preferences.highlightsShortTail,
                    minLength: Preferences.highlightsShortMinLength }
        if (captionsToggle.checked)
            o.captionPreset = Preferences.highlightsShortCaptionPreset
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

    /// The caption presets for the style menu: "Shorts" first, the other built-in ones, then the project's.
    function captionPresetOptions(): var {
        const names = Captions.presets.map(p => String(p.name))
        return names.filter(n => n === "Shorts").concat(names.filter(n => n !== "Shorts")).map(n => ({ id: n, name: n }))
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
        width: Theme.space(84) // 0.4: room for the Length row and the facecam region switch's label
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
                onValueModified: v => splitField.value = v
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
                onValueModified: v => zoomField.value = v
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

        // ---- Length (0.4) ----
        GroupLabel { text: qsTr("Length") }
        Row {
            objectName: "highlightsShortHandles"
            width: parent.width
            spacing: Theme.space(2)
            GroupLabel {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Lead-in")
            }
            NumberField {
                id: leadInField
                objectName: "highlightsShortLeadIn"
                anchors.verticalCenter: parent.verticalCenter
                from: 0
                to: 30
                stepSize: 0.5
                decimals: 1
                suffix: qsTr("s")
                value: Preferences.highlightsShortLeadIn
                Accessible.name: qsTr("Lead-in: seconds before the moment")
                onValueModified: v => Preferences.highlightsShortLeadIn = v
            }
            GroupLabel {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Tail")
            }
            NumberField {
                id: tailField
                objectName: "highlightsShortTail"
                anchors.verticalCenter: parent.verticalCenter
                from: 0
                to: 30
                stepSize: 0.5
                decimals: 1
                suffix: qsTr("s")
                value: Preferences.highlightsShortTail
                Accessible.name: qsTr("Tail: seconds after the moment")
                onValueModified: v => Preferences.highlightsShortTail = v
            }
        }
        GroupLabel { text: qsTr("At least") }
        PillTabs {
            id: minLengthTabs
            objectName: "highlightsShortMinLength"
            width: parent.width
            compact: true
            model: [{ label: qsTr("Off") }, { label: qsTr("15 s") }, { label: qsTr("30 s") }, { label: qsTr("45 s") },
                    { label: qsTr("60 s") }]
            currentIndex: Math.max(0, root.minLengths.indexOf(Preferences.highlightsShortMinLength))
            Accessible.name: qsTr("At least: the shortest Short")
            onActivated: index => Preferences.highlightsShortMinLength = root.minLengths[index]
        }
        Hint {
            objectName: "highlightsShortLengthHint"
            width: parent.width
            text: qsTr("Lead-in adds the build-up before the moment, tail the reaction after. At least pads short moments, two thirds before.")
        }

        Toggle {
            id: captionsToggle
            objectName: "highlightsShortCaptions"
            text: qsTr("Captions")
            checked: true
        }
        // 0.4: the caption style, while captions are on (a caption preset: built in or the project's).
        Row {
            objectName: "highlightsShortCaptionStyleRow"
            visible: captionsToggle.checked
            width: parent.width
            spacing: Theme.space(2)
            GroupLabel {
                id: styleLabel
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Caption style")
            }
            PillButton {
                id: styleButton
                objectName: "highlightsShortCaptionStyle"
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width - styleLabel.width - parent.spacing)
                compact: true
                iconName: "chevron-down"
                text: Preferences.highlightsShortCaptionPreset
                tooltip: qsTr("The look of the Short's captions: a caption preset")
                Accessible.name: qsTr("Caption style: %1").arg(Preferences.highlightsShortCaptionPreset)
                onClicked: {
                    styleMenu.options = root.captionPresetOptions()
                    styleMenu.popup(styleButton, 0, styleButton.height + Theme.space(1))
                }
                OptionsMenu {
                    id: styleMenu
                    objectName: "highlightsShortCaptionStyleMenu"
                    current: Preferences.highlightsShortCaptionPreset
                    onChosen: id => Preferences.highlightsShortCaptionPreset = id
                }
            }
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
