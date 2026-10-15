#pragma once

// wabcut-plughost: the top-level window a plugin's editor (its own GUI) is attached to. The helper owns the window,
// so a plugin GUI lives and dies with the helper process, never inside the editor's process; a separate top-level
// window is what both CLAP (embedded into a parent HWND) and VST3 (IPlugView::attached to an HWND) support on Windows
// without reparenting across processes.

#include <functional>
#include <memory>
#include <string>

namespace wabcut::plughost {

class EditorWindow {
public:
    struct Callbacks {
        std::function<void()> closed;          ///< the user closed the window (the owner detaches and destroys it)
        std::function<void(int, int)> resized; ///< new client size, when the user resized it
    };

    static std::unique_ptr<EditorWindow> create(const std::string& title, int width, int height, bool resizable,
                                                Callbacks callbacks, std::string& error);
    ~EditorWindow();
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    void* hwnd() const noexcept { return m_hwnd; }
    /// Sets the client area's size (the plugin asked to resize).
    void setClientSize(int width, int height);
    void show();

    // Window procedure hooks (Editor.cpp).
    void closeRequested();
    void userResized(int width, int height);

private:
    EditorWindow() = default;

    void* m_hwnd = nullptr;
    Callbacks m_callbacks;
    bool m_settingSize = false;
};

} // namespace wabcut::plughost
