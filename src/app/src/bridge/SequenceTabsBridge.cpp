#include "SequenceTabsBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"

#include <wabcut/model/SequenceSettings.h>

#include <QQmlEngine>

#include <algorithm>

namespace wabcut::app {

SequenceTabsBridge::SequenceTabsBridge(QObject* parent) : QObject(parent) {}

SequenceTabsBridge* SequenceTabsBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new SequenceTabsBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void SequenceTabsBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    m_tabs.clear();
    m_playheads.clear();
    m_current = {};
    if (controller) {
        connect(controller, &ProjectController::sequencesChanged, this, &SequenceTabsBridge::onSequencesChanged);
        connect(controller, &ProjectController::sequenceChanged, this, &SequenceTabsBridge::onSequenceChanged);
        connect(controller, &ProjectController::playheadChanged, this, &SequenceTabsBridge::onPlayheadChanged);
        onSequenceChanged();
    }
    emit tabsChanged();
}

int SequenceTabsBridge::tabIndex(SequenceId id) const {
    const auto it = std::ranges::find(m_tabs, id);
    return it == m_tabs.end() ? -1 : static_cast<int>(it - m_tabs.begin());
}

QVariantList SequenceTabsBridge::tabs() const {
    QVariantList list;
    if (!m_controller)
        return list;
    const ProjectSnapshot snap = m_controller->snapshot();
    for (const SequenceId id : m_tabs) {
        const Sequence* seq = snap ? snap->findSequence(id) : nullptr;
        if (!seq)
            continue;
        list << QVariantMap{{QStringLiteral("id"), sequenceKey(id)},
                            {QStringLiteral("name"), QString::fromStdString(seq->name)},
                            {QStringLiteral("current"), id == m_controller->currentSequenceId()},
                            {QStringLiteral("label"), seq->colorIndex}};
    }
    return list;
}

QString SequenceTabsBridge::current() const {
    return m_controller ? sequenceKey(m_controller->currentSequenceId()) : QString();
}

QVariantList SequenceTabsBridge::sequences() const {
    QVariantList list;
    if (!m_controller)
        return list;
    const ProjectSnapshot snap = m_controller->snapshot();
    if (!snap)
        return list;
    for (const SequencePtr& seq : snap->sequences) {
        list << QVariantMap{{QStringLiteral("id"), sequenceKey(seq->id)},
                            {QStringLiteral("name"), QString::fromStdString(seq->name)},
                            {QStringLiteral("durationText"), m_controller->formatTimecode(seq->duration)},
                            {QStringLiteral("open"), tabIndex(seq->id) >= 0},
                            {QStringLiteral("label"), seq->colorIndex}};
    }
    return list;
}

bool SequenceTabsBridge::switchTo(SequenceId id) {
    if (!m_controller)
        return false;
    const ProjectSnapshot snap = m_controller->snapshot();
    if (!snap)
        return false;
    for (std::size_t i = 0; i < snap->sequences.size(); ++i) {
        if (snap->sequences[i]->id == id) {
            m_controller->setCurrentSequenceIndex(static_cast<int>(i)); // onSequenceChanged restores the playhead
            return true;
        }
    }
    return false;
}

bool SequenceTabsBridge::open(const QString& sequenceId) {
    const auto id = parseSequenceKey(sequenceId);
    if (!id || !m_controller || !m_controller->snapshot()->findSequence(*id))
        return false;
    if (tabIndex(*id) < 0) {
        const int at = tabIndex(m_controller->currentSequenceId());
        m_tabs.insert(at < 0 ? m_tabs.end() : m_tabs.begin() + at + 1, *id);
        emit tabsChanged();
    }
    return switchTo(*id);
}

bool SequenceTabsBridge::activate(const QString& sequenceId) {
    const auto id = parseSequenceKey(sequenceId);
    return id && tabIndex(*id) >= 0 && switchTo(*id);
}

bool SequenceTabsBridge::close(const QString& sequenceId) {
    const auto id = parseSequenceKey(sequenceId);
    const int index = id ? tabIndex(*id) : -1;
    if (index < 0 || m_tabs.size() <= 1 || !m_controller)
        return false;
    const bool wasCurrent = *id == m_controller->currentSequenceId();
    m_tabs.erase(m_tabs.begin() + index);
    m_playheads.erase(id->value);
    if (wasCurrent)
        switchTo(m_tabs[static_cast<std::size_t>(std::min(index, static_cast<int>(m_tabs.size()) - 1))]);
    emit tabsChanged();
    return true;
}

bool SequenceTabsBridge::setLabel(const QString& sequenceId, int colorIndex) {
    const auto id = parseSequenceKey(sequenceId);
    if (!id || !m_controller || colorIndex < 0 || colorIndex > 15)
        return false;
    const SequenceId seq = *id;
    edit::SequenceSettingsChange change;
    change.colorIndex = colorIndex;
    return m_controller->applyEditFn(QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Label"),
                                     [&](const ProjectState& state) { return edit::setSequenceSettings(state, seq, change); });
}

void SequenceTabsBridge::move(int from, int to) {
    const int n = static_cast<int>(m_tabs.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to)
        return;
    const SequenceId id = m_tabs[static_cast<std::size_t>(from)];
    m_tabs.erase(m_tabs.begin() + from);
    m_tabs.insert(m_tabs.begin() + to, id);
    emit tabsChanged();
}

void SequenceTabsBridge::cycle(int delta) {
    const int n = static_cast<int>(m_tabs.size());
    if (n < 2 || !m_controller)
        return;
    const int at = std::max(0, tabIndex(m_controller->currentSequenceId()));
    switchTo(m_tabs[static_cast<std::size_t>(((at + delta) % n + n) % n)]);
}

void SequenceTabsBridge::onSequencesChanged() {
    if (!m_controller)
        return;
    // Sequences that are gone (deleted, another project) lose their tabs.
    const ProjectSnapshot snap = m_controller->snapshot();
    std::erase_if(m_tabs, [&](SequenceId id) { return !snap || !snap->findSequence(id); });
    for (auto it = m_playheads.begin(); it != m_playheads.end();) {
        if (!snap || !snap->findSequence(SequenceId{it->first}))
            it = m_playheads.erase(it);
        else
            ++it;
    }
    onSequenceChanged();
    emit tabsChanged(); // names may have changed
}

void SequenceTabsBridge::onSequenceChanged() {
    if (!m_controller)
        return;
    const SequenceId now = m_controller->currentSequenceId();
    if (!now || now == m_current) {
        if (now && tabIndex(now) < 0) {
            m_tabs.push_back(now);
            emit tabsChanged();
        }
        return;
    }
    m_current = now;
    if (tabIndex(now) < 0) {
        // Switched from elsewhere (the sequence menu, New Sequence, a new project): it gets a tab.
        m_tabs.push_back(now);
    }
    if (const auto saved = m_playheads.find(now.value); saved != m_playheads.end())
        m_controller->setPlayhead(saved->second);
    emit tabsChanged();
}

void SequenceTabsBridge::onPlayheadChanged() {
    if (m_controller && m_current && m_controller->currentSequenceId() == m_current)
        m_playheads[m_current.value] = m_controller->finePlayhead(); // to the sample with audio time units (0.2)
}

} // namespace wabcut::app
