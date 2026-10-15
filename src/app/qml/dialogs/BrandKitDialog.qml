pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// ● BRAND KIT (C.13, M5o; bridge `Brand`; docs/graphics.md "Brand kit"): the project's named colours, font roles and
// logos. Graphics, templates and caption styles refer to them by name, so a change here re-themes all of them at once.
// Every edit is one undo step (a typed name or hex when it is committed, a choice at once). A sample lower third shows
// the kit; kits travel as files (Import kit… / Export kit…; logos are found again by their media's name).
WabcutDialog {
    id: dialog
    objectName: "brandKitDialog"

    readonly property var kit: Brand.kit
    readonly property bool hasKit: Brand.hasKit
    readonly property var colors: kit.colors ?? []
    readonly property var fonts: kit.fonts ?? []
    readonly property var logos: kit.logos ?? []
    readonly property var weights: [
        { id: "300", name: qsTr("Light") }, { id: "400", name: qsTr("Regular") }, { id: "500", name: qsTr("Medium") },
        { id: "600", name: qsTr("Semibold") }, { id: "700", name: qsTr("Bold") }, { id: "900", name: qsTr("Black") }
    ]

    function weightName(w) {
        const found = weights.find(x => Number(x.id) === w)
        return found ? found.name : String(w)
    }
    /// The kit's colour `name` (else its first, else `fallback`) as a colour.
    function colorOf(name, fallback) {
        const c = colors.find(x => x.name === name)
        return c ? c.color : colors.length > 0 ? colors[0].color : fallback
    }
    function fontOf(role) {
        const f = fonts.find(x => x.role === role)
        return f ? f.family : Theme.fontSans
    }
    function weightOf(role, fallback) {
        const f = fonts.find(x => x.role === role)
        return f ? f.weight : fallback
    }
    /// "#RRGGBB" from a typed value, or "".
    function hexOf(text) {
        const t = text.trim()
        const hex = t.startsWith("#") ? t : "#" + t
        return /^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/.test(hex) ? hex.toUpperCase() : ""
    }

    title: qsTr("Brand kit")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(170), Theme.space(170))

    component FieldLabel: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    component ChoicePill: PillButton {
        id: pill
        property var options: []
        property string current: ""
        signal chosen(string id)
        compact: true
        iconName: "chevron-down"
        onClicked: menu.popup(pill, 0, pill.height + Theme.space(1))
        OptionsMenu {
            id: menu
            options: pill.options
            current: pill.current
            onChosen: id => pill.chosen(id)
        }
    }

    Note {
        text: qsTr("Name the colours, fonts and logos of a brand once. Graphics, templates and captions that use them follow the kit, so a new kit re-themes them all.")
    }

    // ---- No kit yet ----
    RowLayout {
        Layout.fillWidth: true
        visible: !dialog.hasKit
        spacing: Theme.space(2)
        FieldInput {
            id: newKitName
            objectName: "brandNewKitName"
            Layout.fillWidth: true
            placeholderText: qsTr("Brand name")
            Accessible.name: qsTr("Brand kit name")
            onAccepted: createButton.clicked()
        }
        PillButton {
            id: createButton
            objectName: "brandCreateKit"
            variant: "primary"
            iconName: "plus"
            text: qsTr("Create brand kit")
            onClicked: Brand.createKit(newKitName.text)
        }
    }

    // ---- The kit ----
    RowLayout {
        Layout.fillWidth: true
        visible: dialog.hasKit
        spacing: Theme.space(2)
        FieldLabel { text: qsTr("Name") }
        FieldInput {
            objectName: "brandKitName"
            Layout.fillWidth: true
            text: dialog.kit.name ?? ""
            Accessible.name: qsTr("Brand kit name")
            onEditingFinished: {
                if (text.trim() !== (dialog.kit.name ?? ""))
                    Brand.setName(text)
            }
        }
    }

    // A sample lower third in the kit's colours and fonts: Primary for the box, Accent for the name.
    Rectangle {
        objectName: "brandKitPreview"
        Layout.fillWidth: true
        visible: dialog.hasKit
        implicitHeight: Theme.space(30)
        radius: Theme.radiusField
        color: Theme.videoBlack
        border.width: Theme.hairline
        border.color: Theme.border
        Accessible.role: Accessible.Graphic
        Accessible.name: qsTr("A sample lower third in the brand kit's colours and fonts")

        Rectangle {
            id: bar
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.leftMargin: Theme.space(6)
            anchors.bottomMargin: Theme.space(5)
            width: sample.implicitWidth + Theme.space(8)
            height: sample.implicitHeight + Theme.space(4)
            radius: Theme.radiusField
            color: dialog.colorOf("Primary", Theme.accentFill)

            Column {
                id: sample
                anchors.centerIn: parent
                spacing: Theme.space(0.5)
                Text {
                    objectName: "brandPreviewName"
                    //: Sample text in the brand kit's preview (a person's name in a lower third)
                    text: qsTr("Somchai Jaidee")
                    color: dialog.colorOf("Accent", Theme.scrimText)
                    font.family: dialog.fontOf("heading")
                    font.weight: dialog.weightOf("heading", 700)
                    font.pixelSize: Theme.textXl
                }
                Text {
                    //: Sample text in the brand kit's preview (a job title in a lower third)
                    text: qsTr("Producer, Bangkok")
                    color: Theme.scrimText
                    font.family: dialog.fontOf("body")
                    font.weight: dialog.weightOf("body", 400)
                    font.pixelSize: Theme.textMd
                }
            }
        }
    }

    ScrollView {
        id: scroller
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(sections.implicitHeight, Theme.space(110))
        visible: dialog.hasKit
        contentWidth: availableWidth
        clip: true
        ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: sections
            width: scroller.availableWidth
            spacing: Theme.space(2)

            // Colours. The rows are counted, not listed, so an edit does not make the fields again (the focus stays).
            SectionHeader {
                Layout.fillWidth: true
                text: qsTr("Colours")
            }
            Repeater {
                model: dialog.colors.length
                delegate: RowLayout {
                    id: colorRow
                    required property int index
                    readonly property var entry: dialog.colors[index] ?? ({})
                    objectName: "brandColor_" + index
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    Rectangle {
                        implicitWidth: Theme.swatchSize + Theme.space(2)
                        implicitHeight: implicitWidth
                        radius: Theme.radiusBadge
                        color: colorRow.entry.color ?? Theme.contentWhite
                        border.width: Theme.hairline
                        border.color: Theme.borderStrong
                    }
                    FieldInput {
                        objectName: "brandColorName_" + colorRow.index
                        Layout.fillWidth: true
                        text: colorRow.entry.name ?? ""
                        Accessible.name: qsTr("Colour %1 name").arg(colorRow.index + 1)
                        onEditingFinished: {
                            if (text.trim() !== "" && text.trim() !== colorRow.entry.name)
                                Brand.setColor(colorRow.index, text, "")
                            else
                                text = colorRow.entry.name ?? ""
                        }
                    }
                    FieldInput {
                        objectName: "brandColorHex_" + colorRow.index
                        Layout.preferredWidth: Theme.space(26)
                        text: colorRow.entry.hex ?? ""
                        font.family: Theme.fontMono
                        Accessible.name: qsTr("%1, hex colour").arg(colorRow.entry.name ?? "")
                        onEditingFinished: {
                            const hex = dialog.hexOf(text)
                            if (hex !== "" && hex !== colorRow.entry.hex)
                                Brand.setColor(colorRow.index, "", hex)
                            else
                                text = colorRow.entry.hex ?? ""
                        }
                    }
                    IconButton {
                        objectName: "brandColorRemove_" + colorRow.index
                        compact: true
                        iconName: "trash-2"
                        tooltip: qsTr("Remove %1 from the kit").arg(colorRow.entry.name ?? "")
                        onClicked: Brand.removeColor(colorRow.index)
                    }
                }
            }
            PillButton {
                objectName: "brandAddColor"
                compact: true
                variant: "ghost"
                iconName: "plus"
                text: qsTr("Add colour")
                enabled: dialog.colors.length < 64
                onClicked: Brand.addColor(qsTr("Colour"), String(Theme.contentWhite))
            }

            // Fonts by role.
            SectionHeader {
                Layout.fillWidth: true
                text: qsTr("Fonts")
            }
            Repeater {
                model: dialog.fonts.length
                delegate: RowLayout {
                    id: fontRow
                    required property int index
                    readonly property var entry: dialog.fonts[index] ?? ({})
                    objectName: "brandFont_" + index
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    FieldInput {
                        objectName: "brandFontRole_" + fontRow.index
                        Layout.preferredWidth: Theme.space(26)
                        text: fontRow.entry.role ?? ""
                        Accessible.name: qsTr("Font role %1").arg(fontRow.index + 1)
                        onEditingFinished: {
                            if (text.trim() !== "" && text.trim() !== fontRow.entry.role)
                                Brand.setFont(fontRow.index, text, "", fontRow.entry.style ?? "", 0)
                            else
                                text = fontRow.entry.role ?? ""
                        }
                    }
                    FieldInput {
                        id: familyField
                        objectName: "brandFontFamily_" + fontRow.index
                        Layout.fillWidth: true
                        text: fontRow.entry.family ?? ""
                        Accessible.name: qsTr("%1 font family").arg(fontRow.entry.role ?? "")
                        onEditingFinished: {
                            if (text.trim() !== "" && text.trim() !== fontRow.entry.family)
                                Brand.setFont(fontRow.index, "", text, fontRow.entry.style ?? "", 0)
                            else
                                text = fontRow.entry.family ?? ""
                        }
                    }
                    // The families of this computer that start with what is typed.
                    IconButton {
                        id: familyButton
                        objectName: "brandFontFamilies_" + fontRow.index
                        compact: true
                        iconName: "chevron-down"
                        tooltip: qsTr("Choose an installed font")
                        onClicked: familyMenu.popup(familyButton, 0, familyButton.height + Theme.space(1))
                        OptionsMenu {
                            id: familyMenu
                            options: {
                                const typed = familyField.text.trim().toLowerCase()
                                const all = Brand.fontFamilies
                                const matches = all.filter(f => typed === "" || f.toLowerCase().startsWith(typed)
                                                           || f === fontRow.entry.family)
                                return (matches.length > 0 ? matches : all).slice(0, 40).map(f => ({ id: f, name: f }))
                            }
                            current: fontRow.entry.family ?? ""
                            onChosen: id => Brand.setFont(fontRow.index, "", id, fontRow.entry.style ?? "", 0)
                        }
                    }
                    ChoicePill {
                        objectName: "brandFontWeight_" + fontRow.index
                        text: dialog.weightName(fontRow.entry.weight ?? 400)
                        options: dialog.weights
                        current: String(fontRow.entry.weight ?? 400)
                        Accessible.name: qsTr("%1 font weight").arg(fontRow.entry.role ?? "")
                        onChosen: id => Brand.setFont(fontRow.index, "", "", fontRow.entry.style ?? "", Number(id))
                    }
                    Text {
                        //: A short sample of a font
                        text: qsTr("Aa")
                        color: Theme.textPrimary
                        font.family: fontRow.entry.family ?? Theme.fontSans
                        font.weight: fontRow.entry.weight ?? 400
                        font.pixelSize: Theme.textLg
                        Accessible.ignored: true
                    }
                    IconButton {
                        objectName: "brandFontRemove_" + fontRow.index
                        compact: true
                        iconName: "trash-2"
                        tooltip: qsTr("Remove the %1 font from the kit").arg(fontRow.entry.role ?? "")
                        onClicked: Brand.removeFont(fontRow.index)
                    }
                }
            }
            PillButton {
                objectName: "brandAddFont"
                compact: true
                variant: "ghost"
                iconName: "plus"
                //: Adds a font role (e.g. "caption") to the brand kit
                text: qsTr("Add font role")
                enabled: dialog.fonts.length < 16
                onClicked: Brand.addFont(qsTr("role"))
            }

            // Logos: pictures of the project.
            SectionHeader {
                Layout.fillWidth: true
                text: qsTr("Logos")
            }
            Repeater {
                model: dialog.logos.length
                delegate: RowLayout {
                    id: logoRow
                    required property int index
                    readonly property var entry: dialog.logos[index] ?? ({})
                    objectName: "brandLogo_" + index
                    Layout.fillWidth: true
                    spacing: Theme.space(2)
                    Icon {
                        name: "image"
                        size: Theme.iconSm
                        color: Theme.textSecondary
                    }
                    FieldInput {
                        objectName: "brandLogoName_" + logoRow.index
                        Layout.preferredWidth: Theme.space(30)
                        text: logoRow.entry.name ?? ""
                        Accessible.name: qsTr("Logo %1 name").arg(logoRow.index + 1)
                        onEditingFinished: {
                            if (text.trim() !== "" && text.trim() !== logoRow.entry.name)
                                Brand.setLogo(logoRow.index, text, "")
                            else
                                text = logoRow.entry.name ?? ""
                        }
                    }
                    ChoicePill {
                        objectName: "brandLogoMedia_" + logoRow.index
                        Layout.fillWidth: true
                        text: logoRow.entry.mediaName !== "" ? logoRow.entry.mediaName : qsTr("Missing picture")
                        options: Brand.logoCandidates.map(m => ({ id: m.key, name: m.name }))
                        current: logoRow.entry.media ?? ""
                        Accessible.name: qsTr("%1 picture").arg(logoRow.entry.name ?? "")
                        onChosen: id => Brand.setLogo(logoRow.index, "", id)
                    }
                    IconButton {
                        objectName: "brandLogoRemove_" + logoRow.index
                        compact: true
                        iconName: "trash-2"
                        tooltip: qsTr("Remove %1 from the kit").arg(logoRow.entry.name ?? "")
                        onClicked: Brand.removeLogo(logoRow.index)
                    }
                }
            }
            PillButton {
                objectName: "brandAddLogo"
                compact: true
                variant: "ghost"
                iconName: "plus"
                text: qsTr("Add logo")
                enabled: Brand.logoCandidates.length > 0 && dialog.logos.length < 32
                onClicked: Brand.addLogo(qsTr("Logo"), Brand.logoCandidates[0].key)
            }
            Note {
                visible: Brand.logoCandidates.length === 0
                text: qsTr("Import a picture or a video into the Library to use it as a logo.")
            }
        }
    }

    Text {
        objectName: "brandKitMessage"
        Layout.fillWidth: true
        visible: text !== ""
        text: Brand.message
        wrapMode: Text.Wrap
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    FileDialog {
        id: importDialog
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Wabcut brand kits (*.wabcut-brand.json *.json)")]
        onAccepted: Brand.importKit(selectedFile)
    }
    FileDialog {
        id: exportDialog
        fileMode: FileDialog.SaveFile
        defaultSuffix: "wabcut-brand.json"
        nameFilters: [qsTr("Wabcut brand kits (*.wabcut-brand.json)")]
        onAccepted: Brand.exportKit(selectedFile)
    }

    footerData: [
        PillButton {
            objectName: "brandImportKit"
            variant: "ghost"
            iconName: "download"
            text: qsTr("Import kit…")
            onClicked: importDialog.open()
        },
        PillButton {
            objectName: "brandExportKit"
            variant: "ghost"
            iconName: "upload"
            text: qsTr("Export kit…")
            visible: dialog.hasKit
            onClicked: exportDialog.open()
        },
        PillButton {
            objectName: "brandRemoveKit"
            variant: "ghost"
            iconName: "trash-2"
            text: qsTr("Remove kit")
            visible: dialog.hasKit
            onClicked: Brand.removeKit()
        },
        PillButton {
            objectName: "closeBrandKit"
            variant: "primary"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    ]
}
