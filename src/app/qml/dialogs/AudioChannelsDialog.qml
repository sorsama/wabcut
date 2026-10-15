pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as QC // qualified: unqualified, its ContextMenu would hide the app's

// Modify > Audio Channels… (A.9, M5n; bridge `AudioChannels`), Premiere's layout: the clip format (mono, stereo, 5.1,
// adaptive with its channel count), then a grid of clip channels (rows: L / R, the six 5.1 channels, ...) by the
// source's channels (columns), a cell per source that feeds the clip channel (several are averaged) and a mute per
// clip channel. The presets fill the grid (Fill Left with Right, Fill Right with Left, Swap, Sum to mono, Default);
// Split to Mono is an edit of its own. "Measure levels" reads each source channel's level (audio.channelLevels) and
// marks the silent ones. OK is one undo step: audio.setChannels on clips (each keeps its channels' volumes) or
// audio.setMediaChannels on Library items ("media").
WabcutDialog {
    id: dialog
    objectName: "audioChannelsDialog"

    /// "clips" | "media"
    property string mode: "clips"
    property var keys: []
    /// AudioChannels.info(keys) / mediaInfo(keys).
    property var info: ({})
    property string format: "stereo"
    /// Adaptive: the clip's channel count.
    property int adaptiveCount: 2
    /// One per clip channel: { sources: [1-based], muted }.
    property var routes: []
    /// Default chosen: OK clears the map (the clip plays its item's map, or the stream as it is).
    property bool useDefault: false
    property string error: ""

    readonly property int sourceCount: info.sourceChannels ?? 2
    readonly property var sourceLabels: info.sourceLabels ?? []
    readonly property var formats: ["mono", "stereo", "5.1", "adaptive"]
    readonly property int clipCount: format === "mono" ? 1 : format === "stereo" ? 2 : format === "5.1" ? 6 : adaptiveCount
    readonly property var clipLabels: AudioChannels.channelLabels(format, clipCount)
    /// The measured levels of the first clip: { channel -> { peakDb, rmsDb } } and its silent channels.
    readonly property var measured: {
        const rows = AudioChannels.levels.clips ?? []
        const first = rows.find(r => dialog.keys.indexOf(r.clip) >= 0)
        return first ?? null
    }

    function openFor(which, itemKeys) {
        mode = which === "media" ? "media" : "clips"
        keys = itemKeys
        error = ""
        useDefault = false
        info = mode === "media" ? AudioChannels.mediaInfo(keys) : AudioChannels.info(keys)
        format = info.format ?? "stereo"
        routes = (info.channels ?? []).map(c => ({ sources: (c.sources ?? []).slice(), muted: c.muted === true }))
        adaptiveCount = Math.max(1, routes.length)
        if (info.ok !== true)
            error = info.error ?? ""
        formatTabs.currentIndex = Math.max(0, formats.indexOf(format))
        open()
    }

    // ---- Editing the grid --------------------------------------------------------------------------------------
    function defaultRoute(i) {
        return { sources: i < dialog.sourceCount ? [i + 1] : [], muted: false }
    }
    function resize(count) {
        const next = []
        for (let i = 0; i < count; ++i)
            next.push(i < routes.length ? routes[i] : defaultRoute(i))
        routes = next
    }
    function setFormat(f) {
        format = f
        useDefault = false
        resize(clipCount)
    }
    function setAdaptiveCount(n) {
        adaptiveCount = Math.max(1, Math.min(32, Math.round(n)))
        if (format === "adaptive")
            resize(adaptiveCount)
    }
    function fed(row, source) {
        return row < routes.length && routes[row].sources.indexOf(source) >= 0
    }
    function toggleSource(row, source) {
        const next = routes.map(r => ({ sources: r.sources.slice(), muted: r.muted }))
        const sources = next[row].sources
        const at = sources.indexOf(source)
        if (at >= 0)
            sources.splice(at, 1)
        else if (sources.length < 8)
            sources.push(source)
        sources.sort((a, b) => a - b)
        routes = next
        useDefault = false
    }
    function toggleMute(row) {
        const next = routes.map(r => ({ sources: r.sources.slice(), muted: r.muted }))
        next[row].muted = !next[row].muted
        routes = next
        useDefault = false
    }
    function preset(name) {
        useDefault = false
        if (name === "default") {
            format = sourceCount === 1 ? "mono" : sourceCount === 2 ? "stereo" : sourceCount === 6 ? "5.1" : "adaptive"
            adaptiveCount = sourceCount
            routes = []
            resize(clipCount)
            useDefault = true
        } else if (name === "fillLeft" || name === "fillRight" || name === "swap") {
            format = "stereo"
            routes = name === "fillLeft" ? [{ sources: [2], muted: false }, { sources: [2], muted: false }]
                   : name === "fillRight" ? [{ sources: [1], muted: false }, { sources: [1], muted: false }]
                   : [{ sources: [2], muted: false }, { sources: [1], muted: false }]
        } else if (name === "sumToMono") {
            format = "mono"
            const all = []
            for (let s = 1; s <= Math.min(8, sourceCount); ++s)
                all.push(s)
            routes = [{ sources: all, muted: false }]
        }
        formatTabs.currentIndex = Math.max(0, formats.indexOf(format))
    }
    function accept() {
        const map = { format: format, channels: routes }
        const failure = useDefault
            ? (mode === "media" ? AudioChannels.applyMediaPreset(keys, "default") : AudioChannels.applyPreset(keys, "default"))
            : (mode === "media" ? AudioChannels.applyMediaMap(keys, map) : AudioChannels.applyMap(keys, map))
        if (failure === "")
            dialog.close()
        else
            dialog.error = failure
    }
    function levelText(source) {
        if (!measured)
            return ""
        const level = (measured.channels ?? []).find(c => c.channel === source)
        if (!level)
            return ""
        return level.rmsDb <= -120 ? qsTr("silent") : qsTr("%1 dB").arg(Number(level.rmsDb).toFixed(0))
    }
    function silent(source) {
        return measured !== null && (measured.silent ?? []).indexOf(source) >= 0
    }

    title: qsTr("Audio Channels")
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(150), Theme.space(150))

    // A cell of the grid: whether a source channel feeds a clip channel, or a clip channel's mute.
    component Cell: QC.AbstractButton {
        id: cell
        property bool on: false
        property color onColor: Theme.accentFill
        property string tip: ""
        implicitWidth: Theme.space(9)
        implicitHeight: Theme.controlHeightSmall
        focusPolicy: Qt.StrongFocus
        hoverEnabled: true
        Accessible.role: Accessible.CheckBox
        Accessible.checkable: true
        Accessible.checked: cell.on
        Accessible.name: cell.tip
        background: Rectangle {
            radius: Theme.radiusBadge
            color: cell.on ? cell.onColor : cell.hovered ? Theme.hover : Theme.sunken
            border.width: Theme.hairline
            border.color: cell.on ? cell.onColor : Theme.border
            opacity: cell.enabled ? 1 : Theme.disabledOpacity
            Behavior on color { ColorAnimation { duration: Theme.durFast } }
            FocusRing {
                targetRadius: Theme.radiusBadge
                visible: cell.visualFocus
            }
        }
        contentItem: Item {
            Icon {
                anchors.centerIn: parent
                visible: cell.on
                name: "check"
                size: Theme.iconSm
                color: Theme.accentOnFill
            }
        }
        HoverHandler {
            enabled: cell.enabled
            cursorShape: Qt.PointingHandCursor
        }
        Loader {
            active: cell.hovered && cell.tip !== ""
            sourceComponent: Tooltip {
                parent: cell
                visible: true
                text: cell.tip
            }
        }
    }
    component GridLabel: Text {
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    Text {
        objectName: "audioChannelsSummary"
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        text: {
            if (dialog.info.ok !== true)
                return ""
            const what = dialog.mode === "media"
                ? (dialog.keys.length === 1 ? qsTr("The media item") : qsTr("%1 media items").arg(dialog.keys.length))
                : (dialog.keys.length === 1 ? qsTr("The selected clip") : qsTr("%1 selected clips").arg(dialog.keys.length))
            const from = dialog.info.mapFrom === "clip" ? qsTr("its own channel map")
                       : dialog.info.mapFrom === "media" ? qsTr("its media item's channel map")
                       : qsTr("the source as it is")
            return qsTr("%1 · source: %2 channel(s) · plays %3").arg(what).arg(dialog.sourceCount).arg(from)
        }
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textMd
    }

    // ---- Clip format ----
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space(2)
        GridLabel { text: qsTr("Clip format") }
        PillTabs {
            id: formatTabs
            objectName: "audioChannelsFormat"
            Layout.preferredWidth: Theme.space(70)
            compact: true
            model: [qsTr("Mono"), qsTr("Stereo"), qsTr("5.1"), qsTr("Adaptive")]
            Accessible.name: qsTr("Clip format")
            onActivated: index => dialog.setFormat(dialog.formats[index])
        }
        NumberField {
            objectName: "audioChannelsCount"
            visible: dialog.format === "adaptive"
            Layout.preferredWidth: Theme.numberFieldWidth
            from: 1
            to: 32
            value: dialog.adaptiveCount
            suffix: qsTr("ch")
            Accessible.name: qsTr("Channels of the adaptive clip")
            onValueModified: v => dialog.setAdaptiveCount(v)
        }
        Item { Layout.fillWidth: true }
    }

    // ---- Presets ----
    Flow {
        Layout.fillWidth: true
        spacing: Theme.space(1)
        PillButton {
            objectName: "presetFillLeft"
            compact: true
            enabled: dialog.sourceCount >= 2
            text: qsTr("Fill Left with Right")
            onClicked: dialog.preset("fillLeft")
        }
        PillButton {
            objectName: "presetFillRight"
            compact: true
            enabled: dialog.sourceCount >= 2
            text: qsTr("Fill Right with Left")
            onClicked: dialog.preset("fillRight")
        }
        PillButton {
            objectName: "presetSwap"
            compact: true
            enabled: dialog.sourceCount >= 2
            text: qsTr("Swap Channels")
            onClicked: dialog.preset("swap")
        }
        PillButton {
            objectName: "presetSum"
            compact: true
            text: qsTr("Sum to mono")
            onClicked: dialog.preset("sumToMono")
        }
        PillButton {
            objectName: "presetDefault"
            compact: true
            variant: dialog.useDefault ? "primary" : "secondary"
            text: qsTr("Default")
            tooltip: qsTr("No channel map: the clip plays the source as it is")
            onClicked: dialog.preset("default")
        }
        PillButton {
            objectName: "presetSplit"
            compact: true
            visible: dialog.mode === "clips" && dialog.keys.length === 1
            enabled: dialog.sourceCount >= 2
            iconName: "fold-vertical"
            text: qsTr("Split to Mono")
            tooltip: qsTr("One linked mono clip per source channel, on the free audio tracks below")
            onClicked: {
                const failure = AudioChannels.splitToMono(dialog.keys[0])
                if (failure === "")
                    dialog.close()
                else
                    dialog.error = failure
            }
        }
    }

    // ---- The grid: clip channels by source channels ----
    Rectangle {
        objectName: "audioChannelsGrid"
        Layout.fillWidth: true
        implicitHeight: grid.implicitHeight + Theme.space(4)
        radius: Theme.radiusCard
        color: Theme.sunken
        border.width: Theme.hairline
        border.color: Theme.border

        QC.ScrollView {
            anchors.fill: parent
            anchors.margins: Theme.space(2)
            contentWidth: grid.implicitWidth

            GridLayout {
                id: grid
                columns: dialog.sourceCount + 2
                columnSpacing: Theme.space(1)
                rowSpacing: Theme.space(1)

                // Header: the source channels, their measured level, Mute.
                GridLabel {
                    text: qsTr("Clip channel")
                    font.weight: Font.DemiBold
                }
                Repeater {
                    model: dialog.sourceCount
                    delegate: ColumnLayout {
                        id: head
                        required property int index
                        readonly property int source: index + 1
                        spacing: 0
                        Layout.alignment: Qt.AlignHCenter
                        GridLabel {
                            Layout.alignment: Qt.AlignHCenter
                            text: head.index < dialog.sourceLabels.length ? dialog.sourceLabels[head.index] : head.source
                            color: dialog.silent(head.source) ? Theme.warning : Theme.textPrimary
                            font.family: Theme.fontMono
                            font.weight: Font.DemiBold
                        }
                        GridLabel {
                            objectName: "sourceLevel" + head.source
                            Layout.alignment: Qt.AlignHCenter
                            visible: text !== ""
                            text: dialog.levelText(head.source)
                            color: dialog.silent(head.source) ? Theme.warning : Theme.textTertiary
                            font.pixelSize: Theme.textXs
                        }
                    }
                }
                GridLabel {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Mute")
                    font.weight: Font.DemiBold
                }

                // One row per clip channel.
                Repeater {
                    model: dialog.routes.length * (dialog.sourceCount + 2)
                    delegate: Item {
                        id: slot
                        required property int index
                        readonly property int columns: dialog.sourceCount + 2
                        readonly property int row: Math.floor(index / columns)
                        readonly property int column: index % columns
                        readonly property string channelLabel: row < dialog.clipLabels.length ? dialog.clipLabels[row] : String(row + 1)
                        implicitWidth: column === 0 ? rowLabel.implicitWidth : Theme.space(9)
                        implicitHeight: Theme.controlHeightSmall
                        Layout.alignment: Qt.AlignHCenter

                        GridLabel {
                            id: rowLabel
                            anchors.verticalCenter: parent.verticalCenter
                            visible: slot.column === 0
                            text: slot.channelLabel
                            color: Theme.textPrimary
                            font.family: Theme.fontMono
                        }
                        Cell {
                            objectName: "routeCell_" + (slot.row + 1) + "_" + slot.column
                            anchors.centerIn: parent
                            visible: slot.column > 0 && slot.column <= dialog.sourceCount
                            on: dialog.fed(slot.row, slot.column)
                            tip: qsTr("Source channel %1 into %2").arg(slot.column).arg(slot.channelLabel)
                            onClicked: dialog.toggleSource(slot.row, slot.column)
                        }
                        Cell {
                            objectName: "muteCell_" + (slot.row + 1)
                            anchors.centerIn: parent
                            visible: slot.column === dialog.sourceCount + 1
                            onColor: Theme.danger
                            on: slot.row < dialog.routes.length && dialog.routes[slot.row].muted
                            tip: qsTr("Mute %1").arg(slot.channelLabel)
                            onClicked: dialog.toggleMute(slot.row)
                        }
                    }
                }
            }
        }
    }

    // ---- Levels ----
    RowLayout {
        Layout.fillWidth: true
        visible: dialog.mode === "clips"
        spacing: Theme.space(2)
        PillButton {
            objectName: "audioChannelsMeasure"
            compact: true
            variant: "ghost"
            iconName: "gauge"
            enabled: !AudioChannels.busy && dialog.info.ok === true
            text: AudioChannels.busy ? qsTr("Measuring…") : qsTr("Measure levels")
            tooltip: qsTr("Each source channel's level (up to two minutes of the clip): finds a silent microphone")
            onClicked: AudioChannels.measure(dialog.keys.slice(0, 1))
        }
        Text {
            objectName: "audioChannelsSilent"
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.Wrap
            text: {
                const silent = dialog.measured ? (dialog.measured.silent ?? []) : []
                if (silent.length === 0)
                    return dialog.measured ? qsTr("Every channel has sound") : ""
                return qsTr("Silent: channel %1. Fill from the other side to hear it on both.").arg(silent.join(", "))
            }
            color: (dialog.measured && (dialog.measured.silent ?? []).length > 0) ? Theme.warning : Theme.textTertiary
            font.family: Theme.fontSans
            font.pixelSize: Theme.textSm
        }
    }

    Text {
        objectName: "audioChannelsError"
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.Wrap
        text: dialog.error
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }

    footerData: [
        PillButton {
            objectName: "audioChannelsCancel"
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "audioChannelsOk"
            variant: "primary"
            enabled: dialog.info.ok === true && dialog.routes.length > 0
            text: qsTr("OK")
            onClicked: dialog.accept()
        }
    ]
}
