pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

// Settings > AI > Features (r07; it replaced the Models tab and the old "Assistant model" row): one card per AI
// capability (assistant, transcription, translation, voice, image, video, sound effects) saying what it powers, what it
// uses right now (model and provider, on this computer or in the cloud, or why nothing can run) and a picker to change
// it: Automatic, any model of a provider that offers the capability, or any model id. "Add a provider" opens Providers
// filtered by that capability. Below: the AI helpers' CPU limit and what always stays on this computer.
Item {
    id: page
    objectName: "aiFeaturesPage"

    required property var catalog

    /// "Add a provider" of a row (the settings page shows Providers filtered by `capability`).
    signal addProviderRequested(string capability)

    /// Scrolls to the row of `capabilityId`, flashes it and focuses its picker (the Settings search). False if none.
    function reveal(capabilityId: string): bool {
        const index = page.catalog.capabilities.findIndex(c => c.id === capabilityId)
        const row = index >= 0 ? rows.itemAt(index) as FeatureRow : null
        if (!row)
            return false
        flick.contentY = Math.max(0, Math.min(row.y - Theme.space(2), flick.contentHeight - flick.height))
        row.flash()
        row.focusPicker()
        return true
    }
    /// Opens the picker of `capabilityId` (tests, screenshots).
    function openPicker(capabilityId: string): bool {
        const index = page.catalog.capabilities.findIndex(c => c.id === capabilityId)
        const row = index >= 0 ? rows.itemAt(index) as FeatureRow : null
        if (!row)
            return false
        row.openPicker()
        return true
    }

    component FeatureRow: Rectangle {
        id: row

        required property var modelData
        required property int index
        readonly property string capability: row.modelData.id
        readonly property var route: page.catalog.route(row.capability)
        readonly property string chosen: page.catalog.defaultModel(row.capability)
        readonly property bool wide: row.width >= Theme.space(150)
        readonly property real pickerWidth: row.wide ? Math.min(Theme.space(72), row.width * 0.42) : row.width - texts.x - Theme.space(4)

        function flash() {
            highlight.opacity = 1
            flashFade.restart()
        }
        function focusPicker() {
            pickerButton.forceActiveFocus(Qt.TabFocusReason)
        }
        function openPicker() {
            picker.current = row.chosen
            picker.open()
        }

        objectName: "aiFeatureRow_" + row.capability
        width: parent ? parent.width : implicitWidth
        height: implicitHeight
        implicitHeight: Math.max(texts.implicitHeight, row.wide ? controls.implicitHeight : 0)
                        + (row.wide ? 0 : controls.implicitHeight + Theme.space(3)) + Theme.space(8)
        radius: Theme.radiusCard
        color: Theme.raised
        border.width: Theme.hairline
        border.color: Theme.border

        Accessible.role: Accessible.Grouping
        Accessible.name: row.modelData.name

        Rectangle {
            id: badge
            x: Theme.space(4)
            y: Theme.space(4)
            width: Theme.controlHeight
            height: width
            radius: width / 2
            color: Theme.hover

            Icon {
                anchors.centerIn: parent
                name: row.modelData.icon
                size: Theme.iconSm
                color: Theme.textSecondary
            }
        }

        Column {
            id: texts
            anchors.left: badge.right
            anchors.leftMargin: Theme.space(3)
            anchors.right: row.wide ? controls.left : parent.right
            anchors.rightMargin: Theme.space(4)
            y: Theme.space(4)
            spacing: Theme.space(1)

            Text {
                width: parent.width
                text: row.modelData.name
                wrapMode: Text.WordWrap
                color: Theme.textPrimary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textLg
                font.weight: Theme.weightLabel
            }
            Text {
                width: parent.width
                text: row.modelData.description
                wrapMode: Text.WordWrap
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Row {
                width: parent.width
                spacing: Theme.space(1.5)

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.dotSize
                    height: width
                    radius: width / 2
                    color: !row.route || !row.route.usable ? Theme.warning : row.route.local ? Theme.info : Theme.success
                }
                Text {
                    objectName: "aiFeatureStatus_" + row.capability
                    width: parent.width - Theme.dotSize - parent.spacing
                    text: !row.route ? qsTr("Not set up")
                          : !row.route.usable ? (row.route.reason !== "" ? row.route.reason : qsTr("Not set up"))
                          : row.route.local ? qsTr("%1 · on this computer").arg(row.route.modelName)
                          : qsTr("%1 · %2 (cloud)").arg(row.route.modelName).arg(row.route.providerName)
                    wrapMode: Text.WordWrap
                    color: row.route && row.route.usable ? Theme.textSecondary : Theme.warningText
                    font.family: Theme.fontSans
                    font.pixelSize: Theme.textSm
                }
            }
        }

        Flow {
            id: controls
            x: row.wide ? row.width - width - Theme.space(4) : texts.x
            y: row.wide ? Math.max(Theme.space(4), (row.height - height) / 2) : texts.y + texts.height + Theme.space(3)
            width: row.pickerWidth
            spacing: Theme.space(2)
            layoutDirection: row.wide ? Qt.RightToLeft : Qt.LeftToRight

            AiChoiceButton {
                id: pickerButton
                objectName: "aiFeaturePicker_" + row.capability
                width: row.wide ? controls.width : Math.min(implicitWidth, controls.width)
                maximumWidth: controls.width
                compact: false
                text: row.chosen === "" ? qsTr("Automatic") : (row.route && row.route.model === row.chosen ? row.route.modelName : row.chosen)
                secondaryText: row.chosen === "" && row.route && row.route.usable ? row.route.modelName
                               : row.route && row.route.usable && !row.route.local ? row.route.providerName : ""
                tooltip: qsTr("The model %1 uses").arg(row.modelData.name)
                onClicked: row.openPicker()
            }
            PillButton {
                objectName: "aiFeatureAddProvider_" + row.capability
                compact: true
                variant: "ghost"
                iconName: "plus"
                text: qsTr("Add a provider")
                tooltip: qsTr("Providers that offer %1").arg(row.modelData.name.toLowerCase())
                onClicked: page.addProviderRequested(row.capability)
            }
        }

        AiModelPicker {
            id: picker
            parent: pickerButton
            catalog: page.catalog
            capability: row.capability
            onAddProviderRequested: capabilityId => page.addProviderRequested(capabilityId)
        }

        Rectangle {
            id: highlight
            objectName: "aiFeatureFlash"
            anchors.fill: parent
            radius: parent.radius
            color: Theme.accentWash
            border.width: Theme.hairline
            border.color: Theme.accentStroke
            opacity: 0
            visible: opacity > 0
        }
        SequentialAnimation {
            id: flashFade
            PauseAnimation { duration: 1200 }
            NumberAnimation { target: highlight; property: "opacity"; to: 0; duration: Theme.durCollapse * 2; easing.type: Easing.OutCubic }
        }
    }

    Flickable {
        id: flick
        objectName: "aiFeatureList"
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: column.implicitHeight + Theme.space(2)
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ThinScrollBar {}

        Column {
            id: column
            width: flick.width - Theme.space(3)
            spacing: Theme.space(2)

            Text {
                width: parent.width
                bottomPadding: Theme.space(1)
                text: qsTr("Choose what each AI feature uses. Automatic picks a model on this computer when there is one, else the first provider that is ready. Smart search always stays on this computer.")
                wrapMode: Text.WordWrap
                color: Theme.textTertiary
                font.family: Theme.fontSans
                font.pixelSize: Theme.textSm
            }
            Repeater {
                id: rows
                model: page.catalog.capabilities
                delegate: FeatureRow {}
            }

            Column {
                width: parent.width
                topPadding: Theme.space(2)
                spacing: Theme.space(2)

                SectionHeader {
                    text: qsTr("On this computer")
                }
                Rectangle {
                    width: parent.width
                    height: localColumn.implicitHeight + Theme.space(8)
                    radius: Theme.radiusCard
                    color: Theme.raised
                    border.width: Theme.hairline
                    border.color: Theme.border

                    Column {
                        id: localColumn
                        x: Theme.space(4)
                        y: Theme.space(4)
                        width: parent.width - Theme.space(8)
                        spacing: Theme.space(2)

                        // 0.2: the AI helpers on this computer run below normal priority under a CPU cap, so playback stays
                        // smooth while they work in the background.
                        Text {
                            width: parent.width
                            text: qsTr("AI helpers' CPU")
                            color: Theme.textPrimary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textMd
                            font.weight: Theme.weightLabel
                        }
                        Text {
                            width: parent.width
                            text: qsTr("Transcription, analysis and the local assistant run below normal priority. A lower limit keeps playback smooth while they work.")
                            wrapMode: Text.WordWrap
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textSm
                        }
                        PillTabs {
                            id: helperCpu
                            readonly property var limits: [100, 75, 50, 25]
                            objectName: "aiHelperCpuLimit"
                            width: Math.min(parent.width, Theme.space(100))
                            compact: true
                            model: [qsTr("No limit"), qsTr("75 %"), qsTr("50 %"), qsTr("25 %")]
                            currentIndex: Math.max(0, helperCpu.limits.indexOf(AiController.helperCpuLimit))
                            Accessible.name: qsTr("Limit the CPU use of AI helpers on this computer")
                            onActivated: index => AiController.helperCpuLimit = helperCpu.limits[index]
                        }
                        Text {
                            width: parent.width
                            topPadding: Theme.space(1)
                            text: qsTr("Model list of %1. Prices are the providers' list prices and drive the spend caps in Usage.").arg(AiController.registryUpdated)
                            wrapMode: Text.WordWrap
                            color: Theme.textTertiary
                            font.family: Theme.fontSans
                            font.pixelSize: Theme.textXs
                        }
                    }
                }
            }
        }
    }
}
