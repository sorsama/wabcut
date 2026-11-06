pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// The model picker of a Settings > AI > Features row (r07): a popover with a search field, "Automatic" (local first,
// then the first usable model), the capability's models grouped by provider (local providers first; a model that cannot
// run now is greyed with its reason), "Other model id" for any id a provider takes, and "Add a provider", which opens
// Providers filtered by this capability. Choosing closes the popover.
Popover {
    id: picker
    objectName: "aiModelPicker"

    required property var catalog
    property string capability: "chat"
    property string current: ""
    property string error: ""
    readonly property string capabilityName: picker.catalog.capabilityName(picker.capability)

    /// "Add a provider" (the page shows Providers filtered by `capability`).
    signal addProviderRequested(string capability)
    /// A choice was made (`modelId` "" = automatic).
    signal picked(string modelId)

    /// Chooses `modelId` ("" = automatic) and closes.
    function choose(modelId: string) {
        if (picker.catalog.setDefault(picker.capability, modelId)) {
            picker.picked(modelId)
            picker.close()
        } else {
            picker.error = qsTr("That model cannot be chosen here.")
        }
    }
    function useOther() {
        const problem = picker.catalog.useModelId(picker.capability, otherProvider.current, otherField.text)
        picker.error = problem
        if (problem === "") {
            picker.picked(otherField.text.trim())
            otherField.clear()
            picker.close()
        }
    }

    title: qsTr("%1 model").arg(picker.capabilityName)
    y: parent ? parent.height + Theme.space(1) : 0
    transformOrigin: Item.Top
    onAboutToShow: {
        picker.error = ""
        search.clear()
        const providers = picker.catalog.providersFor(picker.capability)
        if (otherProvider.current === "" || !providers.some(p => p.id === otherProvider.current))
            otherProvider.current = providers.length > 0 ? providers[0].id : ""
    }
    onOpened: search.forceActiveFocus(Qt.PopupFocusReason)

    // [{kind: "auto" | "header" | "model", ...}] for the list.
    readonly property var entries: {
        const query = search.text.trim().toLowerCase()
        const all = picker.catalog.modelsFor(picker.capability)
        const models = []
        for (let i = 0; i < all.length; ++i) {
            const m = all[i]
            if (query === "" || String(m.name).toLowerCase().indexOf(query) >= 0 || String(m.id).toLowerCase().indexOf(query) >= 0
                    || String(m.providerName).toLowerCase().indexOf(query) >= 0)
                models.push(m)
        }
        const local = models.filter(m => m.local === true)
        const ordered = local.concat(models.filter(m => m.local !== true))
        const out = query === "" ? [{ kind: "auto" }] : []
        let group = ""
        for (let i = 0; i < ordered.length; ++i) {
            const m = ordered[i]
            if (m.providerName !== group) {
                group = m.providerName
                out.push({ kind: "header", title: m.local === true ? qsTr("%1 · on this computer").arg(group) : group })
            }
            out.push(Object.assign({ kind: "model" }, m))
        }
        return out
    }

    Column {
        width: Theme.space(100)
        spacing: Theme.space(2)

        FieldInput {
            id: search
            objectName: "aiModelPickerSearch"
            width: parent.width
            iconName: "search"
            clearOnEscape: true
            placeholderText: qsTr("Search models")
            Accessible.name: qsTr("Search %1 models").arg(picker.capabilityName)
        }

        ListView {
            id: list
            objectName: "aiModelPickerList"
            width: parent.width
            height: Math.min(contentHeight, Theme.space(78))
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: picker.entries
            ScrollBar.vertical: ThinScrollBar {}

            delegate: Item {
                id: entry
                required property var modelData
                required property int index
                readonly property bool header: entry.modelData.kind === "header"
                readonly property bool automatic: entry.modelData.kind === "auto"
                readonly property bool usable: entry.automatic || entry.modelData.usable !== false
                readonly property bool chosen: entry.automatic ? picker.current === "" : entry.modelData.id === picker.current
                objectName: entry.automatic ? "aiModelPickerAuto" : entry.header ? "" : "aiModelPickerModel_" + entry.modelData.id

                width: ListView.view.width
                height: entry.header ? Theme.space(7) : rowTexts.implicitHeight + Theme.space(3)

                Text {
                    visible: entry.header
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.space(2)
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Theme.space(1)
                    text: entry.header ? entry.modelData.title : ""
                    color: Theme.textTertiary
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textXs
                    font.weight: Theme.weightLabel
                }

                Rectangle {
                    visible: !entry.header
                    anchors.fill: parent
                    anchors.rightMargin: Theme.space(2)
                    radius: Theme.radiusField
                    color: rowMouse.containsMouse && entry.usable ? Theme.hover : "transparent"
                    activeFocusOnTab: !entry.header && entry.usable
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: entry.automatic ? qsTr("Automatic") : (entry.modelData.name ?? "")
                    Accessible.description: entry.usable ? "" : (entry.modelData.reason ?? "")
                    Accessible.checked: entry.chosen
                    Accessible.onPressAction: if (entry.usable) picker.choose(entry.automatic ? "" : entry.modelData.id)
                    Keys.onSpacePressed: if (entry.usable) picker.choose(entry.automatic ? "" : entry.modelData.id)
                    Keys.onReturnPressed: if (entry.usable) picker.choose(entry.automatic ? "" : entry.modelData.id)

                    FocusRing {
                        targetRadius: Theme.radiusField
                        visible: parent.activeFocus
                    }

                    Icon {
                        id: check
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.space(2)
                        anchors.verticalCenter: parent.verticalCenter
                        name: "check"
                        size: Theme.iconSm
                        color: Theme.accentStroke
                        opacity: entry.chosen ? 1 : 0
                    }

                    Column {
                        id: rowTexts
                        anchors.left: check.right
                        anchors.leftMargin: Theme.space(2)
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.space(2)
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.space(0.5)
                        opacity: entry.usable ? 1 : 0.6

                        Text {
                            width: parent.width
                            text: entry.automatic ? qsTr("Automatic") : (entry.modelData.name ?? "")
                            elide: Text.ElideRight
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: entry.chosen ? Theme.weightLabel : Theme.weightBody
                        }
                        Text {
                            width: parent.width
                            text: entry.automatic ? qsTr("On this computer when it can, else the first provider that is ready")
                                  : !entry.usable ? (entry.modelData.reason ?? "")
                                  : [entry.modelData.id, entry.modelData.price ?? ""].filter(s => s !== "").join("  ·  ")
                            elide: Text.ElideRight
                            color: entry.usable ? Theme.textTertiary : Theme.warningText
                            font.family: entry.automatic || !entry.usable ? Theme.fontSans : Theme.fontMono
                            font.pixelSize: Theme.textXs
                        }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        enabled: !entry.header && entry.usable
                        hoverEnabled: true
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: picker.choose(entry.automatic ? "" : entry.modelData.id)
                    }
                }
            }
        }

        Text {
            width: parent.width
            visible: picker.entries.length === 0
            text: search.text !== "" ? qsTr("No model matches.") : qsTr("No provider offers this yet. Add one below.")
            wrapMode: Text.WordWrap
            color: Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }

        Rectangle {
            width: parent.width
            height: Theme.hairline
            color: Theme.border
        }

        // Any model id a provider takes (a newer model, an OpenRouter or Ollama id).
        Text {
            width: parent.width
            text: qsTr("Other model id")
            color: Theme.textSecondary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
            font.weight: Theme.weightLabel
        }
        AiChoiceButton {
            id: otherProvider
            objectName: "aiModelPickerProvider"
            width: parent.width
            maximumWidth: parent.width
            options: picker.catalog.providersFor(picker.capability).map(p => ({ id: p.id, name: p.name }))
            placeholder: qsTr("Choose a provider")
            tooltip: qsTr("The provider the model belongs to")
            onChosen: id => otherProvider.current = id
        }
        Row {
            width: parent.width
            spacing: Theme.space(2)

            AiTextField {
                id: otherField
                objectName: "aiModelPickerOtherField"
                width: parent.width - useButton.width - parent.spacing
                monospace: true
                placeholderText: qsTr("Model id, e.g. claude-sonnet-5-5")
                Accessible.name: qsTr("Model id to use")
                onAccepted: if (text.trim() !== "") picker.useOther()
            }
            PillButton {
                id: useButton
                objectName: "aiModelPickerUse"
                compact: true
                variant: "primary"
                text: qsTr("Use")
                enabled: otherField.text.trim() !== "" && otherProvider.current !== ""
                onClicked: picker.useOther()
            }
        }
        Text {
            width: parent.width
            visible: picker.error !== ""
            text: picker.error
            wrapMode: Text.WordWrap
            color: Theme.dangerText
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
        PillButton {
            objectName: "aiModelPickerAddProvider"
            compact: true
            variant: "ghost"
            iconName: "plus"
            text: qsTr("Add a provider")
            tooltip: qsTr("Show the providers that offer %1").arg(picker.capabilityName.toLowerCase())
            onClicked: {
                picker.close()
                picker.addProviderRequested(picker.capability)
            }
        }
    }
}
