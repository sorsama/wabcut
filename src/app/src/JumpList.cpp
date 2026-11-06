#include "JumpList.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QStandardPaths>
#include <QVariantMap>

#include <algorithm>
#include <set>

#ifdef _WIN32
#include <cstring>
#include <cwctype>
#include <windows.h>
#include <objbase.h>
#include <propsys.h>
#include <shobjidl.h>
#include <wrl/client.h>
#endif

namespace wabcut::app {

namespace {

const QString kEnabledKey = QStringLiteral("jumpList/enabled");
constexpr int kLimit = 10;
constexpr int kDebounceMs = 1000;

/// A path as Windows compares them: separators and case do not matter there.
QString comparable(const QString& path) {
    QString clean = QDir::cleanPath(path);
#ifdef _WIN32
    clean = clean.toLower();
#endif
    return clean;
}

} // namespace

std::vector<JumpListEntry> jumpListEntries(const QStringList& recent, const QStringList& starred, int limit,
                                           const std::function<bool(const QString&)>& exists) {
    std::vector<JumpListEntry> out;
    std::set<QString> seen;
    const auto add = [&](const QString& raw, bool isStarred) {
        if (limit <= 0 || static_cast<int>(out.size()) >= limit || raw.trimmed().isEmpty())
            return;
        const QFileInfo info(raw);
        const QString absolute = info.absoluteFilePath();
        if (!seen.insert(comparable(absolute)).second)
            return;
        if (exists ? !exists(absolute) : !info.isFile())
            return;
        JumpListEntry entry;
        entry.title = info.completeBaseName();
        entry.path = QDir::toNativeSeparators(absolute);
        entry.tooltip = entry.path;
        entry.starred = isStarred;
        out.push_back(std::move(entry));
    };
    for (const QString& path : starred)
        add(path, true);
    for (const QString& path : recent)
        add(path, false);
    // Two projects with one name: their folder tells them apart (all the twins are found first, then renamed).
    std::vector<bool> twin(out.size(), false);
    for (std::size_t i = 0; i < out.size(); ++i) {
        for (std::size_t k = 0; k < out.size(); ++k) {
            if (i != k && out[i].title.compare(out[k].title, Qt::CaseInsensitive) == 0)
                twin[i] = true;
        }
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (!twin[i])
            continue;
        const QString folder = QFileInfo(out[i].path).dir().dirName();
        if (!folder.isEmpty())
            out[i].title = QStringLiteral("%1 (%2)").arg(out[i].title, folder);
    }
    return out;
}

#ifdef _WIN32

namespace {

using Microsoft::WRL::ComPtr;

std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return text;
}

/// The Title property of a shell item (System.Title, {F29F85E0-4FF9-1068-AB91-08002B27B3D9}, 2).
bool setTitle(IPropertyStore* store, const std::wstring& title) {
    const PROPERTYKEY key{{0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}, 2};
    PROPVARIANT value;
    PropVariantInit(&value);
    const std::size_t bytes = (title.size() + 1) * sizeof(wchar_t);
    value.vt = VT_LPWSTR;
    value.pwszVal = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (!value.pwszVal)
        return false;
    std::memcpy(value.pwszVal, title.c_str(), bytes);
    const bool ok = SUCCEEDED(store->SetValue(key, value)) && SUCCEEDED(store->Commit());
    PropVariantClear(&value);
    return ok;
}

ComPtr<IShellLinkW> makeLink(const std::wstring& exe, const JumpListEntry& entry) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return {};
    // The project's path, quoted: wabcut.exe "<project>" - SingleInstance hands it to the running Wabcut.
    const std::wstring arguments = L"\"" + entry.path.toStdWString() + L"\"";
    if (FAILED(link->SetPath(exe.c_str())) || FAILED(link->SetArguments(arguments.c_str())) ||
        FAILED(link->SetIconLocation(exe.c_str(), 0)))
        return {};
    link->SetDescription(entry.tooltip.toStdWString().c_str());
    ComPtr<IPropertyStore> store;
    if (FAILED(link.As(&store)) || !setTitle(store.Get(), entry.title.toStdWString()))
        return {};
    return link;
}

} // namespace

