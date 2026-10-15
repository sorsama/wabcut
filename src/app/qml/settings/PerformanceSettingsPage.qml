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
            label: qsTr("Video decoding")
            description: Playback.decodeBackend !== ""
                         ? qsTr("Now: %1").arg(Playback.decodeBackend)
                         : qsTr("Hardware decoding is chosen per clip when playback starts.")
        }
    }

    SettingsSection {
        title: qsTr("Media & cache")

        SettingsRow {
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
