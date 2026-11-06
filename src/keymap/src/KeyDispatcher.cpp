#include <wabcut/keymap/KeyDispatcher.h>

#include <utility>

namespace wabcut::keymap {
namespace {

std::chrono::milliseconds steadyNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
}

void append(std::vector<TransportIntent>& to, const std::vector<TransportIntent>& from) {
    to.insert(to.end(), from.begin(), from.end());
}

} // namespace

KeyDispatcher::KeyDispatcher(const KeymapManager& keymap, Clock clock,
                             std::chrono::milliseconds holdThreshold)
    : m_keymap(&keymap), m_clock(clock ? std::move(clock) : Clock(steadyNow)), m_gesture(holdThreshold),
      m_holdThreshold(holdThreshold) {}

KeyResult KeyDispatcher::keyPress(Scope focused, KeyChord chord, bool autoRepeat) {
    KeyResult out;
    const KeyChord normalized = chord.normalized();
    if (!normalized.isValid()) {
        return out;
    }
    const CommandInfo* info = nullptr;
    const auto held = m_held.find(normalized.key);
    if (autoRepeat && held != m_held.end()) {
        info = held->second.info; // a repeat keeps running what the original press resolved to
        held->second.repeated = true;
    } else {
        // A fresh press of a key we think is down means its release was lost: release it first.
        if (held != m_held.end() && held->second.info != nullptr) {
            if (const auto role = TransportGesture::roleOf(held->second.info->id)) {
                append(out.transport, m_gesture.release(*role));
            }
            if (held->second.info->momentary && held->second.repeated) {
                out.released.emplace_back(held->second.info->id); // its hold ended without a release we saw
            }
        }
        info = m_keymap->resolveCommand(focused, chord);
        m_held[normalized.key] = Held{info, m_clock(), false};
    }
    if (info == nullptr) {
        return out;
    }
    out.handled = true;
    if (const auto role = TransportGesture::roleOf(info->id)) {
        append(out.transport, m_gesture.press(*role, autoRepeat, m_clock()));
    } else if (!autoRepeat || info->autoRepeat) {
        out.commands.emplace_back(info->id);
    }
    return out;
}

KeyResult KeyDispatcher::keyRelease(KeyChord chord, bool autoRepeat) {
    KeyResult out;
    const auto held = m_held.find(chord.normalized().key);
    if (held == m_held.end()) {
        return out;
    }
    out.handled = held->second.info != nullptr;
    if (autoRepeat) {
        return out; // X11/Windows deliver synthetic release+press pairs while a key repeats
    }
    const Held down = held->second;
    m_held.erase(held);
    if (down.info != nullptr) {
        if (const auto role = TransportGesture::roleOf(down.info->id)) {
            out.transport = m_gesture.release(*role);
        }
        // A momentary command (H / Z): a long press was a hold, so the app hands the replaced state back. A quick tap
        // leaves the command's effect in place (Photoshop's tool keys).
        if (down.info->momentary && (down.repeated || m_clock() - down.since >= m_holdThreshold)) {
            out.released.emplace_back(down.info->id);
        }
    }
    return out;
}

KeyResult KeyDispatcher::trigger(std::string_view command) {
    KeyResult out;
    const CommandInfo* info = m_keymap->registry().find(command);
    if (info == nullptr) {
        return out;
    }
    out.handled = true;
    if (const auto role = TransportGesture::roleOf(info->id)) {
        append(out.transport, m_gesture.press(*role, false, m_clock()));
        append(out.transport, m_gesture.release(*role));
    } else {
        out.commands.emplace_back(info->id);
    }
    return out;
}

std::vector<TransportIntent> KeyDispatcher::tick() {
    return m_gesture.tick(m_clock());
}

std::vector<CommandId> KeyDispatcher::heldMomentary() const {
    std::vector<CommandId> out;
    for (const auto& [key, down] : m_held) {
        if (down.info != nullptr && down.info->momentary && (down.repeated || m_clock() - down.since >= m_holdThreshold)) {
            out.emplace_back(down.info->id);
        }
    }
    return out;
}

std::vector<TransportIntent> KeyDispatcher::focusLost() {
    m_held.clear();
    return m_gesture.reset();
}

} // namespace wabcut::keymap