bool writeWindowsJumpList(const std::vector<JumpListEntry>& entries) {
    ComPtr<ICustomDestinationList> list;
    if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list))))
        return false;
    UINT minSlots = 0; // (not "slots": a Qt keyword macro)
    ComPtr<IObjectArray> removed;
    if (FAILED(list->BeginList(&minSlots, IID_PPV_ARGS(&removed))))
        return false;
    // Items the user took off the list stay off (AppendCategory refuses them, and they should not come back).
    std::set<std::wstring> removedArguments;
    UINT removedCount = 0;
    if (removed && SUCCEEDED(removed->GetCount(&removedCount))) {
        for (UINT i = 0; i < removedCount; ++i) {
            ComPtr<IShellLinkW> link;
            wchar_t arguments[2048]{};
            if (SUCCEEDED(removed->GetAt(i, IID_PPV_ARGS(&link))) && SUCCEEDED(link->GetArguments(arguments, 2048)))
                removedArguments.insert(lower(arguments));
        }
    }
    const std::wstring exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
    bool ok = true;
    for (const bool starred : {true, false}) {
        ComPtr<IObjectCollection> collection;
        if (FAILED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&collection)))) {
            ok = false;
            break;
        }
        int added = 0;
        for (const JumpListEntry& entry : entries) {
            if (entry.starred != starred)
                continue;
            if (removedArguments.contains(lower(L"\"" + entry.path.toStdWString() + L"\"")))
                continue;
            if (const ComPtr<IShellLinkW> link = makeLink(exe, entry); link && SUCCEEDED(collection->AddObject(link.Get())))
                ++added;
        }
        if (added == 0)
            continue;
        ComPtr<IObjectArray> array;
        const std::wstring name = QCoreApplication::translate("wabcut::app::JumpList", starred ? QT_TRANSLATE_NOOP("wabcut::app::JumpList", "Starred") : QT_TRANSLATE_NOOP("wabcut::app::JumpList", "Recent projects")).toStdWString();
        if (FAILED(collection.As(&array)) || FAILED(list->AppendCategory(name.c_str(), array.Get()))) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        list->AbortList();
        return false;
    }
    return SUCCEEDED(list->CommitList());
}

bool deleteWindowsJumpList() {
    ComPtr<ICustomDestinationList> list;
    if (FAILED(CoCreateInstance(CLSID_DestinationList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list))))
        return false;
    return SUCCEEDED(list->DeleteList(nullptr));
}

#endif

JumpList::JumpList(QObject* parent) : QObject(parent) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, [this] { refreshNow(); });
    m_backend.write = [](const std::vector<JumpListEntry>& entries) {
#ifdef _WIN32
        // The tests run in test mode: they never touch the real taskbar.
        return !QStandardPaths::isTestModeEnabled() && writeWindowsJumpList(entries);
#else
        Q_UNUSED(entries);
        return false;
#endif
    };
    m_backend.clear = [] {
#ifdef _WIN32
        return !QStandardPaths::isTestModeEnabled() && deleteWindowsJumpList();
#else
        return false;
#endif
    };
}

bool JumpList::available() const {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

bool JumpList::enabled() const {
    return QSettings().value(kEnabledKey, true).toBool();
}

void JumpList::setEnabled(bool on) {
    if (on == enabled())
        return;
    QSettings().setValue(kEnabledKey, on);
    emit enabledChanged();
    refreshNow();
}

void JumpList::refresh() {
    m_debounce.start();
}

bool JumpList::refreshNow() {
    m_debounce.stop();
    if (!enabled()) {
        m_last.clear();
        return m_backend.clear ? m_backend.clear() : false;
    }
    std::vector<JumpListEntry> entries = jumpListEntries(storedRecent(), storedStarred(), kLimit);
    if (m_backend.write && m_backend.write(entries)) {
        m_last = std::move(entries);
        return true;
    }
    m_last.clear();
    return false;
}

QStringList JumpList::storedRecent() {
    QStringList out;
    for (const QVariant& entry : QSettings().value(QStringLiteral("home/recentProjects")).toList()) {
        const QString path = entry.toMap().value(QStringLiteral("path")).toString();
        if (!path.isEmpty())
            out << path;
    }
    return out;
}

QStringList JumpList::storedStarred() {
    // Preferences stores the starred projects as a JSON list in a string (home/starred).
    const QVariant value = QSettings().value(QStringLiteral("home/starred"));
    QStringList out;
    if (value.typeId() == QMetaType::QStringList) {
        out = value.toStringList();
    } else {
        for (const QJsonValue& entry : QJsonDocument::fromJson(value.toString().toUtf8()).array()) {
            if (entry.isString() && !entry.toString().isEmpty())
                out << entry.toString();
        }
    }
    out.removeAll(QString());
    return out;
}

} // namespace wabcut::app
