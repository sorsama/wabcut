pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic

// ● FONT PICKER (card 053): the one place a font is chosen - Effect Controls' Text section, the Essential Graphics
// editor, the Brand Kit and the caption styles all open this popover (usually through FontChoicePill). One search box over
// every installed family, ranked in groups with a header each (the `Fonts` singleton, FontCatalog.h):
//   Name         the family's name matches what was typed ("Thai" -> Noto Sans Thai, Leelawadee UI Thai)
//   Supports X   the family draws the language or script typed, in English or its own name (Thai / ไทย, Japanese / 日本語,
//                Arabic / عربي): its row shows a greeting in that script
//   Similar      letters in order, spaces and dashes ignored, small typos
// Every row is drawn in its own font. Filters (the button beside the box): language or script (several: the font draws all),
// Bold / Italic / several weights, type (Sans, Serif, Mono, Display, Handwriting, Symbol), source (Brand kit, Recently used,
// Favourites, Project fonts), sort, and hiding the fonts that cannot draw the layer's text. A bookmark on a row makes the
// family a favourite; a warning shows the letters a family cannot draw instead of a box. Up / Down move through the rows
// (the headers are skipped), Enter chooses, Escape closes.
Popover {
    id: picker
    objectName: "fontPicker"
    title: qsTr("Font")
    transformOrigin: Item.TopLeft

    /// The family in use ("" = the platform's default), or an extra's id: marked in the list.
    property string current: ""
    /// Choices that are not installed families, listed first while the box is empty or they match it: [{ id, name }] - the
    /// default font and the brand kit's font roles.
    property var extras: []
    /// What the font will draw (a layer's text, a caption sample): rows show a warning for letters a family cannot draw, and
    /// the filters offer to hide those families.
    property string sampleText: ""
    /// The language of that text ("th": a caption track's): the language filter starts on its script.
    property string language: ""
    /// A family or an extra's id was chosen.
    signal chosen(string id)

    // ---- The filters ----
    property bool filtersOpen: false
    property string source: "all"
    property string sort: "relevance"
    property var scriptKeys: []
    property var kindKeys: []
    property bool bold: false
    property bool italic: false
    property bool manyWeights: false
    property bool hideUnsupported: false
    readonly property int filterCount: scriptKeys.length + kindKeys.length + (bold ? 1 : 0) + (italic ? 1 : 0)
                                       + (manyWeights ? 1 : 0) + (source !== "all" ? 1 : 0) + (hideUnsupported ? 1 : 0)
    readonly property var brandFamilies: Brand.fontRoles.map(r => r.family)
    readonly property var filters: ({
        scripts: picker.scriptKeys, bold: picker.bold, italic: picker.italic, minWeights: picker.manyWeights ? 3 : 0,
        kinds: picker.kindKeys, source: picker.source, sort: picker.sort, hideUnsupported: picker.hideUnsupported,
        text: picker.sampleText, brandFamilies: picker.brandFamilies
    })
    /// The search: re-run when the text, a filter, the catalogue or the favourites change.
    readonly property var result: {
        const dependencies = (Fonts.ready ? 1 : 0) + Fonts.favourites.length + Fonts.recent.length
        return dependencies >= 0 ? Fonts.search(searchField.text, picker.filters) : ({ rows: [], total: 0, note: "" })
    }
    /// The extras that match the text (all of them while the box is empty), when no filter narrows the list.
    readonly property var visibleExtras: {
        const q = searchField.text.trim().toLowerCase()
        if (picker.filterCount > 0)
            return []
        return picker.extras.filter(e => q === "" || e.name.toLowerCase().indexOf(q) >= 0)
                            .map(e => ({ family: "", id: e.id, label: e.name, group: "extra", groupLabel: qsTr("Default and brand fonts"),
                                         sample: e.name, kind: "", favourite: false, isExtra: true }))
    }
    readonly property var rows: picker.visibleExtras.concat(picker.result.rows)

    function labelOf(family: string): string {
        return family
    }
    function toggleIn(list: var, key: string): var {
        const next = list.slice()
        const at = next.indexOf(key)
        if (at >= 0)
            next.splice(at, 1)
        else
            next.push(key)
        return next
    }
    function clearFilters() {
        scriptKeys = []
        kindKeys = []
        bold = false
        italic = false
        manyWeights = false
        source = "all"
        hideUnsupported = false
    }
    function choose(id: string) {
        picker.chosen(id)
        picker.close()
    }
    function chooseRow(row: var) {
        if (!row)
            return
        picker.choose(row.isExtra === true ? row.id : row.family)
    }

    onAboutToShow: {
        // The language filter follows the caption's language, or the script of the text already typed in the layer.
        const fromLanguage = picker.language !== "" ? Fonts.scriptOfLanguage(picker.language) : ""
        const needed = picker.sampleText !== "" ? Fonts.scriptsOfText(picker.sampleText) : []
        const nonLatin = needed.filter(k => k !== "latin")
        picker.scriptKeys = fromLanguage !== "" && fromLanguage !== "latin" ? [fromLanguage] : nonLatin.length > 0 ? [nonLatin[0]] : []
    }
    onOpened: {
        searchField.text = ""
        searchField.forceActiveFocus(Qt.PopupFocusReason)
        let at = 0
        for (let i = 0; i < picker.rows.length; ++i) {
            if ((picker.rows[i].isExtra === true ? picker.rows[i].id : picker.rows[i].family) === picker.current) {
                at = i
                break
            }
        }
        fontList.currentIndex = picker.rows.length > 0 ? at : -1
        fontList.positionViewAtIndex(Math.max(0, at), ListView.Center)
    }

    component Chip: PillButton {
        id: chip
        property bool selected: false
        compact: true
        variant: chip.selected ? "primary" : "secondary"
        Accessible.checkable: true
        Accessible.checked: chip.selected
    }
    component FilterLabel: Text {
        Layout.fillWidth: true
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        font.weight: Font.DemiBold
    }

    Column {
        width: Theme.space(88)
        spacing: Theme.space(2)

        RowLayout {
            width: parent.width
            spacing: Theme.space(2)
            FieldInput {
                id: searchField
                objectName: "fontPickerFilter"
                Layout.fillWidth: true
                iconName: "search"
                clearOnEscape: true
                placeholderText: qsTr("Find a font, language or script")
                Accessible.name: qsTr("Find a font")
                onTextChanged: fontList.currentIndex = picker.rows.length > 0 ? 0 : -1
                Keys.onDownPressed: fontList.currentIndex = Math.min(fontList.count - 1, fontList.currentIndex + 1)
                Keys.onUpPressed: fontList.currentIndex = Math.max(0, fontList.currentIndex - 1)
                onAccepted: picker.chooseRow(picker.rows[Math.max(0, fontList.currentIndex)])
            }
            PillButton {
                id: filtersButton
                objectName: "fontPickerFilters"
                compact: true
                iconName: "funnel"
                variant: picker.filtersOpen || picker.filterCount > 0 ? "primary" : "secondary"
                text: picker.filterCount > 0 ? qsTr("Filters (%1)").arg(picker.filterCount) : qsTr("Filters")
                tooltip: qsTr("Filter by language, style, type or source, and sort")
                Accessible.name: qsTr("Font filters")
                onClicked: picker.filtersOpen = !picker.filtersOpen
            }
        }

        // ---- Filters ----
        ColumnLayout {
            id: filtersPanel
            objectName: "fontPickerFiltersPanel"
            width: parent.width
            visible: picker.filtersOpen
            spacing: Theme.space(1.5)

            FilterLabel { text: qsTr("Language or script (a font must draw all you choose)") }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Repeater {
                    model: Fonts.scripts
                    delegate: Chip {
                        required property var modelData
                        objectName: "fontScript_" + modelData.key
                        text: modelData.name
                        tooltip: modelData.label
                        selected: picker.scriptKeys.indexOf(modelData.key) >= 0
                        onClicked: picker.scriptKeys = picker.toggleIn(picker.scriptKeys, modelData.key)
                    }
                }
            }
            FilterLabel { text: qsTr("Style") }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Chip {
                    objectName: "fontBold"
                    text: qsTr("Has bold")
                    selected: picker.bold
                    onClicked: picker.bold = !picker.bold
                }
                Chip {
                    objectName: "fontItalic"
                    text: qsTr("Has italic")
                    selected: picker.italic
                    onClicked: picker.italic = !picker.italic
                }
                Chip {
                    objectName: "fontWeights"
                    text: qsTr("3 or more weights")
                    selected: picker.manyWeights
                    onClicked: picker.manyWeights = !picker.manyWeights
                }
            }
            FilterLabel { text: qsTr("Type") }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Repeater {
                    model: [{ key: "sans", name: qsTr("Sans") }, { key: "serif", name: qsTr("Serif") },
                            { key: "mono", name: qsTr("Mono") }, { key: "display", name: qsTr("Display") },
                            { key: "script", name: qsTr("Handwriting") }, { key: "symbol", name: qsTr("Symbol") }]
                    delegate: Chip {
                        required property var modelData
                        objectName: "fontKind_" + modelData.key
                        text: modelData.name
                        selected: picker.kindKeys.indexOf(modelData.key) >= 0
                        onClicked: picker.kindKeys = picker.toggleIn(picker.kindKeys, modelData.key)
                    }
                }
            }
            FilterLabel { text: qsTr("Source") }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Repeater {
                    model: [{ key: "all", name: qsTr("Installed") }, { key: "brand", name: qsTr("Brand kit") },
                            { key: "recent", name: qsTr("Recently used") }, { key: "favourites", name: qsTr("Favourites") },
                            { key: "project", name: qsTr("Project fonts") }]
                    delegate: Chip {
                        required property var modelData
                        objectName: "fontSource_" + modelData.key
                        text: modelData.name
                        selected: picker.source === modelData.key
                        onClicked: picker.source = modelData.key
                    }
                }
            }
            FilterLabel { text: qsTr("Sort") }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.space(1)
                Repeater {
                    model: [{ key: "relevance", name: qsTr("Relevance") }, { key: "az", name: qsTr("A to Z") },
                            { key: "recent", name: qsTr("Recently used") }]
                    delegate: Chip {
                        required property var modelData
                        objectName: "fontSort_" + modelData.key
                        text: modelData.name
                        selected: picker.sort === modelData.key
                        onClicked: picker.sort = modelData.key
                    }
                }
            }
            Toggle {
                objectName: "fontHideUnsupported"
                Layout.fillWidth: true
                visible: picker.sampleText !== ""
                text: qsTr("Hide fonts that cannot draw this text")
                tooltip: qsTr("Fonts without the letters of the text would show empty boxes")
                checked: picker.hideUnsupported
                onToggled: {
                    picker.hideUnsupported = checked
                    checked = Qt.binding(() => picker.hideUnsupported)
                }
            }
            PillButton {
                objectName: "fontClearFilters"
                compact: true
                visible: picker.filterCount > 0
                text: qsTr("Clear filters")
                onClicked: picker.clearFilters()
            }
        }

        ListView {
            id: fontList
            objectName: "fontPickerList"
            width: parent.width
            height: picker.filtersOpen ? Theme.space(64) : Theme.space(84)
            clip: true
            model: picker.rows
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            ScrollBar.vertical: ThinScrollBar {}
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Fonts")
            // Each group's first row carries the group's header above it (a JS array has no roles for ListView sections).
            delegate: Item {
                id: cell
                required property var modelData
                required property int index
                readonly property bool firstOfGroup: cell.index === 0 || picker.rows[cell.index - 1].groupLabel !== cell.modelData.groupLabel
                readonly property real headerHeight: cell.firstOfGroup ? Theme.space(5) : 0
                width: ListView.view ? ListView.view.width : 0
                height: cell.headerHeight + Theme.controlHeight + Theme.space(1)
                Accessible.ignored: true

                Text {
                    objectName: "fontGroupHeader"
                    visible: cell.firstOfGroup
                    x: Theme.space(2)
                    y: Theme.space(1.5)
                    width: parent.width - Theme.space(4)
                    text: cell.modelData.groupLabel
                    elide: Text.ElideRight
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.weight: Font.DemiBold
                    font.letterSpacing: Theme.labelTracking
                }
            Rectangle {
                id: row
                readonly property var modelData: cell.modelData
                readonly property int index: cell.index
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                readonly property bool isExtra: row.modelData.isExtra === true
                readonly property string rowId: row.isExtra ? row.modelData.id : row.modelData.family
                readonly property string display: row.isExtra ? row.modelData.label : row.modelData.family
                readonly property bool sampled: !row.isExtra && row.modelData.sample !== row.modelData.family
                readonly property string missing: !row.isExtra && picker.sampleText !== ""
                                                  ? Fonts.missingGlyphs(row.modelData.family, picker.sampleText) : ""
                height: Theme.controlHeight + Theme.space(1)
                radius: Theme.radiusField
                color: rowHover.hovered || cell.ListView.isCurrentItem ? Theme.hover : "transparent"
                border.width: row.rowId === picker.current ? Theme.hairline : 0
                border.color: Theme.accentStroke
                Accessible.role: Accessible.ListItem
                Accessible.name: row.missing !== "" ? qsTr("%1, cannot draw %2").arg(row.display).arg(row.missing) : row.display
                Accessible.description: row.modelData.groupLabel
                Accessible.onPressAction: picker.chooseRow(row.modelData)

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space(2)
                    anchors.rightMargin: Theme.space(1)
                    spacing: Theme.space(2)
                    // Each row in its own font: the greeting in the script when a language is asked for, else the name.
                    Text {
                        Layout.fillWidth: !row.sampled
                        Layout.preferredWidth: row.sampled ? parent.width * 0.5 : -1
                        text: row.isExtra ? row.modelData.label : row.modelData.sample
                        elide: Text.ElideRight
                        color: Theme.textPrimary
                        font.family: row.isExtra ? Theme.fontSans : row.modelData.family
                        font.pixelSize: Theme.textMd
                    }
                    Text {
                        visible: row.sampled
                        Layout.fillWidth: true
                        text: row.modelData.family
                        elide: Text.ElideRight
                        color: Theme.textTertiary
                        font.family: Theme.fontSans
                        font.pixelSize: Theme.textXs
                    }
                    Icon {
                        objectName: "fontMissingGlyphs"
                        visible: row.missing !== ""
                        name: "triangle-alert"
                        size: Theme.iconSm
                        color: Theme.warningText
                        id: warnIcon
                        HoverHandler {
                            id: warnHover
                        }
                        Loader {
                            active: warnHover.hovered
                            sourceComponent: Tooltip {
                                parent: warnIcon
                                visible: true
                                text: qsTr("Cannot draw: %1").arg(row.missing)
                            }
                        }
                    }
                    IconButton {
                        objectName: "fontFavourite_" + row.modelData.family
                        visible: !row.isExtra && (rowHover.hovered || row.modelData.favourite === true || cell.ListView.isCurrentItem)
                        compact: true
                        checkable: true
                        checked: row.modelData.favourite === true
                        iconName: "bookmark"
                        tooltip: row.modelData.favourite === true ? qsTr("Remove from favourites") : qsTr("Add to favourites")
                        Accessible.name: qsTr("Favourite: %1").arg(row.modelData.family)
                        onClicked: Fonts.setFavourite(row.modelData.family, !(row.modelData.favourite === true))
                    }
                }
                HoverHandler {
                    id: rowHover
                }
                TapHandler {
                    onTapped: picker.chooseRow(row.modelData)
                }
            }
            }
        }

        Text {
            objectName: "fontPickerStatus"
            width: parent.width
            wrapMode: Text.Wrap
            text: picker.result.note !== "" ? picker.result.note
                  : picker.result.total === 0 ? qsTr("No font matches")
                  : qsTr("%n font(s)", "", picker.result.total)
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textXs
        }
    }
}
