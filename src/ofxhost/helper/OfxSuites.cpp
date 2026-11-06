// wabcut-ofxhost: the OpenFX suites the host offers plug-ins (0.7, docs/plugins.md), the property bag behind every
// property set, and the host's description of itself.
//
// The suites follow the OpenFX 1.5 specification (headers vendored from github.com/AcademySoftwareFoundation/openfx, tag
// OFX_Release_1.5.1): https://openfx.readthedocs.io/en/main/Reference/ofxPropertySuite.html and its siblings.
// Everything here runs on the helper's main thread or on the threads of OfxMultiThreadSuiteV1::multiThread; a plug-in is
// third-party code, so every handle and index it passes is checked.

#include "OfxHost.h"

#include <openfx/ofxMemory.h>
#include <openfx/ofxMessage.h>
#include <openfx/ofxMultiThread.h>
#include <openfx/ofxProgress.h>
#include <openfx/ofxTimeLine.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <malloc.h>
#include <windows.h>

// ---- The property bag ---------------------------------------------------------------------------------------------

namespace {
constexpr std::size_t kMaxPropertyDimension = 4096;
}

std::size_t PropertyBag::Entry::size() const {
    switch (kind) {
    case Kind::Int:
        return ints.size();
    case Kind::Double:
        return doubles.size();
    case Kind::String:
        return strings.size();
    case Kind::Pointer:
        return pointers.size();
    }
    return 0;
}

void PropertyBag::Entry::resize(std::size_t n) {
    switch (kind) {
    case Kind::Int:
        ints.resize(n, 0);
        break;
    case Kind::Double:
        doubles.resize(n, 0.0);
        break;
    case Kind::String:
        strings.resize(n);
        break;
    case Kind::Pointer:
        pointers.resize(n, nullptr);
        break;
    }
}

PropertyBag::Entry* PropertyBag::find(const std::string& name) {
    const auto it = m_items.find(name);
    return it == m_items.end() ? nullptr : &it->second;
}

const PropertyBag::Entry* PropertyBag::find(const std::string& name) const {
    const auto it = m_items.find(name);
    return it == m_items.end() ? nullptr : &it->second;
}

PropertyBag::Entry* PropertyBag::ensure(const std::string& name, Kind kind) {
    auto it = m_items.find(name);
    if (it == m_items.end()) {
        Entry e;
        e.kind = kind;
        it = m_items.emplace(name, std::move(e)).first;
    }
    return it->second.kind == kind ? &it->second : nullptr;
}

void PropertyBag::setInt(const std::string& name, int value, std::size_t index) {
    Entry* e = ensure(name, Kind::Int);
    if (e == nullptr) {
        m_items.erase(name);
        e = ensure(name, Kind::Int);
    }
    e->resize(std::max(e->size(), index + 1));
    e->ints[index] = value;
}

void PropertyBag::setDouble(const std::string& name, double value, std::size_t index) {
    Entry* e = ensure(name, Kind::Double);
    if (e == nullptr) {
        m_items.erase(name);
        e = ensure(name, Kind::Double);
    }
    e->resize(std::max(e->size(), index + 1));
    e->doubles[index] = value;
}

void PropertyBag::setString(const std::string& name, std::string_view value, std::size_t index) {
    Entry* e = ensure(name, Kind::String);
    if (e == nullptr) {
        m_items.erase(name);
        e = ensure(name, Kind::String);
    }
    e->resize(std::max(e->size(), index + 1));
    e->strings[index] = std::string(value);
}

void PropertyBag::setPointer(const std::string& name, void* value, std::size_t index) {
    Entry* e = ensure(name, Kind::Pointer);
    if (e == nullptr) {
        m_items.erase(name);
        e = ensure(name, Kind::Pointer);
    }
    e->resize(std::max(e->size(), index + 1));
    e->pointers[index] = value;
}

void PropertyBag::setInts(const std::string& name, std::initializer_list<int> values) {
    m_items.erase(name);
    std::size_t i = 0;
    for (const int v : values) {
        setInt(name, v, i++);
    }
}

void PropertyBag::setDoubles(const std::string& name, std::initializer_list<double> values) {
    m_items.erase(name);
    std::size_t i = 0;
    for (const double v : values) {
        setDouble(name, v, i++);
    }
}

void PropertyBag::setStrings(const std::string& name, std::initializer_list<std::string_view> values) {
    m_items.erase(name);
    std::size_t i = 0;
    for (const std::string_view v : values) {
        setString(name, v, i++);
    }
}

int PropertyBag::getInt(const std::string& name, int fallback, std::size_t index) const {
    const Entry* e = find(name);
    if (e == nullptr) {
        return fallback;
    }
    if (e->kind == Kind::Int && index < e->ints.size()) {
        return e->ints[index];
    }
    if (e->kind == Kind::Double && index < e->doubles.size()) {
        return static_cast<int>(std::lround(e->doubles[index]));
    }
    return fallback;
}

double PropertyBag::getDouble(const std::string& name, double fallback, std::size_t index) const {
    const Entry* e = find(name);
    if (e == nullptr) {
        return fallback;
    }
    if (e->kind == Kind::Double && index < e->doubles.size()) {
        return e->doubles[index];
    }
    if (e->kind == Kind::Int && index < e->ints.size()) {
        return static_cast<double>(e->ints[index]);
    }
    return fallback;
}

std::string PropertyBag::getString(const std::string& name, const std::string& fallback, std::size_t index) const {
    const Entry* e = find(name);
    return e != nullptr && e->kind == Kind::String && index < e->strings.size() ? e->strings[index] : fallback;
}

void* PropertyBag::getPointer(const std::string& name, std::size_t index) const {
    const Entry* e = find(name);
    return e != nullptr && e->kind == Kind::Pointer && index < e->pointers.size() ? e->pointers[index] : nullptr;
}

std::vector<double> PropertyBag::numbers(const std::string& name) const {
    std::vector<double> out;
    if (const Entry* e = find(name)) {
        if (e->kind == Kind::Double) {
            out = e->doubles;
        } else if (e->kind == Kind::Int) {
            out.assign(e->ints.begin(), e->ints.end());
        }
    }
    return out;
}

std::vector<std::string> PropertyBag::strings(const std::string& name) const {
    const Entry* e = find(name);
    return e != nullptr && e->kind == Kind::String ? e->strings : std::vector<std::string>{};
}

std::size_t PropertyBag::dimension(const std::string& name) const {
    const Entry* e = find(name);
    return e != nullptr ? e->size() : 0;
}

