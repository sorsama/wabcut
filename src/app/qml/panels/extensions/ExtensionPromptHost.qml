pragma ComponentBehavior: Bound
import QtQuick

// The open extension forms (C.12 ui.prompt, M5n): one non-modal ExtensionPromptDialog per entry of Extensions.prompts,
// created when a form opens and kept while it is open (a form opening or closing elsewhere does not recreate the others,
// so what the user typed stays). Each new one is offset a little so stacked forms stay visible. Main has one, filling
// the window.
Item {
    id: host
    objectName: "extensionPromptHost"

    /// The open forms (ExtensionPromptDialog), oldest first.
    readonly property int count: forms.count
    function dialogAt(index) {
        return forms.objectAt(index)
    }

    ListModel {
        id: openForms
    }

    /// Adds the new forms and drops the closed ones (by key).
    function sync() {
        const keys = Extensions.prompts.map(p => p.key)
        for (let i = openForms.count - 1; i >= 0; --i) {
            if (!keys.includes(openForms.get(i).key))
                openForms.remove(i)
        }
        for (const key of keys) {
            let known = false
            for (let i = 0; i < openForms.count; ++i)
                known = known || openForms.get(i).key === key
            if (!known)
                openForms.append({ key: key })
        }
    }

    Connections {
        target: Extensions
        function onPromptsChanged() {
            host.sync()
        }
    }
    Component.onCompleted: sync()

    Instantiator {
        id: forms
        model: openForms
        delegate: ExtensionPromptDialog {
            required property string key
            required property int index
            parent: host
            anchors.centerIn: undefined
            x: Math.max(0, (host.width - width) / 2 + index * Theme.space(6))
            y: Math.max(0, (host.height - height) / 2 + index * Theme.space(6))
            prompt: Extensions.prompts.find(p => p.key === key) ?? ({})
            Component.onCompleted: open()
        }
    }
}
