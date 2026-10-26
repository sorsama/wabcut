pragma ComponentBehavior: Bound
import QtQuick

// Settings > Playback & performance (0.1.3): what the GPU status pill used to say in the title bar (the graphics API
// and adapter this window actually renders with, RendererInfo, and the video decode path), and Media & cache.
SettingsPage {
    id: page
    objectName: "performanceSettingsPage"

    title: qsTr("Playback & performance")
    description: qsTr("The graphics and decoding Wabcut uses, proxies and caches.")

    RendererInfo {
        id: renderer
        objectName: "settingsRendererInfo"
        window: page.Window.window
    }

    SettingsSection {
        title: qsTr("Graphics")

        SettingsRow {
            objectName: "settingsRenderApi"
            rowKey: "rendering"
            label: qsTr("Rendering")
            description: renderer.ready ? qsTr("%1 on %2").arg(renderer.api).arg(renderer.adapter || qsTr("an unknown adapter"))
                                        : qsTr("Detected when the window shows its first frame.")
            Rectangle {
                anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                width: Theme.dotSize + Theme.space(1)
                height: width
                radius: width / 2
                color: renderer.ready ? Theme.success : Theme.textTertiary
                Accessible.ignored: true
            }
        }
        SettingsRow {
            objectName: "settingsDecodeBackend"
            rowKey: "decoding"
            label: qsTr("Video decoding")
            description: Playback.decodeBackend !== ""
                         ? qsTr("Now: %1").arg(Playback.decodeBackend)
                         : qsTr("Hardware decoding is chosen per clip when playback starts.")
        }
        // P0 round (issue #10): which GPU previews, decodes and exports (Gpu). Automatic is the high-performance GPU
        // unless Windows Graphics settings choose one for Wabcut; the integrated GPU saves battery. Exports use a new
        // choice at once, the preview and decoding after a restart.
        SettingsRow {
            objectName: "settingsGraphicsCard"
            rowKey: "graphicsCard"
            label: qsTr("Graphics card")
            description: {
                const parts = []
                if (Gpu.restartNeeded)
                    //: %1 is a GPU
                    parts.push(qsTr("Exports use %1 now; the preview and playback switch to it when Wabcut restarts.").arg(Gpu.selectedName))
                else if (Gpu.inUseName !== "")
                    //: %1 is a GPU, %2 why ("the high-performance GPU, chosen automatically")
                    parts.push(qsTr("Now: %1 (%2).").arg(Gpu.inUseName).arg(Gpu.inUseReason))
                parts.push(Gpu.previewFollows
                           ? qsTr("Automatic uses the high-performance GPU unless Windows Graphics settings choose one for Wabcut. The integrated GPU saves battery.")
                           : qsTr("The system picks the preview's GPU; this choice orders the export encoders."))
                return parts.join(" ")
            }
            PillButton {
                objectName: "graphicsCardWindowsSettings"
                visible: Gpu.previewFollows
                compact: true
                variant: "ghost"
                text: qsTr("Windows settings")
                tooltip: qsTr("Open Windows Graphics settings, where a GPU can be set for Wabcut")
                onClicked: Gpu.openWindowsGraphicsSettings()
            }
            PillButton {
                id: gpuButton
                objectName: "graphicsCardButton"
                compact: true
                variant: "secondary"
                iconName: "chevron-down"
                LayoutMirroring.enabled: true    // label first, chevron last
                LayoutMirroring.childrenInherit: true
                readonly property var chosen: Gpu.adapters.find(a => a.key === Gpu.choice)
                text: Gpu.choice === "" || !chosen ? qsTr("Automatic") : chosen.name
                Accessible.name: qsTr("Graphics card: %1").arg(text)
                onClicked: gpuMenu.popup(gpuButton, 0, gpuButton.height + Theme.space(1))

                ContextMenu {
                    id: gpuMenu
                    objectName: "graphicsCardMenu"

                    ContextMenuItem {
                        objectName: "graphicsCardAutomatic"
                        text: Gpu.automaticText
                        checkable: true
                        checked: Gpu.choice === ""
                        onTriggered: Gpu.choice = ""
                    }
                    ContextMenuSeparator {}
                    Instantiator {
                        model: Gpu.adapters
                        delegate: ContextMenuItem {
                            required property var modelData
                            objectName: "graphicsCard_" + modelData.key
                            text: modelData.detail !== "" ? qsTr("%1 · %2").arg(modelData.name).arg(modelData.detail)
                                                          : modelData.name
                            checkable: true
                            checked: modelData.key === Gpu.choice
                            onTriggered: Gpu.choice = modelData.key
                        }
                        onObjectAdded: (index, object) => gpuMenu.addItem(object as ContextMenuItem) // after Automatic, in order
                        onObjectRemoved: (index, object) => gpuMenu.removeItem(object as ContextMenuItem)
                    }
                }
            }
        }
    }

    SettingsSection {
        title: qsTr("Media & cache")

        SettingsRow {
            rowKey: "mediaCache"
            label: qsTr("Proxies, preview quality and disk caches")
            description: qsTr("When Wabcut makes proxies, the playback resolution, and where caches live and how big they get.")
            PillButton {
                objectName: "mediaCacheSettingsButton"
                iconName: "gauge"
                text: qsTr("Open…")
                onClicked: page.actionRequested("media")
            }
        }
    }
}
