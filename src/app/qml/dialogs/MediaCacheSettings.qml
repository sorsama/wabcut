pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic as QQC
import QtQuick.Dialogs
import QtQuick.Layouts

// Settings > Media & cache (MediaReadiness): when proxies are made and how, what the monitors decode, and the disk
// caches (proxies, thumbnails, waveforms, analysis results) with their sizes, limits and cleaning. Opened from the
// Settings popover, the library's "Proxies" pill and the monitor's quality menu (MediaReadiness.openSettings()).
// P1 #48 (stream 3E): where proxies go (next to the project or in one folder, at once) and the app-wide media cache and
// AI folders (when Wabcut restarts, with Restart now), each with the free space on its drive; files already made stay
// where they are (the cache rows clean them).
// P1 #58 (stream 2A): the project's render files are a row of the disk caches (their size; Clean deletes them all, as
// Sequence > Delete Render Files does per sequence; no size limit), and the limits are kept automatically (a toggle).
// Fix round 1 (F1A): only the thumbnails' and waveforms' limits exist; proxies and analysis results, which projects may
// still use, are deleted by their Clean alone, and opening this dialog never deletes anything.
// Fix round 2 (F2C): Clean deletes only files Wabcut made (the proxies its index records, analysis cache entries), and
// asks first with what goes and what stays; AI masks and chat logs, which cannot be rebuilt, have their own row whose
// Clean says so, and "Clean regenerable data" leaves them.
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
    // Secondary copy: the folder paths, the free space and the long explanations a user reads before choosing
    // (QA: textTertiary's small face measured 3.3:1 on the light theme's raised card; secondary is 6.8:1).
    component Note: Text {
        Layout.fillWidth: true
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }
    // P1 #48: one folder row: what it is, where (the default when none is chosen) and the free space there, Change...
    // and Default.
    component FolderRow: RowLayout {
        id: folderRow
        property string key: ""            // "media" or "ai"
        property string label: ""
        property string chosen: ""         // "" = the default
        property string defaultFolder: ""
        property string freeText: ""
        Layout.fillWidth: true
        spacing: Theme.space(2)
        objectName: "folderRow_" + key

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            Label {
                text: folderRow.label
                color: Theme.textPrimary
                font.weight: Font.DemiBold
            }
            Note {
                objectName: "folderPath_" + folderRow.key
                text: folderRow.chosen !== "" ? folderRow.chosen : qsTr("%1 (default)").arg(folderRow.defaultFolder)
                elide: Text.ElideMiddle
                wrapMode: Text.NoWrap
            }
            Note {
                objectName: "folderFree_" + folderRow.key
                visible: folderRow.freeText !== ""
                text: folderRow.freeText
            }
        }
        PillButton {
            objectName: "folderChange_" + folderRow.key
            compact: true
            text: qsTr("Change…")
            tooltip: qsTr("Choose the folder for %1").arg(folderRow.label)
            onClicked: dialog.chooseFolder(folderRow.key, folderRow.chosen !== "" ? folderRow.chosen : folderRow.defaultFolder)
        }
        PillButton {
            objectName: "folderDefault_" + folderRow.key
            compact: true
            text: qsTr("Default")
            enabled: folderRow.chosen !== ""
            tooltip: qsTr("Go back to the default folder for %1").arg(folderRow.label)
            onClicked: dialog.setFolder(folderRow.key, "")
        }
    }

    // ---- Folder choice (P1 #48) ----
    property string folderTarget: ""   // "proxies", "media" or "ai"
    function chooseFolder(target: string, start: string) {
        dialog.folderTarget = target
        const forward = start.replace(/\\/g, "/")
        folderDialog.currentFolder = forward === "" ? "" : (forward.startsWith("/") ? "file://" : "file:///") + forward
        folderDialog.open()
    }
    function setFolder(target: string, folder: string) {
        if (target === "media")
            MediaReadiness.mediaCacheFolder = folder
        else if (target === "ai")
            MediaReadiness.aiCacheFolder = folder
        else if (target === "proxies") {
            MediaReadiness.proxyRootFolder = folder
            MediaReadiness.proxyLocation = folder !== "" ? "folder" : "project"
        }
    }
    FolderDialog {
        id: folderDialog
        objectName: "cacheFolderDialog"
        title: dialog.folderTarget === "proxies" ? qsTr("Folder for proxies")
             : dialog.folderTarget === "ai" ? qsTr("Folder for AI models and analysis results")
             : qsTr("Folder for the media cache")
        onAccepted: dialog.setFolder(dialog.folderTarget, selectedFolder.toString())
    }

    // ---- Clean confirmations (fix F2C) ----
    // What "Clean regenerable data" deletes: every row that can be rebuilt (not the render files, which it leaves, and
    // not the AI masks and chat logs), and of that the proxies.
    readonly property var regenerable: {
        let files = 0, bytes = 0, proxyFiles = 0, proxyBytes = 0
        const rows = MediaReadiness.caches
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            if (row.id === "renderFiles" || row.rebuildable !== true)
                continue
            files += row.cleanableFiles
            bytes += row.cleanableBytes
            if (row.proxies === true) {
                proxyFiles += row.cleanableFiles
                proxyBytes += row.cleanableBytes
            }
        }
        return { files: files, bytes: bytes, proxyFiles: proxyFiles, proxyBytes: proxyBytes }
    }
    function filesText(count: real): string {
        return count === 1 ? qsTr("1 file") : qsTr("%1 files").arg(count)
    }
    function proxiesText(count: real): string {
        return count === 1 ? qsTr("1 proxy") : qsTr("%1 proxies").arg(count)
    }
    // One row's Clean: proxies, analysis results and the AI masks ask first; thumbnails and waveforms (pictures made
    // again on sight) and the render files (as before) go at once.
    function askClean(row: var) {
        const id = row.id
        const size = row.cleanableText
        if (row.proxies === true) {
            cleanConfirm.ask(qsTr("Delete proxies?"),
                             qsTr("This deletes %1 that Wabcut made (%2). Other projects may still use these proxies; Wabcut makes them again when needed. The proxies this project uses and any file Wabcut did not make stay.")
                                 .arg(dialog.proxiesText(row.cleanableFiles)).arg(size),
                             qsTr("Delete proxies"), () => MediaReadiness.cleanCache(id))
        } else if (row.rebuildable !== true) {
            cleanConfirm.ask(qsTr("Delete AI masks and chat logs?"),
                             qsTr("This deletes %1 (%2) that cannot be rebuilt from the original media. AI object masks that use them, in this and other projects, fall back to their box until you track them again, and imported chat logs must be imported again.")
                                 .arg(dialog.filesText(row.cleanableFiles)).arg(size),
                             qsTr("Delete permanently"), () => MediaReadiness.cleanCache(id))
        } else if (id === "analysis") {
            cleanConfirm.ask(qsTr("Delete analysis results?"),
                             qsTr("This deletes %1 (%2): transcripts, shots, beats and the other results Wabcut made from your media, for every project. They are made again when needed, which takes time (a cloud transcription is paid again). AI masks and chat logs stay.")
                                 .arg(dialog.filesText(row.cleanableFiles)).arg(size),
                             qsTr("Delete"), () => MediaReadiness.cleanCache(id))
        } else {
            MediaReadiness.cleanCache(id)
        }
    }
    function askCleanAll() {
        const plan = dialog.regenerable
        const proxies = plan.proxyFiles > 0
                      ? " " + qsTr("That includes %1 (%2): other projects may still use them; Wabcut makes them again when needed.")
                                  .arg(dialog.proxiesText(plan.proxyFiles)).arg(MediaReadiness.sizeText(plan.proxyBytes))
                      : ""
        cleanConfirm.ask(qsTr("Clean regenerable data?"),
                         qsTr("This deletes %1 (%2) that Wabcut can make again from the original media: thumbnails, waveforms, analysis results and unused proxies.")
                             .arg(dialog.filesText(plan.files)).arg(MediaReadiness.sizeText(plan.bytes))
                         + proxies + " "
                         + qsTr("It keeps the files this project uses, AI masks and chat logs (they cannot be rebuilt), the render files and any file Wabcut did not make."),
                         qsTr("Clean"), () => MediaReadiness.cleanCache(""))
    }
    MenuConfirmDialog {
        id: cleanConfirm
        objectName: "cacheCleanConfirm"
    }

    // P1 #48: the sections scroll when the window is too short for them all (a small screen, 150 %).
    Flickable {
        id: scroller
        objectName: "mediaCacheScroller"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(sections.implicitHeight,
                                         dialog.parent ? dialog.parent.height - Theme.space(60) : sections.implicitHeight)
        contentHeight: sections.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        QQC.ScrollBar.vertical: ThinScrollBar {}

        ColumnLayout {
            id: sections
            width: scroller.width - Theme.space(3) // room for the scroll bar
            spacing: Theme.space(3)

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
                    Accessible.name: qsTr("Make proxies")
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
                    Accessible.name: qsTr("Proxy format")
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
                    Accessible.name: qsTr("Proxy size")
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
            // P1 #48: where new proxies go; the ones made before stay where they are.
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.space(4)
                rowSpacing: Theme.space(2)

                Label { text: qsTr("Save proxies") }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space(2)

                    PillTabs {
                        objectName: "proxyLocationTabs"
                        Layout.fillWidth: true
                        compact: true
                        model: [qsTr("Next to the project"), qsTr("In this folder")]
                        currentIndex: MediaReadiness.proxyLocation === "folder" ? 1 : 0
                        Accessible.name: qsTr("Save proxies")
                        onActivated: index => {
                            if (index === 1 && MediaReadiness.proxyRootFolder === "")
                                dialog.chooseFolder("proxies", "") // the folder first; Cancel keeps "Next to the project"
                            else
                                MediaReadiness.proxyLocation = index === 1 ? "folder" : "project"
                            currentIndex = Qt.binding(() => MediaReadiness.proxyLocation === "folder" ? 1 : 0)
                        }
                    }
                    PillButton {
                        objectName: "proxyFolderChoose"
                        compact: true
                        visible: MediaReadiness.proxyLocation === "folder"
                        text: qsTr("Choose…")
                        tooltip: qsTr("Choose the folder for proxies")
                        onClicked: dialog.chooseFolder("proxies", MediaReadiness.proxyRootFolder)
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Note {
                        objectName: "proxyFolderText"
                        text: qsTr("Saved in %1").arg(MediaReadiness.proxyFolder)
                        elide: Text.ElideMiddle
                        wrapMode: Text.NoWrap
                    }
                    Note {
                        objectName: "proxyFreeText"
                        visible: MediaReadiness.proxyFreeText !== ""
                        text: MediaReadiness.proxyFreeText
                    }
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
                    Accessible.name: qsTr("Preview quality")
                    model: [qsTr("Auto"), qsTr("Full"), "1/2", "1/4"]
                    currentIndex: values.indexOf(MediaReadiness.previewQuality)
                    onActivated: index => {
                        MediaReadiness.previewQuality = values[index]
                        currentIndex = Qt.binding(() => values.indexOf(MediaReadiness.previewQuality))
                    }
                }
                // P1 (#16): Premiere's Paused Resolution (the monitors' menus have it too).
                Label { text: qsTr("Paused") }
                PillTabs {
                    objectName: "pausedQualityTabs"
                    Layout.fillWidth: true
                    compact: true
                    readonly property var values: ["full", "half", "quarter", "same"]
                    model: [qsTr("Full"), "1/2", "1/4", qsTr("Same as playback")]
                    Accessible.name: qsTr("Paused resolution")
                    currentIndex: values.indexOf(MediaReadiness.pausedQuality)
                    onActivated: index => {
                        MediaReadiness.pausedQuality = values[index]
                        currentIndex = Qt.binding(() => values.indexOf(MediaReadiness.pausedQuality))
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

            // ---- Cache folders (P1 #48) ----
            SectionHeader {
                Layout.fillWidth: true
                text: qsTr("Cache folders")
            }
            FolderRow {
                key: "media"
                label: qsTr("Media cache (thumbnails and waveforms)")
                chosen: MediaReadiness.mediaCacheFolder
                defaultFolder: MediaReadiness.defaultMediaCacheFolder
                freeText: MediaReadiness.mediaCacheFreeText
            }
            FolderRow {
                key: "ai"
                label: qsTr("AI models and analysis results")
                chosen: MediaReadiness.aiCacheFolder
                defaultFolder: MediaReadiness.defaultAiCacheFolder
                freeText: MediaReadiness.aiCacheFreeText
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space(2)

                Note {
                    objectName: "cacheFoldersNote"
                    text: MediaReadiness.restartNeeded
                          ? qsTr("The new folders are used when Wabcut restarts. What is in the old ones stays there: clean it below before you restart.")
                          : qsTr("Used from the next start. A production's own media cache (Production Settings) still wins for its projects.")
                }
                PillButton {
                    objectName: "cacheFoldersRestart"
                    visible: MediaReadiness.restartNeeded
                    compact: true
                    variant: "primary"
                    text: qsTr("Restart now")
                    tooltip: qsTr("Close and reopen Wabcut with the new folders (it asks about unsaved changes first)")
                    onClicked: {
                        dialog.close()
                        AppShell.requestRestart()
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
                    readonly property bool renderFiles: modelData.id === "renderFiles"
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
                            text: qsTr("%1 in %2").arg(cacheRow.modelData.sizeText)
                                                  .arg(cacheRow.modelData.files === 1
                                                       ? qsTr("1 file")
                                                       : qsTr("%1 files").arg(cacheRow.modelData.files))
                                  + (cacheRow.modelData.inUseText !== "" ? " · " + cacheRow.modelData.inUseText : "")
                        }
                        // P1 #48: the drive's free space (the low-space warnings' lookup).
                        Note {
                            objectName: "cacheFree_" + cacheRow.modelData.id
                            visible: (cacheRow.modelData.freeText ?? "") !== ""
                            text: cacheRow.modelData.freeText ?? ""
                        }
                        // Fix F1A (ARGON-04): proxies and analysis results may still be used by a project, so no limit
                        // deletes them; only their Clean does.
                        Note {
                            objectName: "cacheNoLimit_" + cacheRow.modelData.id
                            visible: cacheRow.modelData.limited !== true && !cacheRow.renderFiles
                            // Fix F2C: what cannot be rebuilt says so (and is left by Clean regenerable data).
                            text: cacheRow.modelData.rebuildable === false
                                  ? qsTr("Cannot be rebuilt from the media: never deleted automatically or by Clean regenerable data")
                                  : qsTr("Never deleted automatically: projects may still use them")
                        }
                    }
                    PillButton {
                        id: limitButton
                        objectName: "cacheLimit_" + cacheRow.modelData.id
                        compact: true
                        // Only caches no project refers to have a limit (thumbnails, waveforms; fix F1A); render files
                        // are never cut by a limit (P1 #58).
                        visible: cacheRow.modelData.limited === true
                        text: qsTr("Limit: %1").arg(cacheRow.modelData.limitText)
                        tooltip: qsTr("With the switch below on, the oldest files beyond the limit are deleted; they are made again when needed")
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
                        objectName: "cacheClean_" + cacheRow.modelData.id
                        compact: true
                        text: qsTr("Clean")
                        enabled: cacheRow.modelData.cleanableBytes > 0 && !MediaReadiness.cacheBusy
                                 && !(cacheRow.renderFiles && MediaReadiness.rendering)
                        tooltip: cacheRow.renderFiles
                                 ? qsTr("Delete every render file of this project: its render bars turn red or yellow again, and you can render them again")
                                 : cacheRow.modelData.rebuildable === false
                                   ? qsTr("Delete them for good, after you confirm: they cannot be rebuilt")
                                   : cacheRow.modelData.proxies === true
                                     ? qsTr("Delete the proxies Wabcut made that this project does not use, after you confirm")
                                     : qsTr("Delete what can be rebuilt from the originals")
                        disabledReason: cacheRow.renderFiles && MediaReadiness.rendering ? qsTr("Wait for the render to finish") : ""
                        onClicked: dialog.askClean(cacheRow.modelData)
                    }
                }
            }
            Note {
                objectName: "cacheTotalText"
                text: MediaReadiness.cacheBusy ? qsTr("Measuring…") : qsTr("%1 in total. Clean deletes only files Wabcut made; the files this project uses are never deleted, and Clean regenerable data keeps the AI masks and chat logs, which cannot be rebuilt.").arg(MediaReadiness.cacheTotalText)
            }
            // P1 #58: the limits without opening this dialog (after start and after each batch of proxies).
            Toggle {
                objectName: "autoEnforceToggle"
                text: qsTr("Keep the caches within their limits automatically")
                tooltip: qsTr("Shortly after Wabcut starts and after each batch of proxies, the oldest thumbnails and waveforms beyond their limits are deleted. It waits while you play, render or export. Proxies and analysis results are never deleted automatically.")
                checked: MediaReadiness.autoEnforce
                onToggled: MediaReadiness.autoEnforce = checked
            }
        }
    }

    footerData: [
        PillButton {
            objectName: "cleanAllButton"
            text: qsTr("Clean regenerable data")
            enabled: !MediaReadiness.cacheBusy && dialog.regenerable.bytes > 0
            tooltip: qsTr("Delete what Wabcut can make again from the original media, after you confirm")
            onClicked: dialog.askCleanAll()
        },
        PillButton {
            objectName: "mediaCacheDone"
            variant: "primary"
            text: qsTr("Done")
            onClicked: dialog.close()
        }
    ]
}
