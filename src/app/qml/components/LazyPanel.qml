import QtQuick

// A panel created when it is first shown, or when the window preloads the panels after its first frame (M5n startup:
// the window shows the active workspace first), then kept alive: switching back to it is instant and keeps its state.
// Created asynchronously (incubated between frames), so creating one never stalls a frame. An engine that cannot create
// in the background (no incubation controller: a plain QQmlEngine, as in the tests) creates it synchronously, and
// Main.qml then preloads every panel up front, as before M5n. The panel fills the loader; the host puts opacity,
// visible and enabled on the LazyPanel as it did on the panel.
//
//   LazyPanel {
//       name: "essential"
//       shown: window.browserPanel === "essential"
//       preload: window.preloadPanels
//       sourceComponent: EssentialSoundPanel {}
//   }
Loader {
    id: root

    /// For the startup timings ("panel <name>" in UiProfile) and the objectName ("lazyPanel_<name>").
    property string name: ""
    /// The panel is shown (or about to be): create it now.
    property bool shown: false
    /// The window has shown its first frame: create it in the background.
    property bool preload: false
    /// Set once: a created panel stays.
    property bool wanted: false
    readonly property bool ready: status === Loader.Ready
    property real startedAt: 0
    property var pending: []

    /// Calls f(panel) now when the panel exists, else creates it and calls f once it is loaded.
    function withItem(f) {
        if (root.item) {
            f(root.item)
            return
        }
        root.pending.push(f)
        root.wanted = true
    }

    objectName: "lazyPanel_" + name
    asynchronous: UiProfile.backgroundLoading()
    active: wanted
    // Plain bindings would let a panel go again when it is hidden: latch them.
    onShownChanged: {
        if (shown)
            wanted = true
    }
    onPreloadChanged: {
        if (preload)
            wanted = true
    }
    Component.onCompleted: {
        if (shown || preload)
            wanted = true
    }
    onWantedChanged: {
        if (wanted)
            startedAt = UiProfile.now()
    }
    onLoaded: {
        UiProfile.span("panel " + (root.name !== "" ? root.name : "?"), UiProfile.now() - root.startedAt)
        const calls = root.pending
        root.pending = []
        for (const f of calls)
            f(root.item)
    }
}
