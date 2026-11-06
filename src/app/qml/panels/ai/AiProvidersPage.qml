pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Settings > AI > Providers (r07 rework; it replaced one long column of provider cards whose buttons ran off the edge):
// master-detail that scales to 50+ providers. On top: a search field, "+ Custom provider" and filter chips (All,
// Connected, each capability, Local, Custom). Left: the providers grouped Connected, your custom providers, cloud, and
// local or self-hosted, each with a status dot, its status and its capability icons. Right: the chosen provider's
// detail (AiProviderDetail: key, server address, models, data policy, spending). Narrower than 720 px the panes stack:
// the list, then the detail with a Back button.
Item {
    id: page
    objectName: "aiProvidersPage"

    required property var catalog
    /// The filter chip: "all", "connected", "local", "custom" or a capability id.
    property string filter: "all"
    /// The chosen provider's id.
    property string selected: ""
    /// Narrow layout: the detail shows instead of the list.
    property bool showingDetail: false
    readonly property bool narrow: page.width < Theme.space(180)

    /// "+ Custom provider" ("" = a new one) and a custom provider's Edit (the settings page owns the dialog).
    signal customProviderRequested(string providerId)
    /// A custom provider's Delete (the settings page asks first).
    signal deleteRequested(string providerId)
    /// The detail's "See usage".
    signal usageRequested()

    readonly property var chips: [
        { id: "all", name: qsTr("All"), icon: "" },
        { id: "connected", name: qsTr("Connected"), icon: "check" }
    ].concat(page.catalog.capabilities.map(c => ({ id: c.id, name: c.name, icon: c.icon }))).concat([
        { id: "local", name: qsTr("Local"), icon: "house" },
        { id: "custom", name: qsTr("Custom"), icon: "settings" }
    ])

    function matches(p: var, query: string): bool {
        const f = page.filter
        const ok = f === "all" ? true
                 : f === "connected" ? p.connected === true
                 : f === "local" ? (p.category === "local" || p.local === true)
                 : f === "custom" ? p.custom === true
                 : p.caps.indexOf(f) >= 0
        if (!ok)
            return false
        if (query === "")
            return true
        return String(p.name).toLowerCase().indexOf(query) >= 0 || String(p.id).toLowerCase().indexOf(query) >= 0
            || String(p.description).toLowerCase().indexOf(query) >= 0
    }
    /// The visible providers.
    readonly property var filtered: {
        const query = search.text.trim().toLowerCase()
        return page.catalog.providers.filter(p => page.matches(p, query))
    }
    /// The list's rows: group headers and providers.
    readonly property var entries: {
        const out = []
        for (let g = 0; g < page.catalog.groupOrder.length; ++g) {
            const group = page.catalog.groupOrder[g]
            const members = page.filtered.filter(p => p.group === group)
            if (members.length === 0)
                continue
            out.push({ kind: "header", id: "header_" + group, title: page.catalog.groupNames[group], count: members.length })
            for (let i = 0; i < members.length; ++i)
                out.push(Object.assign({ kind: "provider" }, members[i]))
        }
        return out
    }
    readonly property var current: page.catalog.provider(page.selected)

    /// Shows `providerId`'s detail.
    function select(providerId: string) {
        page.selected = providerId
        page.showingDetail = true
    }
    /// Sets the filter chip (Features' "Add a provider") and shows the first matching provider.
    function showFilter(filterId: string) {
        page.filter = filterId
        search.clear()
        page.showingDetail = false
        const first = page.entries.find(e => e.kind === "provider")
        if (first && !page.narrow)
            page.selected = first.id
    }
    function focusCustomButton() {
        customButton.forceActiveFocus(Qt.TabFocusReason)
    }

    Component.onCompleted: {
        if (page.selected === "") {
            const first = page.entries.find(e => e.kind === "provider")
            page.selected = first ? first.id : ""
        }
    }

    // ---- Toolbar: legacy keys, search, custom provider, filter chips ----
    Column {
        id: toolbar
        width: parent.width
        spacing: Theme.space(2)

        // Keys left in plaintext by the removed opt-in (review wave 4, P2-5): move them, then delete the file.
        Rectangle {
            id: legacyKeys
            objectName: "legacyKeysBanner"
            property string result: ""
            visible: AiController.legacyPlaintextKeys > 0 || result !== ""
            width: parent.width
            height: visible ? legacyColumn.implicitHeight + Theme.space(6) : 0
            radius: Theme.radiusCard
            color: Theme.raised
            border.color: Theme.warning
            border.width: Theme.hairline

            Column {
                id: legacyColumn
                x: Theme.space(4)
                y: Theme.space(3)
                width: parent.width - Theme.space(8)
                spacing: Theme.space(2)

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: legacyKeys.result !== "" ? legacyKeys.result
                          : qsTr("%n API key(s) are still saved in a plaintext file from an earlier version. Move them into %1 and delete the file?", "", AiController.legacyPlaintextKeys).arg(AiController.keyStoreName)
                    color: Theme.textPrimary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
                Flow {
                    width: parent.width
                    spacing: Theme.space(2)
                    visible: AiController.legacyPlaintextKeys > 0
                    PillButton {
                        objectName: "importLegacyKeys"
                        compact: true
                        text: qsTr("Move keys")
                        onClicked: legacyKeys.result = AiController.importPlaintextKeys()
                    }
                    PillButton {
                        objectName: "deleteLegacyKeys"
                        compact: true
                        text: qsTr("Delete file")
                        onClicked: legacyKeys.result = AiController.deletePlaintextKeys() ? qsTr("The plaintext key file was deleted.")
                                                                                          : qsTr("The plaintext key file could not be deleted.")
                    }
                }
            }
        }

        Row {
            width: parent.width
            spacing: Theme.space(2)

            FieldInput {
                id: search
                objectName: "aiProviderSearch"
                width: parent.width - customButton.width - parent.spacing
                iconName: "search"
                clearOnEscape: true
                placeholderText: qsTr("Search %n provider(s)", "", page.catalog.providers.length)
                Accessible.name: qsTr("Search providers")
            }
            PillButton {
                id: customButton
                objectName: "aiAddCustomProvider"
                anchors.verticalCenter: search.verticalCenter
                compact: true
                iconName: "plus"
                text: qsTr("Custom provider")
                tooltip: qsTr("Use your own server or any OpenAI-, Anthropic- or Gemini-compatible service")
                onClicked: page.customProviderRequested("")
            }
        }

        Flow {
            id: chipFlow
            objectName: "aiProviderFilters"
            width: parent.width
            spacing: Theme.space(1.5)
            Accessible.role: Accessible.Grouping
            Accessible.name: qsTr("Show providers")

            Repeater {
                model: page.chips
                delegate: AiChip {
                    required property var modelData
                    objectName: "aiProviderFilter_" + modelData.id
                    text: modelData.name
                    iconName: modelData.icon
                    checked: page.filter === modelData.id
                    onClicked: page.showFilter(modelData.id)
                }
            }
        }
    }

    // ---- The panes ----
    Item {
        id: panes
        anchors.top: toolbar.bottom
        anchors.topMargin: Theme.space(3)
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        readonly property real listWidth: page.narrow ? panes.width : Math.round(Math.max(Theme.space(60), Math.min(Theme.space(84), panes.width * 0.36)))

        ListView {
            id: list
            objectName: "aiProviderList"
            visible: !page.narrow || !page.showingDetail
            width: panes.listWidth
            height: parent.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: page.entries
            ScrollBar.vertical: ThinScrollBar {}

            footer: Item {
                width: list.width - Theme.space(3)
                height: page.entries.length === 0 ? noMatch.implicitHeight : 0

                Text {
                    id: noMatch
                    objectName: "aiProviderNoMatch"
                    width: parent.width
                    visible: page.entries.length === 0
                    topPadding: Theme.space(2)
                    text: qsTr("No provider matches. Try another filter, or add your own with Custom provider.")
                    wrapMode: Text.WordWrap
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }

            delegate: Item {
                id: entry
                required property var modelData
                readonly property bool header: entry.modelData.kind === "header"
                readonly property bool chosen: !entry.header && entry.modelData.id === page.selected

                objectName: entry.header ? "aiProviderGroup_" + entry.modelData.id : "aiProviderRow_" + entry.modelData.id
                width: ListView.view.width - Theme.space(3)
                height: entry.header ? Theme.space(8) : Theme.space(13)

                Text {
                    visible: entry.header
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.space(2)
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Theme.space(1.5)
                    text: entry.header ? qsTr("%1  %2").arg(entry.modelData.title).arg(entry.modelData.count) : ""
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.weight: Theme.weightLabel
                    Accessible.role: Accessible.Heading
                    Accessible.name: entry.header ? entry.modelData.title : ""
                }

                Rectangle {
                    id: rowFrame
                    visible: !entry.header
                    anchors.fill: parent
                    anchors.topMargin: Theme.space(0.5)
                    anchors.bottomMargin: Theme.space(0.5)
                    radius: Theme.radiusField
                    color: entry.chosen ? Theme.hover : rowMouse.containsMouse ? Qt.alpha(Theme.hover, 0.6) : "transparent"
                    border.width: entry.chosen ? Theme.hairline : 0
                    border.color: Theme.borderStrong
                    activeFocusOnTab: !entry.header
                    Accessible.role: Accessible.ListItem
                    Accessible.name: entry.header ? "" : entry.modelData.name
                    Accessible.description: entry.header ? "" : entry.modelData.statusText ?? ""
                    Accessible.selected: entry.chosen
                    Accessible.onPressAction: page.select(entry.modelData.id)
                    Keys.onSpacePressed: page.select(entry.modelData.id)
                    Keys.onReturnPressed: page.select(entry.modelData.id)

                    Behavior on color { ColorAnimation { duration: Theme.durFast } }

                    FocusRing {
                        targetRadius: Theme.radiusField
                        visible: rowFrame.activeFocus
                    }

                    Rectangle {
                        id: dot
                        x: Theme.space(3)
                        anchors.verticalCenter: parent.verticalCenter
                        width: Theme.dotSize
                        height: width
                        radius: width / 2
                        color: entry.header ? "transparent" : page.catalog.stateColor(entry.modelData.status)
                    }
                    Column {
                        anchors.left: dot.right
                        anchors.leftMargin: Theme.space(2.5)
                        anchors.right: capIcons.left
                        anchors.rightMargin: Theme.space(2)
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.space(0.5)

                        Text {
                            width: parent.width
                            text: entry.header ? "" : entry.modelData.name
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Theme.weightLabel
                        }
                        Text {
                            width: parent.width
                            text: entry.header ? "" : entry.modelData.statusText ?? ""
                            elide: Text.ElideRight
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                    }
                    // The capabilities as icons (at most four, then "+n").
                    Row {
                        id: capIcons
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.space(3)
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.space(1)

                        Repeater {
                            model: entry.header ? [] : entry.modelData.caps.slice(0, 4)
                            delegate: Icon {
                                required property string modelData
                                name: page.catalog.capabilityIcon(modelData)
                                size: Theme.iconXs
                                color: Theme.textTertiary
                            }
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !entry.header && entry.modelData.caps.length > 4
                            text: entry.header ? "" : qsTr("+%1").arg(entry.modelData.caps.length - 4)
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            rowFrame.forceActiveFocus(Qt.MouseFocusReason)
                            page.select(entry.modelData.id)
                        }
                    }
                }
            }
        }

        // The divider between the panes.
        Rectangle {
            visible: !page.narrow
            x: list.width + Theme.space(2)
            width: Theme.hairline
            height: parent.height
            color: Theme.border
        }

        AiProviderDetail {
            id: detail
            objectName: "aiProviderDetail"
            visible: page.current !== null && (!page.narrow || page.showingDetail)
            x: page.narrow ? 0 : list.width + Theme.space(5)
            width: parent.width - x
            height: parent.height
            catalog: page.catalog
            provider: page.current
            showBack: page.narrow
            onBackRequested: page.showingDetail = false
            onEditRequested: providerId => page.customProviderRequested(providerId)
            onDeleteRequested: providerId => page.deleteRequested(providerId)
            onUsageRequested: page.usageRequested()
        }
        Text {
            visible: page.current === null && !page.narrow
            x: list.width + Theme.space(5)
            width: parent.width - x
            topPadding: Theme.space(4)
            text: qsTr("Choose a provider to set it up.")
            wrapMode: Text.WordWrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textMd
        }
    }
}
