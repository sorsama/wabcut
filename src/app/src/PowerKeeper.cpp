#include "PowerKeeper.h"

#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wabcut::app {

namespace {

/// Keepers active now, across instances (GUI thread only).
int g_holds = 0;

std::function<void(bool)>& testBackend() {
    static std::function<void(bool)> backend;
    return backend;
}

/// The platform's hold: one lazily made power request, set while held. Elsewhere nothing.
void platformHold(bool hold) {
#ifdef _WIN32
    static HANDLE request = nullptr;
    if (!request) {
        if (!hold)
            return;
        static wchar_t reason[] = L"Wabcut is exporting or processing media";
        REASON_CONTEXT context{};
        context.Version = POWER_REQUEST_CONTEXT_VERSION;
        context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        context.Reason.SimpleReasonString = reason;
        const HANDLE made = PowerCreateRequest(&context);
        if (made == INVALID_HANDLE_VALUE || made == nullptr)
            return; // nothing to hold with: the computer sleeps as usual
        request = made;
    }
    if (hold)
        PowerSetRequest(request, PowerRequestSystemRequired);
    else
        PowerClearRequest(request, PowerRequestSystemRequired);
#else
    (void)hold;
#endif
}

void transition(bool hold) {
    if (testBackend())
        testBackend()(hold);
    else
        platformHold(hold);
}

void acquire() {
    if (g_holds++ == 0)
        transition(true);
}

void release() {
    if (g_holds > 0 && --g_holds == 0)
        transition(false);
}

} // namespace

PowerKeeper::PowerKeeper(QObject* parent) : QObject(parent) {}

PowerKeeper::~PowerKeeper() {
    if (m_active)
        release();
}

void PowerKeeper::setActive(bool on) {
    if (on == m_active)
        return;
    m_active = on;
    if (on)
        acquire();
    else
        release();
    emit activeChanged();
}

int PowerKeeper::holds() {
    return g_holds;
}

void PowerKeeper::setBackendForTests(std::function<void(bool hold)> backend) {
    testBackend() = std::move(backend);
}

} // namespace wabcut::app