namespace wabcut::ofxhost {

namespace {

const char* gLogPrefix = "[ofxhost]";

using Kind = PropertyBag::Kind;

// ---- Property suite -----------------------------------------------------------------------------------------------

OfxStatus checkSet(OfxPropertySetHandle properties, const char* property, int index) {
    if (properties == nullptr || property == nullptr) {
        return kOfxStatErrBadHandle;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= kMaxPropertyDimension) {
        return kOfxStatErrBadIndex;
    }
    return kOfxStatOK;
}

template <class T, class Store>
OfxStatus setOne(OfxPropertySetHandle properties, const char* property, int index, Kind kind, T value, Store store) {
    if (const OfxStatus s = checkSet(properties, property, index); s != kOfxStatOK) {
        return s;
    }
    PropertyBag::Entry* e = properties->bag.ensure(property, kind);
    if (e == nullptr) {
        return kOfxStatErrValue;
    }
    e->resize(std::max(e->size(), static_cast<std::size_t>(index) + 1));
    store(*e, static_cast<std::size_t>(index), value);
    return kOfxStatOK;
}

OfxStatus propSetPointer(OfxPropertySetHandle p, const char* name, int index, void* value) {
    return setOne(p, name, index, Kind::Pointer, value, [](PropertyBag::Entry& e, std::size_t i, void* v) { e.pointers[i] = v; });
}
OfxStatus propSetString(OfxPropertySetHandle p, const char* name, int index, const char* value) {
    return setOne(p, name, index, Kind::String, value != nullptr ? value : "",
                  [](PropertyBag::Entry& e, std::size_t i, const char* v) { e.strings[i] = v; });
}
OfxStatus propSetDouble(OfxPropertySetHandle p, const char* name, int index, double value) {
    return setOne(p, name, index, Kind::Double, value, [](PropertyBag::Entry& e, std::size_t i, double v) { e.doubles[i] = v; });
}
OfxStatus propSetInt(OfxPropertySetHandle p, const char* name, int index, int value) {
    return setOne(p, name, index, Kind::Int, value, [](PropertyBag::Entry& e, std::size_t i, int v) { e.ints[i] = v; });
}

template <class T>
OfxStatus setMany(OfxPropertySetHandle p, const char* name, int count, Kind kind, const T* values) {
    if (const OfxStatus s = checkSet(p, name, 0); s != kOfxStatOK) {
        return s;
    }
    if (count < 0 || static_cast<std::size_t>(count) > kMaxPropertyDimension || (count > 0 && values == nullptr)) {
        return kOfxStatErrBadIndex;
    }
    PropertyBag::Entry* e = p->bag.ensure(name, kind);
    if (e == nullptr) {
        return kOfxStatErrValue;
    }
    e->resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const auto k = static_cast<std::size_t>(i);
        if constexpr (std::is_same_v<T, void*>) {
            e->pointers[k] = values[i];
        } else if constexpr (std::is_same_v<T, const char*>) {
            e->strings[k] = values[i] != nullptr ? values[i] : "";
        } else if constexpr (std::is_same_v<T, double>) {
            e->doubles[k] = values[i];
        } else {
            e->ints[k] = values[i];
        }
    }
    return kOfxStatOK;
}

OfxStatus propSetPointerN(OfxPropertySetHandle p, const char* n, int c, void* const* v) {
    return setMany<void*>(p, n, c, Kind::Pointer, v);
}
OfxStatus propSetStringN(OfxPropertySetHandle p, const char* n, int c, const char* const* v) {
    return setMany<const char*>(p, n, c, Kind::String, v);
}
OfxStatus propSetDoubleN(OfxPropertySetHandle p, const char* n, int c, const double* v) {
    return setMany<double>(p, n, c, Kind::Double, v);
}
OfxStatus propSetIntN(OfxPropertySetHandle p, const char* n, int c, const int* v) {
    return setMany<int>(p, n, c, Kind::Int, v);
}

/// The entry `name` of `p` read at `index`, or the status that says why not.
OfxStatus entryAt(OfxPropertySetHandle p, const char* name, int index, Kind kind, const PropertyBag::Entry*& out) {
    if (p == nullptr || name == nullptr) {
        return kOfxStatErrBadHandle;
    }
    const PropertyBag::Entry* e = p->bag.find(name);
    if (e == nullptr) {
        return kOfxStatErrUnknown;
    }
    if (e->kind != kind) {
        return kOfxStatErrValue;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= e->size()) {
        return kOfxStatErrBadIndex;
    }
    out = e;
    return kOfxStatOK;
}

OfxStatus propGetPointer(OfxPropertySetHandle p, const char* name, int index, void** value) {
    const PropertyBag::Entry* e = nullptr;
    if (const OfxStatus s = entryAt(p, name, index, Kind::Pointer, e); s != kOfxStatOK) {
        return s;
    }
    if (value == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *value = e->pointers[static_cast<std::size_t>(index)];
    return kOfxStatOK;
}
OfxStatus propGetString(OfxPropertySetHandle p, const char* name, int index, char** value) {
    const PropertyBag::Entry* e = nullptr;
    if (const OfxStatus s = entryAt(p, name, index, Kind::String, e); s != kOfxStatOK) {
        return s;
    }
    if (value == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *value = const_cast<char*>(e->strings[static_cast<std::size_t>(index)].c_str());
    return kOfxStatOK;
}
OfxStatus propGetDouble(OfxPropertySetHandle p, const char* name, int index, double* value) {
    const PropertyBag::Entry* e = nullptr;
    if (const OfxStatus s = entryAt(p, name, index, Kind::Double, e); s != kOfxStatOK) {
        return s;
    }
    if (value == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *value = e->doubles[static_cast<std::size_t>(index)];
    return kOfxStatOK;
}
OfxStatus propGetInt(OfxPropertySetHandle p, const char* name, int index, int* value) {
    const PropertyBag::Entry* e = nullptr;
    if (const OfxStatus s = entryAt(p, name, index, Kind::Int, e); s != kOfxStatOK) {
        return s;
    }
    if (value == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *value = e->ints[static_cast<std::size_t>(index)];
    return kOfxStatOK;
}

template <class T, class Get>
OfxStatus getMany(OfxPropertySetHandle p, const char* name, int count, Kind kind, T* out, Get get) {
    if (p == nullptr || name == nullptr || (count > 0 && out == nullptr)) {
        return kOfxStatErrBadHandle;
    }
    const PropertyBag::Entry* e = p->bag.find(name);
    if (e == nullptr) {
        return kOfxStatErrUnknown;
    }
    if (e->kind != kind) {
        return kOfxStatErrValue;
    }
    if (count < 0 || static_cast<std::size_t>(count) > e->size()) {
        return kOfxStatErrBadIndex;
    }
    for (int i = 0; i < count; ++i) {
        out[i] = get(*e, static_cast<std::size_t>(i));
    }
    return kOfxStatOK;
}

OfxStatus propGetPointerN(OfxPropertySetHandle p, const char* n, int c, void** v) {
    return getMany(p, n, c, Kind::Pointer, v, [](const PropertyBag::Entry& e, std::size_t i) { return e.pointers[i]; });
}
OfxStatus propGetStringN(OfxPropertySetHandle p, const char* n, int c, char** v) {
    return getMany(p, n, c, Kind::String, v,
                   [](const PropertyBag::Entry& e, std::size_t i) { return const_cast<char*>(e.strings[i].c_str()); });
}
OfxStatus propGetDoubleN(OfxPropertySetHandle p, const char* n, int c, double* v) {
    return getMany(p, n, c, Kind::Double, v, [](const PropertyBag::Entry& e, std::size_t i) { return e.doubles[i]; });
}
OfxStatus propGetIntN(OfxPropertySetHandle p, const char* n, int c, int* v) {
    return getMany(p, n, c, Kind::Int, v, [](const PropertyBag::Entry& e, std::size_t i) { return e.ints[i]; });
}

OfxStatus propReset(OfxPropertySetHandle p, const char* name) {
    if (p == nullptr || name == nullptr) {
        return kOfxStatErrBadHandle;
    }
    if (!p->bag.has(name)) {
        return kOfxStatErrUnknown;
    }
    p->bag.reset(name);
    return kOfxStatOK;
}

OfxStatus propGetDimension(OfxPropertySetHandle p, const char* name, int* count) {
    if (p == nullptr || name == nullptr || count == nullptr) {
        return kOfxStatErrBadHandle;
    }
    if (!p->bag.has(name)) {
        return kOfxStatErrUnknown;
    }
    *count = static_cast<int>(p->bag.dimension(name));
    return kOfxStatOK;
}

const OfxPropertySuiteV1 gPropertySuite = {
    propSetPointer, propSetString, propSetDouble, propSetInt, propSetPointerN, propSetStringN, propSetDoubleN, propSetIntN,
    propGetPointer, propGetString, propGetDouble, propGetInt, propGetPointerN, propGetStringN, propGetDoubleN, propGetIntN,
    propReset,      propGetDimension};

// ---- Memory suite -------------------------------------------------------------------------------------------------

OfxStatus memoryAlloc(void*, size_t bytes, void** out) {
    if (out == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *out = _aligned_malloc(bytes == 0 ? 1 : bytes, 64);
    return *out != nullptr ? kOfxStatOK : kOfxStatErrMemory;
}

OfxStatus memoryFree(void* data) {
    _aligned_free(data);
    return kOfxStatOK;
}

const OfxMemorySuiteV1 gMemorySuite = {memoryAlloc, memoryFree};

/// Image memory (OfxImageEffectSuiteV1::imageMemoryAlloc): a block and its size, behind an opaque handle.
struct ImageMemory {
    void* data = nullptr;
    std::size_t bytes = 0;
};

std::mutex gImageMemoryMutex;
std::set<ImageMemory*>& imageMemories() {
    static std::set<ImageMemory*> all;
    return all;
}

OfxStatus imageMemoryAlloc(OfxImageEffectHandle, size_t bytes, OfxImageMemoryHandle* out) {
    if (out == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *out = nullptr;
    auto* memory = new (std::nothrow) ImageMemory();
    if (memory == nullptr) {
        return kOfxStatErrMemory;
    }
    memory->data = _aligned_malloc(bytes == 0 ? 1 : bytes, 64);
    if (memory->data == nullptr) {
        delete memory;
        return kOfxStatErrMemory;
    }
    memory->bytes = bytes;
    {
        std::lock_guard lock(gImageMemoryMutex);
        imageMemories().insert(memory);
    }
    *out = reinterpret_cast<OfxImageMemoryHandle>(memory);
    return kOfxStatOK;
}

ImageMemory* imageMemoryOf(OfxImageMemoryHandle handle) {
    auto* memory = reinterpret_cast<ImageMemory*>(handle);
    std::lock_guard lock(gImageMemoryMutex);
    return imageMemories().count(memory) != 0 ? memory : nullptr;
}

OfxStatus imageMemoryFree(OfxImageMemoryHandle handle) {
    ImageMemory* memory = imageMemoryOf(handle);
    if (memory == nullptr) {
        return kOfxStatErrBadHandle;
    }
    {
        std::lock_guard lock(gImageMemoryMutex);
        imageMemories().erase(memory);
    }
    _aligned_free(memory->data);
    delete memory;
    return kOfxStatOK;
}

OfxStatus imageMemoryLock(OfxImageMemoryHandle handle, void** out) {
    if (out == nullptr) {
        return kOfxStatErrBadHandle;
    }
    ImageMemory* memory = imageMemoryOf(handle);
    *out = memory != nullptr ? memory->data : nullptr;
    return memory != nullptr ? kOfxStatOK : kOfxStatErrBadHandle;
}

OfxStatus imageMemoryUnlock(OfxImageMemoryHandle handle) { return imageMemoryOf(handle) != nullptr ? kOfxStatOK : kOfxStatErrBadHandle; }

// ---- Multi-thread suite -------------------------------------------------------------------------------------------

thread_local unsigned tlsThreadIndex = 0;
thread_local bool tlsSpawned = false;
std::atomic<bool> gInMultiThread{false};

unsigned cpuCount() {
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1u : n;
}

OfxStatus multiThread(OfxThreadFunctionV1 func, unsigned int nThreads, void* customArg) {
    if (func == nullptr || nThreads == 0) {
        return kOfxStatFailed;
    }
    if (gInMultiThread.exchange(true)) {
        return kOfxStatErrExists; // the suite cannot be called recursively
    }
    // The threads are limited to the CPUs there are (the spec), and to a sane number.
    const unsigned n = std::min({nThreads, cpuCount(), 64u});
    std::vector<std::thread> threads;
    threads.reserve(n);
    for (unsigned i = 1; i < n; ++i) {
        try {
            threads.emplace_back([=] {
                tlsThreadIndex = i;
                tlsSpawned = true;
                try {
                    func(i, n, customArg);
                } catch (...) {
                    std::fprintf(stderr, "%s a plug-in's thread threw an exception\n", gLogPrefix);
                }
            });
        } catch (const std::exception&) {
            break; // no more threads: the ones started and this one do the work
        }
    }
    tlsThreadIndex = 0;
    func(0, n, customArg);
    for (std::thread& t : threads) {
        t.join();
    }
    gInMultiThread.store(false);
    return kOfxStatOK;
}

OfxStatus multiThreadNumCPUs(unsigned int* n) {
    if (n == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *n = cpuCount();
    return kOfxStatOK;
}

OfxStatus multiThreadIndex(unsigned int* index) {
    if (index == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *index = tlsThreadIndex;
    return tlsSpawned ? kOfxStatOK : kOfxStatFailed;
}

int multiThreadIsSpawnedThread() { return tlsSpawned ? 1 : 0; }

} // namespace
} // namespace wabcut::ofxhost

struct OfxMutex {
    std::recursive_mutex mutex;
};

namespace wabcut::ofxhost {
namespace {

std::mutex gMutexesMutex;
std::set<OfxMutex*>& mutexes() {
    static std::set<OfxMutex*> all;
    return all;
}

OfxMutex* mutexOf(OfxMutexHandle handle) {
    std::lock_guard lock(gMutexesMutex);
    return mutexes().count(handle) != 0 ? handle : nullptr;
}

OfxStatus mutexCreate(OfxMutexHandle* out, int lockCount) {
    if (out == nullptr) {
        return kOfxStatErrBadHandle;
    }
    auto* m = new (std::nothrow) OfxMutex();
    if (m == nullptr) {
        return kOfxStatErrMemory;
    }
    for (int i = 0; i < lockCount && i < 1024; ++i) {
        m->mutex.lock();
    }
    {
        std::lock_guard lock(gMutexesMutex);
        mutexes().insert(m);
    }
    *out = m;
    return kOfxStatOK;
}

OfxStatus mutexDestroy(const OfxMutexHandle handle) {
    OfxMutex* m = mutexOf(handle);
    if (m == nullptr) {
        return kOfxStatErrBadHandle;
    }
    {
        std::lock_guard lock(gMutexesMutex);
        mutexes().erase(m);
    }
    delete m;
    return kOfxStatOK;
}

OfxStatus mutexLock(const OfxMutexHandle handle) {
    OfxMutex* m = mutexOf(handle);
    if (m == nullptr) {
        return kOfxStatErrBadHandle;
    }
    m->mutex.lock();
    return kOfxStatOK;
}

OfxStatus mutexUnLock(const OfxMutexHandle handle) {
    OfxMutex* m = mutexOf(handle);
    if (m == nullptr) {
        return kOfxStatErrBadHandle;
    }
    m->mutex.unlock();
    return kOfxStatOK;
}

OfxStatus mutexTryLock(const OfxMutexHandle handle) {
    OfxMutex* m = mutexOf(handle);
    if (m == nullptr) {
        return kOfxStatErrBadHandle;
    }
    return m->mutex.try_lock() ? kOfxStatOK : kOfxStatFailed;
}

const OfxMultiThreadSuiteV1 gMultiThreadSuite = {multiThread,  multiThreadNumCPUs, multiThreadIndex, multiThreadIsSpawnedThread,
                                                 mutexCreate,  mutexDestroy,       mutexLock,        mutexUnLock,
                                                 mutexTryLock};

// ---- Message suite ------------------------------------------------------------------------------------------------

Effect* effectOf(const void* handle) { return const_cast<Effect*>(static_cast<const Effect*>(handle)); }

OfxStatus message(void* handle, const char* type, const char* /*id*/, const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format != nullptr ? format : "", args);
    va_end(args);
    const std::string kind = type != nullptr ? type : "";
    std::fprintf(stderr, "%s plug-in message (%s): %s\n", gLogPrefix, kind.c_str(), text);
    if (kind == kOfxMessageError || kind == kOfxMessageFatal) {
        if (Effect* effect = effectOf(handle); effect != nullptr && handle != nullptr) {
            effect->lastMessage = text;
        }
    }
    // A question needs a user: the helper has none, so it answers "no".
    return kind == kOfxMessageQuestion ? kOfxStatReplyNo : kOfxStatOK;
}

OfxStatus setPersistentMessage(void* handle, const char* type, const char* id, const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format != nullptr ? format : "", args);
    va_end(args);
    return message(handle, type, id, "%s", text);
}

OfxStatus clearPersistentMessage(void*) { return kOfxStatOK; }

const OfxMessageSuiteV1 gMessageSuiteV1 = {message};
const OfxMessageSuiteV2 gMessageSuiteV2 = {message, setPersistentMessage, clearPersistentMessage};

// ---- Progress and time line (accepted, no user interface) ---------------------------------------------------------

OfxStatus progressStart(void*, const char*) { return kOfxStatOK; }
OfxStatus progressUpdate(void*, double) { return kOfxStatOK; }
OfxStatus progressEnd(void*) { return kOfxStatOK; }
const OfxProgressSuiteV1 gProgressSuiteV1 = {progressStart, progressUpdate, progressEnd};

// ---- Parameter suite ----------------------------------------------------------------------------------------------

ParamSet* paramSetOf(OfxParamSetHandle handle) { return reinterpret_cast<ParamSet*>(handle); }
ParamDef* paramOf(OfxParamHandle handle) { return reinterpret_cast<ParamDef*>(handle); }

/// A set belongs to exactly one effect; the handle is the effect's `params` member.
bool isHostParamSet(const ParamSet* set) { return set != nullptr; }

void defaultsFor(ParamDef& p) {
    PropertyBag& b = p.props.bag;
    b.setString(kOfxPropType, kOfxTypeParameter);
    b.setString(kOfxParamPropType, [&]() -> std::string_view {
        switch (p.kind) {
        case ParamKind::Double:
            return kOfxParamTypeDouble;
        case ParamKind::Int:
            return kOfxParamTypeInteger;
        case ParamKind::Bool:
            return kOfxParamTypeBoolean;
        case ParamKind::Choice:
            return kOfxParamTypeChoice;
        case ParamKind::StrChoice:
            return kOfxParamTypeStrChoice;
        case ParamKind::Rgba:
            return kOfxParamTypeRGBA;
        case ParamKind::Rgb:
            return kOfxParamTypeRGB;
        case ParamKind::Double2D:
            return kOfxParamTypeDouble2D;
        case ParamKind::Int2D:
            return kOfxParamTypeInteger2D;
        case ParamKind::Double3D:
            return kOfxParamTypeDouble3D;
        case ParamKind::Int3D:
            return kOfxParamTypeInteger3D;
        case ParamKind::String:
            return kOfxParamTypeString;
        case ParamKind::Custom:
            return kOfxParamTypeCustom;
        case ParamKind::Bytes:
            return kOfxParamTypeBytes;
        case ParamKind::Group:
            return kOfxParamTypeGroup;
        case ParamKind::Page:
            return kOfxParamTypePage;
        case ParamKind::PushButton:
            return kOfxParamTypePushButton;
        default:
            return "";
        }
    }());
    b.setString(kOfxPropName, p.name);
    b.setInt(kOfxParamPropSecret, 0);
    b.setInt(kOfxParamPropEnabled, 1);
    b.setInt(kOfxParamPropEvaluateOnChange, 1);
    b.setInt(kOfxParamPropCanUndo, 1);
    b.setInt(kOfxParamPropPersistant, 1);
    b.setInt(kOfxParamPropPluginMayWrite, 0);
    b.setInt(kOfxParamPropAnimates, paramIsNumeric(p.kind) ? 1 : 0);
    b.setString(kOfxParamPropHint, "");
    b.setString(kOfxParamPropParent, "");
    b.setString(kOfxParamPropScriptName, p.name);
    const int dim = paramDimension(p.kind);
    switch (p.kind) {
    case ParamKind::Double:
    case ParamKind::Rgba:
    case ParamKind::Rgb:
    case ParamKind::Double2D:
    case ParamKind::Double3D: {
        const bool colour = p.kind == ParamKind::Rgba || p.kind == ParamKind::Rgb;
        for (int i = 0; i < dim; ++i) {
            const auto k = static_cast<std::size_t>(i);
            b.setDouble(kOfxParamPropDefault, colour && i == 3 ? 1.0 : 0.0, k);
            b.setDouble(kOfxParamPropMin, colour ? 0.0 : -DBL_MAX, k);
            b.setDouble(kOfxParamPropMax, colour ? 1.0 : DBL_MAX, k);
            b.setDouble(kOfxParamPropDisplayMin, colour ? 0.0 : -DBL_MAX, k);
            b.setDouble(kOfxParamPropDisplayMax, colour ? 1.0 : DBL_MAX, k);
        }
        b.setDouble(kOfxParamPropIncrement, 1.0);
        b.setInt(kOfxParamPropDigits, 2);
        if (p.kind == ParamKind::Double || p.kind == ParamKind::Double2D || p.kind == ParamKind::Double3D) {
            b.setString(kOfxParamPropDoubleType, kOfxParamDoubleTypePlain);
            b.setString(kOfxParamPropDefaultCoordinateSystem, kOfxParamCoordinatesCanonical);
        }
        break;
    }
    case ParamKind::Int:
    case ParamKind::Int2D:
    case ParamKind::Int3D:
        for (int i = 0; i < dim; ++i) {
            const auto k = static_cast<std::size_t>(i);
            b.setInt(kOfxParamPropDefault, 0, k);
            b.setInt(kOfxParamPropMin, INT_MIN, k);
            b.setInt(kOfxParamPropMax, INT_MAX, k);
            b.setInt(kOfxParamPropDisplayMin, INT_MIN, k);
            b.setInt(kOfxParamPropDisplayMax, INT_MAX, k);
        }
        break;
    case ParamKind::Bool:
    case ParamKind::Choice:
        b.setInt(kOfxParamPropDefault, 0);
        break;
    case ParamKind::StrChoice:
    case ParamKind::String:
        b.setString(kOfxParamPropDefault, "");
        if (p.kind == ParamKind::String) {
            b.setString(kOfxParamPropStringMode, kOfxParamStringIsSingleLine);
        }
        break;
    default:
        break;
    }
}

OfxStatus paramDefine(OfxParamSetHandle handle, const char* type, const char* name, OfxPropertySetHandle* props) {
    ParamSet* set = paramSetOf(handle);
    if (!isHostParamSet(set) || type == nullptr || name == nullptr) {
        return kOfxStatErrBadHandle;
    }
    const ParamKind kind = paramKindOf(type);
    if (kind == ParamKind::Unknown) {
        return kOfxStatErrUnknown;
    }
    if (set->find(name) != nullptr) {
        return kOfxStatErrExists;
    }
    if (set->params.size() >= 4096) {
        return kOfxStatErrMemory;
    }
    auto p = std::make_unique<ParamDef>();
    p->name = name;
    p->kind = kind;
    defaultsFor(*p);
    if (props != nullptr) {
        *props = &p->props;
    }
    set->params.push_back(std::move(p));
    return kOfxStatOK;
}

OfxStatus paramGetHandle(OfxParamSetHandle handle, const char* name, OfxParamHandle* param, OfxPropertySetHandle* props) {
    ParamSet* set = paramSetOf(handle);
    if (!isHostParamSet(set) || name == nullptr || param == nullptr) {
        return kOfxStatErrBadHandle;
    }
    ParamDef* p = set->find(name);
    if (p == nullptr) {
        return kOfxStatErrUnknown;
    }
    *param = reinterpret_cast<OfxParamHandle>(p);
    if (props != nullptr) {
        *props = &p->props;
    }
    return kOfxStatOK;
}

OfxStatus paramSetGetPropertySet(OfxParamSetHandle handle, OfxPropertySetHandle* props) {
    // The property set of a parameter set is its effect's (the spec: "the same as the plug-in instance's").
    if (handle == nullptr || props == nullptr) {
        return kOfxStatErrBadHandle;
    }
    const ParamSet* set = paramSetOf(handle);
    if (set->effect == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *props = &set->effect->props;
    return kOfxStatOK;
}

OfxStatus paramGetPropertySet(OfxParamHandle handle, OfxPropertySetHandle* props) {
    ParamDef* p = paramOf(handle);
    if (p == nullptr || props == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *props = &p->props;
    return kOfxStatOK;
}

int toInt(double v) { return static_cast<int>(std::lround(std::clamp(v, static_cast<double>(INT_MIN), static_cast<double>(INT_MAX)))); }

/// Writes a parameter's value to the pointers of `args` (paramGetValue's varargs).
OfxStatus readValue(ParamDef& p, va_list args) {
    const auto num = [&](std::size_t i) { return i < p.numbers.size() ? p.numbers[i] : 0.0; };
    switch (p.kind) {
    case ParamKind::Double: {
        if (double* out = va_arg(args, double*)) {
            *out = num(0);
        }
        return kOfxStatOK;
    }
    case ParamKind::Int:
    case ParamKind::Bool:
    case ParamKind::Choice: {
        if (int* out = va_arg(args, int*)) {
            *out = p.kind == ParamKind::Bool ? (num(0) != 0.0 ? 1 : 0) : toInt(num(0));
        }
        return kOfxStatOK;
    }
    case ParamKind::Rgba:
    case ParamKind::Rgb:
    case ParamKind::Double2D:
    case ParamKind::Double3D: {
        const int n = paramDimension(p.kind);
        for (int i = 0; i < n; ++i) {
            if (double* out = va_arg(args, double*)) {
                *out = num(static_cast<std::size_t>(i));
            }
        }
        return kOfxStatOK;
    }
    case ParamKind::Int2D:
    case ParamKind::Int3D: {
        const int n = paramDimension(p.kind);
        for (int i = 0; i < n; ++i) {
            if (int* out = va_arg(args, int*)) {
                *out = toInt(num(static_cast<std::size_t>(i)));
            }
        }
        return kOfxStatOK;
    }
    case ParamKind::String:
    case ParamKind::StrChoice:
    case ParamKind::Custom: {
        if (char** out = va_arg(args, char**)) {
            p.returned = p.text;
            *out = p.returned.data();
        }
        return kOfxStatOK;
    }
    default:
        return kOfxStatErrBadHandle;
    }
}

OfxStatus paramGetValue(OfxParamHandle handle, ...) {
    ParamDef* p = paramOf(handle);
    if (p == nullptr) {
        return kOfxStatErrBadHandle;
    }
    va_list args;
    va_start(args, handle);
    const OfxStatus s = readValue(*p, args);
    va_end(args);
    return s;
}

// The parameters are constants within a render (the editor evaluates curves itself and sends the values at the frame's
// time), so a value at any time is the value.
OfxStatus paramGetValueAtTime(OfxParamHandle handle, OfxTime time, ...) {
    ParamDef* p = paramOf(handle);
    if (p == nullptr) {
        return kOfxStatErrBadHandle;
    }
    va_list args;
    va_start(args, time);
    const OfxStatus s = readValue(*p, args);
    va_end(args);
    return s;
}

OfxStatus paramGetDerivative(OfxParamHandle handle, OfxTime time, ...) {
    ParamDef* p = paramOf(handle);
    if (p == nullptr) {
        return kOfxStatErrBadHandle;
    }
    va_list args;
    va_start(args, time);
    const int n = paramDimension(p->kind);
    for (int i = 0; i < n; ++i) {
        if (double* out = va_arg(args, double*)) {
            *out = 0.0;
        }
    }
    va_end(args);
    return kOfxStatOK;
}

OfxStatus paramGetIntegral(OfxParamHandle handle, OfxTime t1, OfxTime t2, ...) {
    ParamDef* p = paramOf(handle);
    if (p == nullptr) {
        return kOfxStatErrBadHandle;
    }
    va_list args;
    va_start(args, t2);
    const int n = paramDimension(p->kind);
    for (int i = 0; i < n; ++i) {
        if (double* out = va_arg(args, double*)) {
            *out = (i < static_cast<int>(p->numbers.size()) ? p->numbers[static_cast<std::size_t>(i)] : 0.0) * (t2 - t1);
        }
    }
    va_end(args);
    return kOfxStatOK;
}

OfxStatus setValueFrom(ParamDef& p, va_list args) {
    switch (p.kind) {
    case ParamKind::Double:
        p.numbers.assign(1, va_arg(args, double));
        return kOfxStatOK;
    case ParamKind::Int:
    case ParamKind::Bool:
    case ParamKind::Choice:
        p.numbers.assign(1, static_cast<double>(va_arg(args, int)));
        return kOfxStatOK;
    case ParamKind::Rgba:
    case ParamKind::Rgb:
    case ParamKind::Double2D:
    case ParamKind::Double3D: {
        const int n = paramDimension(p.kind);
        p.numbers.assign(static_cast<std::size_t>(n), 0.0);
        for (int i = 0; i < n; ++i) {
            p.numbers[static_cast<std::size_t>(i)] = va_arg(args, double);
        }
        return kOfxStatOK;
    }
    case ParamKind::Int2D:
    case ParamKind::Int3D: {
        const int n = paramDimension(p.kind);
        p.numbers.assign(static_cast<std::size_t>(n), 0.0);
        for (int i = 0; i < n; ++i) {
            p.numbers[static_cast<std::size_t>(i)] = static_cast<double>(va_arg(args, int));
        }
        return kOfxStatOK;
    }
    case ParamKind::String:
    case ParamKind::StrChoice:
    case ParamKind::Custom: {
        const char* text = va_arg(args, const char*);
        p.text = text != nullptr ? std::string(text, strnlen(text, kMaxTextBytes)) : std::string();
        return kOfxStatOK;
    }
    default:
        return kOfxStatErrBadHandle;
    }
}

OfxStatus paramSetValue(OfxParamHandle handle, ...) {
    ParamDef* p = paramOf(handle);
    if (p == nullptr) {
        return kOfxStatErrBadHandle;
    }
    va_list args;
    va_start(args, handle);
    const OfxStatus s = setValueFrom(*p, args);
    va_end(args);
    return s;
}

OfxStatus paramSetValueAtTime(OfxParamHandle handle, OfxTime time, ...) {
    ParamDef* p = paramOf(handle);
    if (p == nullptr) {
        return kOfxStatErrBadHandle;
    }
    va_list args;
    va_start(args, time);
    const OfxStatus s = setValueFrom(*p, args);
    va_end(args);
    return s;
}

// Keys: the editor owns animation, so a plug-in sees none.
OfxStatus paramGetNumKeys(OfxParamHandle handle, unsigned int* n) {
    if (paramOf(handle) == nullptr || n == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *n = 0;
    return kOfxStatOK;
}
OfxStatus paramGetKeyTime(OfxParamHandle handle, unsigned int, OfxTime*) {
    return paramOf(handle) == nullptr ? kOfxStatErrBadHandle : kOfxStatErrBadIndex;
}
OfxStatus paramGetKeyIndex(OfxParamHandle handle, OfxTime, int, int* index) {
    if (paramOf(handle) == nullptr) {
        return kOfxStatErrBadHandle;
    }
    if (index != nullptr) {
        *index = -1;
    }
    return kOfxStatFailed;
}
OfxStatus paramDeleteKey(OfxParamHandle handle, OfxTime) { return paramOf(handle) == nullptr ? kOfxStatErrBadHandle : kOfxStatOK; }
OfxStatus paramDeleteAllKeys(OfxParamHandle handle) { return paramOf(handle) == nullptr ? kOfxStatErrBadHandle : kOfxStatOK; }

OfxStatus paramCopy(OfxParamHandle to, OfxParamHandle from, OfxTime, const OfxRangeD*) {
    ParamDef* a = paramOf(to);
    ParamDef* b = paramOf(from);
    if (a == nullptr || b == nullptr) {
        return kOfxStatErrBadHandle;
    }
    if (a->kind != b->kind) {
        return kOfxStatErrBadHandle;
    }
    a->numbers = b->numbers;
    a->text = b->text;
    return kOfxStatOK;
}

OfxStatus paramEditBegin(OfxParamSetHandle, const char*) { return kOfxStatOK; }
OfxStatus paramEditEnd(OfxParamSetHandle) { return kOfxStatOK; }

const OfxParameterSuiteV1 gParameterSuite = {
    paramDefine,        paramGetHandle,    paramSetGetPropertySet, paramGetPropertySet, paramGetValue,       paramGetValueAtTime,
    paramGetDerivative, paramGetIntegral,  paramSetValue,          paramSetValueAtTime, paramGetNumKeys,     paramGetKeyTime,
    paramGetKeyIndex,   paramDeleteKey,    paramDeleteAllKeys,     paramCopy,           paramEditBegin,      paramEditEnd};

// ---- Image effect suite -------------------------------------------------------------------------------------------

OfxStatus getPropertySet(OfxImageEffectHandle handle, OfxPropertySetHandle* props) {
    Effect* effect = effectOf(handle);
    if (effect == nullptr || props == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *props = &effect->props;
    return kOfxStatOK;
}

OfxStatus getParamSet(OfxImageEffectHandle handle, OfxParamSetHandle* set) {
    Effect* effect = effectOf(handle);
    if (effect == nullptr || set == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *set = reinterpret_cast<OfxParamSetHandle>(&effect->params);
    return kOfxStatOK;
}

OfxStatus clipDefine(OfxImageEffectHandle handle, const char* name, OfxPropertySetHandle* props) {
    Effect* effect = effectOf(handle);
    if (effect == nullptr || name == nullptr) {
        return kOfxStatErrBadHandle;
    }
    if (effect->find(name) != nullptr) {
        return kOfxStatErrExists;
    }
    if (effect->clips.size() >= 64) {
        return kOfxStatErrMemory;
    }
    auto clip = std::make_unique<Clip>();
    clip->name = name;
    clip->owner = effect;
    clip->props.bag.setString(kOfxPropType, kOfxTypeClip);
    clip->props.bag.setString(kOfxPropName, name);
    clip->props.bag.setString(kOfxPropLabel, name);
    clip->props.bag.setInt(kOfxImageClipPropOptional, 0);
    clip->props.bag.setInt(kOfxImageClipPropIsMask, 0);
    clip->props.bag.setInt(kOfxImageEffectPropSupportsTiles, 0);
    if (props != nullptr) {
        *props = &clip->props;
    }
    effect->clips.push_back(std::move(clip));
    return kOfxStatOK;
}

OfxStatus clipGetHandle(OfxImageEffectHandle handle, const char* name, OfxImageClipHandle* clip, OfxPropertySetHandle* props) {
    Effect* effect = effectOf(handle);
    if (effect == nullptr || name == nullptr || clip == nullptr) {
        return kOfxStatErrBadHandle;
    }
    Clip* c = effect->find(name);
    if (c == nullptr) {
        return kOfxStatErrUnknown;
    }
    *clip = reinterpret_cast<OfxImageClipHandle>(c);
    if (props != nullptr) {
        *props = &c->props;
    }
    return kOfxStatOK;
}

OfxStatus clipGetPropertySet(OfxImageClipHandle handle, OfxPropertySetHandle* props) {
    auto* clip = reinterpret_cast<Clip*>(handle);
    if (clip == nullptr || props == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *props = &clip->props;
    return kOfxStatOK;
}

OfxStatus clipGetImage(OfxImageClipHandle handle, OfxTime, const OfxRectD*, OfxPropertySetHandle* image) {
    auto* clip = reinterpret_cast<Clip*>(handle);
    if (clip == nullptr || image == nullptr) {
        return kOfxStatErrBadHandle;
    }
    *image = nullptr;
    Effect* effect = clip->owner;
    if (effect == nullptr || !effect->instance) {
        return kOfxStatFailed;
    }
    // The picture of the clip is bound to its property `kOfxImagePropData`'s owner by the instance, at render time: a clip
    // that is not connected (an optional input this host has no picture for) has none.
    void* bound = clip->props.bag.getPointer("wabcut.image");
    if (bound == nullptr) {
        return kOfxStatFailed;
    }
    auto* img = static_cast<Image*>(bound);
    effect->images.insert(img);
    *image = &img->props;
    return kOfxStatOK;
}

OfxStatus clipReleaseImage(OfxPropertySetHandle image) {
    if (image == nullptr) {
        return kOfxStatErrBadHandle;
    }
    // Images live for the render that made them; releasing only forgets the plug-in's hold.
    return kOfxStatOK;
}

OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle handle, OfxTime, OfxRectD* bounds) {
    auto* clip = reinterpret_cast<Clip*>(handle);
    if (clip == nullptr || bounds == nullptr || clip->owner == nullptr) {
        return kOfxStatErrBadHandle;
    }
    bounds->x1 = 0.0;
    bounds->y1 = 0.0;
    bounds->x2 = clip->owner->width;
    bounds->y2 = clip->owner->height;
    return kOfxStatOK;
}

int abortRender(OfxImageEffectHandle) { return 0; }

const OfxImageEffectSuiteV1 gImageEffectSuite = {getPropertySet,    getParamSet,   clipDefine,         clipGetHandle,     clipGetPropertySet,
                                                  clipGetImage,      clipReleaseImage, clipGetRegionOfDefinition, abortRender,
                                                  imageMemoryAlloc,  imageMemoryFree, imageMemoryLock,  imageMemoryUnlock};

// ---- The host -----------------------------------------------------------------------------------------------------

OfxPropertySetStruct& hostProperties() {
    static OfxPropertySetStruct props = [] {
        OfxPropertySetStruct p;
        PropertyBag& b = p.bag;
        b.setString(kOfxPropType, kOfxTypeImageEffectHost);
        b.setString(kOfxPropName, "org.wabcut.ofxhost");
        b.setString(kOfxPropLabel, "Wabcut");
        b.setInts(kOfxPropVersion, {0, 7, 0});
        b.setString(kOfxPropVersionLabel, "0.7");
        b.setInts(kOfxPropAPIVersion, {1, 5});
        b.setInt(kOfxImageEffectHostPropIsBackground, 1); // a processing-only host: no interactive session
        b.setInt(kOfxImageEffectPropSupportsOverlays, 0);
        b.setInt(kOfxImageEffectPropSupportsMultiResolution, 0);
        b.setInt(kOfxImageEffectPropSupportsTiles, 0);
        b.setInt(kOfxImageEffectPropTemporalClipAccess, 0);
        b.setStrings(kOfxImageEffectPropSupportedComponents, {kOfxImageComponentRGBA, kOfxImageComponentRGB});
        b.setStrings(kOfxImageEffectPropSupportedContexts, {kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral});
        b.setStrings(kOfxImageEffectPropSupportedPixelDepths, {kOfxBitDepthByte, kOfxBitDepthShort, kOfxBitDepthFloat});
        b.setInt(kOfxImageEffectPropSetableFrameRate, 0);
        b.setInt(kOfxImageEffectPropSetableFielding, 0);
        b.setInt(kOfxImageEffectPropSupportsMultipleClipDepths, 0);
        b.setInt(kOfxImageEffectPropSupportsMultipleClipPARs, 0);
        b.setInt(kOfxParamHostPropSupportsCustomAnimation, 0);
        b.setInt(kOfxParamHostPropSupportsStringAnimation, 0);
        b.setInt(kOfxParamHostPropSupportsBooleanAnimation, 0);
        b.setInt(kOfxParamHostPropSupportsChoiceAnimation, 0);
        b.setInt(kOfxParamHostPropSupportsStrChoiceAnimation, 0);
        b.setInt(kOfxParamHostPropSupportsStrChoice, 1);
        b.setInt(kOfxParamHostPropSupportsCustomInteract, 0);
        b.setInt(kOfxParamHostPropMaxParameters, -1);
        b.setInt(kOfxParamHostPropMaxPages, 0);
        b.setInts(kOfxParamHostPropPageRowColumnCount, {0, 0});
        b.setString(kOfxImageEffectHostPropNativeOrigin, kOfxHostNativeOriginBottomLeft);
        b.setString("OfxImageEffectPropOpenGLRenderSupported", "false");
        return p;
    }();
    return props;
}

const void* fetchSuite(OfxPropertySetHandle host, const char* name, int version) {
    if (host != &hostProperties() || name == nullptr) {
        return nullptr;
    }
    const std::string_view n = name;
    if (n == kOfxPropertySuite && version == 1) {
        return &gPropertySuite;
    }
    if (n == kOfxImageEffectSuite && version == 1) {
        return &gImageEffectSuite;
    }
    if (n == kOfxParameterSuite && version == 1) {
        return &gParameterSuite;
    }
    if (n == kOfxMemorySuite && version == 1) {
        return &gMemorySuite;
    }
    if (n == kOfxMultiThreadSuite && version == 1) {
        return &gMultiThreadSuite;
    }
    if (n == kOfxMessageSuite && version == 1) {
        return &gMessageSuiteV1;
    }
    if (n == kOfxMessageSuite && version == 2) {
        return &gMessageSuiteV2;
    }
    if (n == kOfxProgressSuite && version == 1) {
        return &gProgressSuiteV1;
    }
    return nullptr; // the OpenGL, draw, colour-space, dialog and time line suites are not offered
}

} // namespace

// ---- Parameter kinds, sets and effects ----------------------------------------------------------------------------

ParamKind paramKindOf(std::string_view t) {
    if (t == kOfxParamTypeDouble) {
        return ParamKind::Double;
    }
    if (t == kOfxParamTypeInteger) {
        return ParamKind::Int;
    }
    if (t == kOfxParamTypeBoolean) {
        return ParamKind::Bool;
    }
    if (t == kOfxParamTypeChoice) {
        return ParamKind::Choice;
    }
    if (t == kOfxParamTypeStrChoice) {
        return ParamKind::StrChoice;
    }
    if (t == kOfxParamTypeRGBA) {
        return ParamKind::Rgba;
    }
    if (t == kOfxParamTypeRGB) {
        return ParamKind::Rgb;
    }
    if (t == kOfxParamTypeDouble2D) {
        return ParamKind::Double2D;
    }
    if (t == kOfxParamTypeInteger2D) {
        return ParamKind::Int2D;
    }
    if (t == kOfxParamTypeDouble3D) {
        return ParamKind::Double3D;
    }
    if (t == kOfxParamTypeInteger3D) {
        return ParamKind::Int3D;
    }
    if (t == kOfxParamTypeString) {
        return ParamKind::String;
    }
    if (t == kOfxParamTypeCustom) {
        return ParamKind::Custom;
    }
    if (t == kOfxParamTypeBytes) {
        return ParamKind::Bytes;
    }
    if (t == kOfxParamTypeGroup) {
        return ParamKind::Group;
    }
    if (t == kOfxParamTypePage) {
        return ParamKind::Page;
    }
    if (t == kOfxParamTypePushButton) {
        return ParamKind::PushButton;
    }
    if (t == "OfxParamTypeParametric") {
        return ParamKind::Parametric;
    }
    return ParamKind::Unknown;
}

std::string_view paramKindName(ParamKind kind) {
    switch (kind) {
    case ParamKind::Double:
        return "double";
    case ParamKind::Int:
        return "int";
    case ParamKind::Bool:
        return "bool";
    case ParamKind::Choice:
        return "choice";
    case ParamKind::StrChoice:
        return "strchoice";
    case ParamKind::Rgba:
        return "rgba";
    case ParamKind::Rgb:
        return "rgb";
    case ParamKind::Double2D:
        return "double2d";
    case ParamKind::Int2D:
        return "int2d";
    case ParamKind::Double3D:
        return "double3d";
    case ParamKind::Int3D:
        return "int3d";
    case ParamKind::String:
        return "string";
    case ParamKind::Custom:
        return "custom";
    case ParamKind::Bytes:
        return "bytes";
    case ParamKind::Group:
        return "group";
    case ParamKind::Page:
        return "page";
    case ParamKind::PushButton:
        return "pushbutton";
    case ParamKind::Parametric:
        return "parametric";
    case ParamKind::Unknown:
        break;
    }
    return "unknown";
}

int paramDimension(ParamKind kind) {
    switch (kind) {
    case ParamKind::Double:
    case ParamKind::Int:
    case ParamKind::Bool:
    case ParamKind::Choice:
        return 1;
    case ParamKind::Rgba:
        return 4;
    case ParamKind::Rgb:
    case ParamKind::Double3D:
    case ParamKind::Int3D:
        return 3;
    case ParamKind::Double2D:
    case ParamKind::Int2D:
        return 2;
    default:
        return 0;
    }
}

bool paramIsNumeric(ParamKind kind) { return paramDimension(kind) > 0; }

ParamDef* ParamSet::find(std::string_view name) const {
    for (const auto& p : params) {
        if (p->name == name) {
            return p.get();
        }
    }
    return nullptr;
}

Clip* Effect::find(std::string_view name) const {
    for (const auto& c : clips) {
        if (c->name == name) {
            return c.get();
        }
    }
    return nullptr;
}

const OfxHost* hostStruct() {
    static const OfxHost host = {&hostProperties(), fetchSuite};
    return &host;
}

void setLogPrefix(const char* prefix) { gLogPrefix = prefix; }

} // namespace wabcut::ofxhost
