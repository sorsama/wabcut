pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Import Layered File (0.2; Premiere's dialog for Photoshop files): Import As Merge All Layers, Merged Layers (the layers
// ticked, merged), Individual Layers (one clip per layer, in a bin named after the file) or Sequence (those clips on
// their own tracks of a new sequence of the document's size); Footage Dimensions Document Size (each layer where it sits
// on the document's canvas) or Layer Size (cropped to its pixels). Hidden layers start unticked. One undo step
// (Library.importLayeredFile -> media.importLayeredFile). Files dropped or imported together open the dialog in turn.
WabcutDialog {
    id: dialog
    objectName: "importLayeredFileDialog"

    property var queue: []
    property url file: ""
    property var info: ({})
    property string importAs: "mergeAll"
    property string footage: "document"
    property var ticked: ({}) // layer index -> true
    property string errorText: ""
    readonly property var modes: [
        { id: "mergeAll", name: qsTr("Merge All Layers") }, { id: "merged", name: qsTr("Merged Layers") },
        { id: "layers", name: qsTr("Individual Layers") }, { id: "sequence", name: qsTr("Sequence") }
    ]
    readonly property bool picksLayers: dialog.importAs !== "mergeAll"
    readonly property var pixelLayers: (dialog.info.layers ?? []).filter(l => l.hasPixels && !l.group)

    /// Opens the dialog for each of `urls` in turn.
    function openFor(urls) {
        dialog.queue = dialog.queue.concat(urls)
        if (!dialog.visible)
            dialog.next()
    }
    function next() {
        while (dialog.queue.length > 0) {
            const url = dialog.queue[0]
            dialog.queue = dialog.queue.slice(1)
            const read = Library.layeredFileInfo(url)
            if (read.layers === undefined) {
                Library.importLayeredFile(url, { as: "mergeAll" }) // reports why through Library.message
                continue
            }
            dialog.file = url
            dialog.info = read
            dialog.importAs = "mergeAll"
            dialog.footage = "document"
            const chosen = {}
            for (const l of read.layers)
                if (l.visible && l.hasPixels && !l.group)
                    chosen[l.index] = true
            dialog.ticked = chosen
            dialog.errorText = ""
            open()
            return
        }
    }
    function apply() {
        const options = { as: dialog.importAs, footage: dialog.footage }
        if (dialog.picksLayers)
            options.layers = Object.keys(dialog.ticked).filter(k => dialog.ticked[k]).map(k => Number(k))
        if (dialog.picksLayers && options.layers.length === 0) {
            dialog.errorText = qsTr("Tick at least one layer")
            return
        }
        if (Library.importLayeredFile(dialog.file, options) === "") {
            dialog.errorText = Library.message !== "" ? Library.message : qsTr("The file could not be imported")
            return
        }
        dialog.close() // onClosed opens the next file's
    }

    title: qsTr("Import Layered File: %1").arg(decodeURIComponent(dialog.file.toString().split("/").pop()))
    width: Math.min(parent ? parent.width - Theme.space(16) : Theme.space(160), Theme.space(160))
    onClosed: Qt.callLater(dialog.next)

    Text {
        Layout.fillWidth: true
        text: qsTr("Import As")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    PillTabs {
        objectName: "layeredImportAs"
        Layout.fillWidth: true
        compact: true
        model: dialog.modes.map(m => m.name)
        currentIndex: dialog.modes.findIndex(m => m.id === dialog.importAs)
        onActivated: index => dialog.importAs = dialog.modes[index].id
    }
    Text {
        Layout.fillWidth: true
        text: qsTr("Footage Dimensions")
        color: Theme.textSecondary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        visible: dialog.importAs === "layers" || dialog.importAs === "sequence"
    }
    PillTabs {
        objectName: "layeredFootage"
        Layout.fillWidth: true
        compact: true
        visible: dialog.importAs === "layers" || dialog.importAs === "sequence"
        model: [qsTr("Document Size"), qsTr("Layer Size")]
        currentIndex: dialog.footage === "layer" ? 1 : 0
        onActivated: index => dialog.footage = index === 1 ? "layer" : "document"
    }
    Text {
        objectName: "layeredSummary"
        Layout.fillWidth: true
        // CMYK documents are converted to RGB; 32-bit ones are linear light, shown with the sRGB curve.
        text: qsTr("%1 x %2, %3-bit · %n layer(s)", "", dialog.pixelLayers.length).arg(dialog.info.width ?? 0)
                  .arg(dialog.info.height ?? 0).arg(dialog.info.depth ?? 8)
              + (dialog.info.colorMode === "cmyk" ? qsTr(" · CMYK, converted to RGB")
                 : dialog.info.depth === 32 ? qsTr(" · 32-bit float, linear") : "")
        color: Theme.textTertiary
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
    }
    ListView {
        id: layerList
        objectName: "layeredLayers"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, Theme.space(64))
        visible: dialog.picksLayers
        clip: true
        model: dialog.pixelLayers.slice().reverse() // top layer first, as Photoshop lists them
        ScrollBar.vertical: ScrollBar {}
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Layers")
        delegate: Toggle {
            required property var modelData
            width: ListView.view.width
            text: (modelData.groups.length > 0 ? modelData.groups.join(" / ") + " / " : "") + modelData.name +
                  (modelData.visible ? "" : qsTr(" (hidden)"))
            checked: dialog.ticked[modelData.index] === true
            onToggled: {
                const chosen = Object.assign({}, dialog.ticked)
                chosen[modelData.index] = checked
                dialog.ticked = chosen
            }
        }
    }
    Text {
        objectName: "layeredError"
        Layout.fillWidth: true
        visible: text !== ""
        text: dialog.errorText
        color: Theme.danger
        font.family: Theme.fontSans
        font.pixelSize: Theme.textSm
        wrapMode: Text.Wrap
    }

    footerData: [
        PillButton {
            variant: "ghost"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        },
        PillButton {
            objectName: "layeredOk"
            variant: "primary"
            text: qsTr("OK")
            onClicked: dialog.apply()
        }
    ]
}
