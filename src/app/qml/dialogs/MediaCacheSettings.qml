pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts

// Settings > Media & cache (MediaReadiness): when proxies are made and how, what the monitors decode, and the disk
// caches (proxies, thumbnails, waveforms, analysis results) with their sizes, limits and cleaning. Opened from the
// Settings popover, the library's "Proxies" pill and the monitor's quality menu (MediaReadiness.openSettings()).
WabcutDialog {
    id: dialog
    objectName: "mediaCacheSettings"

    title: qsTr("Media & cache")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(190), Theme.space(190))

    onOpened: MediaReadiness.scanCaches()

    component Label: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }
    // One "Limit" menu entry of a cache row.
    component LimitItem: ContextMenuItem {
        property real bytes: 0
        property var cache: null
        text: bytes === 0 ? qsTr("No limit") : qsTr("%1 GB").arg(bytes / 1e9)
        checkable: true
        checked: cache !== null && cache.limitBytes === bytes
        onTriggered: MediaReadiness.setCacheLimit(cache.id, bytes)
    }
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    // ---- Proxies ----
    SectionHeader {
        Layout.fillWidth: true
        text: qsTr("Proxies")
    }
    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.space(4)
        rowSpacing: Theme.space(2)

        Label { text: qsTr("Make proxies") }
        PillTabs {
            objectName: "proxyPolicyTabs"
            Layout.fillWidth: true
            compact: true
            readonly property var values: ["off", "auto", "always"]
            model: [qsTr("Off"), qsTr("Auto"), qsTr("Always")]
            currentIndex: values.indexOf(MediaReadiness.policy)
            onActivated: index => {
                MediaReadiness.policy = values[index]
                currentIndex = Qt.binding(() => values.indexOf(MediaReadiness.policy))
            }
        }

        Label { text: qsTr("Format") }
        PillTabs {
            objectName: "proxyFormatTabs"
            Layout.fillWidth: true
            compact: true
            readonly property var values: ["prores", "dnxhr", "h264"]
            //: Proxy codecs: ProRes Proxy, DNxHR LB, all-intra H.264 (fast, uses the GPU encoder when it can)
            model: [qsTr("ProRes Proxy"), qsTr("DNxHR LB"), qsTr("H.264 (fast)")]
            currentIndex: values.indexOf(MediaReadiness.proxyFormat)
            onActivated: index => {
                MediaReadiness.proxyFormat = values[index]
                currentIndex = Qt.binding(() => values.indexOf(MediaReadiness.proxyFormat))
            }
        }

        Label { text: qsTr("Size") }
        PillTabs {
            objectName: "proxySizeTabs"
            Layout.fillWidth: true
            compact: true
            model: [qsTr("Half (1/2)"), qsTr("Quarter (1/4)")]
            currentIndex: MediaReadiness.proxyDivisor === 4 ? 1 : 0
            onActivated: index => {
                MediaReadiness.proxyDivisor = index === 1 ? 4 : 2
                currentIndex = Qt.binding(() => MediaReadiness.proxyDivisor === 4 ? 1 : 0)
            }
        }
    }
    Note {
        text: qsTr("Auto makes proxies for media this computer cannot play smoothly: larger than 4K, 10-bit or 4:2:2 H.264 without a hardware decoder, 10-bit HEVC on machines without one, and clips that dropped frames in playback. Proxies are made in the background, pause while you play or drag, and start with what is under the playhead. Exports always use the originals.")
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)

        Note {
            objectName: "proxyFolderText"
            text: qsTr("Saved in %1").arg(MediaReadiness.proxyFolder)
            elide: Text.ElideMiddle
            wrapMode: Text.NoWrap
        }
        PillButton {
            compact: true
            text: qsTr("Show")
            tooltip: qsTr("Show the proxy folder")
            onClicked: MediaReadiness.revealProxyFolder()
        }
    }

    // ---- Preview ----
    SectionHeader {
        Layout.fillWidth: true
        text: qsTr("Preview")
    }
    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.space(4)
        rowSpacing: Theme.space(2)

        Label { text: qsTr("Quality") }
        PillTabs {
            objectName: "previewQualityTabs"
            Layout.fillWidth: true
            compact: true
            readonly property var values: ["auto", "full", "half", "quarter"]
            model: [qsTr("Auto"), qsTr("Full"), "1/2", "1/4"]
            currentIndex: values.indexOf(MediaReadiness.previewQuality)
            onActivated: index => {
                MediaReadiness.previewQuality = values[index]
                currentIndex = Qt.binding(() => values.indexOf(MediaReadiness.previewQuality))
            }
        }
        Item { implicitWidth: 1; implicitHeight: 1 }
        Toggle {
            objectName: "useProxiesToggle"
            text: qsTr("Use proxies in the monitors")
            checked: MediaReadiness.useProxies
            onToggled: MediaReadiness.useProxies = checked
        }
    }
    // The first-run performance check (I.7) suggests these settings; run it again here.
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)

        Note {
            text: PerformanceCheck.hasReport
                  //: %1 is a date and time
                  ? qsTr("Performance check: last run %1.").arg(PerformanceCheck.reportTime)
                  : qsTr("A ten-second performance check suggests proxy and preview settings for this computer.")
        }
        PillButton {
            objectName: "runPerformanceCheck"
            compact: true
            text: qsTr("Performance check")
            onClicked: {
                dialog.close()
                PerformanceCheck.openCheck()
            }
        }
    }

    // ---- Disk caches ----
    SectionHeader {
        Layout.fillWidth: true
        text: qsTr("Disk caches")
    }
    Repeater {
        model: MediaReadiness.caches

        delegate: RowLayout {
            id: cacheRow
            required property var modelData
            Layout.fillWidth: true
            spacing: Theme.space(3)
            objectName: "cacheRow_" + modelData.id

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                Label {
                    text: cacheRow.modelData.name
                    color: Theme.textPrimary
                    font.weight: Font.DemiBold
                }
                Note {
                    text: cacheRow.modelData.inUseText !== ""
                          ? qsTr("%1 in %n file(s) · %2", "", cacheRow.modelData.files).arg(cacheRow.modelData.sizeText).arg(cacheRow.modelData.inUseText)
                          : qsTr("%1 in %n file(s)", "", cacheRow.modelData.files).arg(cacheRow.modelData.sizeText)
                }
            }
            PillButton {
                id: limitButton
                compact: true
                text: qsTr("Limit: %1").arg(cacheRow.modelData.limitText)
                tooltip: qsTr("Oldest files beyond the limit are deleted (never a proxy the project uses)")
                onClicked: limitMenu.popup(limitButton, 0, limitButton.height)

                ContextMenu {
                    id: limitMenu
                    LimitItem { bytes: 0; cache: cacheRow.modelData }
                    LimitItem { bytes: 1e9; cache: cacheRow.modelData }
                    LimitItem { bytes: 5e9; cache: cacheRow.modelData }
                    LimitItem { bytes: 20e9; cache: cacheRow.modelData }
                    LimitItem { bytes: 50e9; cache: cacheRow.modelData }
                    LimitItem { bytes: 200e9; cache: cacheRow.modelData }
                }
            }
            PillButton {
                compact: true
                text: qsTr("Clean")
                enabled: cacheRow.modelData.bytes > 0 && !MediaReadiness.cacheBusy
                tooltip: qsTr("Delete what can be rebuilt from the originals")
                onClicked: MediaReadiness.cleanCache(cacheRow.modelData.id)
            }
        }
    }
    Note {
        objectName: "cacheTotalText"
        text: MediaReadiness.cacheBusy ? qsTr("Measuring…") : qsTr("%1 in total. Everything here can be rebuilt from the original media; proxies the project uses are never deleted.").arg(MediaReadiness.cacheTotalText)
    }

    footerData: [
        PillButton {
            objectName: "cleanAllButton"
            text: qsTr("Clean regenerable data")
            enabled: !MediaReadiness.cacheBusy
            onClicked: MediaReadiness.cleanCache("")
        },
        PillButton {
            objectName: "mediaCacheDone"
            variant: "primary"
            text: qsTr("Done")
            onClicked: dialog.close()
        }
    ]
}
