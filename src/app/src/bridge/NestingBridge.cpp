#include "NestingBridge.h"

#include "BridgeIds.h"
#include "CommandJob.h"
#include "LibraryBridge.h"
#include "ProjectController.h"
#include "SequenceTabsBridge.h"

#include <QQmlEngine>

#include <algorithm>

namespace wabcut::app {

using command::Json;

namespace {
QString text(const std::string& s) { return QString::fromStdString(s); }

/// The selected clips that are in the current sequence.
std::vector<ClipId> selectedHere(const ProjectController& controller) {
    std::vector<ClipId> out;
    const SequencePtr seq = controller.currentSequence();
    for (const ClipId id : controller.selection()) {
        if (seq && seq->findClip(id))
            out.push_back(id);
    }
    return out;
}
} // namespace

NestingBridge::NestingBridge(QObject* parent) : QObject(parent) {}

NestingBridge* NestingBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new NestingBridge;
    if (qmlEngine) {
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
        bridge->setTabs(qmlEngine->singletonInstance<SequenceTabsBridge*>("Wabcut", "SequenceTabs"));
        bridge->setLibrary(qmlEngine->singletonInstance<LibraryBridge*>("Wabcut", "Library"));
    }
    return bridge;
}

void NestingBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    m_openedFrom.clear();
    if (controller) {
        connect(controller, &ProjectController::sequenceChanged, this, &NestingBridge::breadcrumbChanged);
        connect(controller, &ProjectController::revisionChanged, this, &NestingBridge::breadcrumbChanged); // renames
        connect(controller, &ProjectController::changed, this, [this](const QVariantMap& change) {
            if (change.value(QStringLiteral("kind")).toString() == QStringLiteral("reset")) {
                m_openedFrom.clear();
                emit breadcrumbChanged();
            }
        });
    }
    emit breadcrumbChanged();
}

void NestingBridge::setTabs(SequenceTabsBridge* tabs) { m_tabs = tabs; }

void NestingBridge::setLibrary(LibraryBridge* library) { m_library = library; }

void NestingBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged();
}

QVariantList NestingBridge::breadcrumb() const {
    QVariantList out;
    if (!m_controller)
        return out;
    const ProjectSnapshot snap = m_controller->snapshot();
    SequenceId at = m_controller->currentSequenceId();
    std::vector<SequenceId> chain{at};
    for (std::size_t guard = 0; guard < 64; ++guard) {
        const auto it = m_openedFrom.find(at.value);
        if (it == m_openedFrom.end() || !snap->findSequence(it->second) ||
            std::find(chain.begin(), chain.end(), it->second) != chain.end())
            break;
        at = it->second;
        chain.push_back(at);
    }
    if (chain.size() < 2)
        return out; // not opened from a nested clip
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const Sequence* s = snap->findSequence(*it);
        if (!s)
            continue;
        out.append(QVariantMap{{QStringLiteral("key"), sequenceKey(*it)},
                               {QStringLiteral("name"), text(s->name)},
                               {QStringLiteral("current"), *it == m_controller->currentSequenceId()}});
    }
    return out;
}

QString NestingBridge::defaultName() const {
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    for (int n = 1; n < 1000; ++n) {
        const QString candidate = tr("Nested Sequence %1").arg(n, 2, 10, QLatin1Char('0'));
        const bool taken = snap && std::any_of(snap->sequences.begin(), snap->sequences.end(),
                                               [&](const SequencePtr& s) { return text(s->name) == candidate; });
        if (!taken)
            return candidate;
    }
    return tr("Nested Sequence");
}

bool NestingBridge::canNest() const { return m_controller && !selectedHere(*m_controller).empty(); }

QString NestingBridge::nestSelection(const QString& name) {
    if (!canNest())
        return tr("Select the clips to nest first");
    Json clips = Json::array();
    for (const ClipId id : selectedHere(*m_controller))
        clips.push_back(id.value);
    Json params{{"clips", clips}, {"linked", true}};
    const QString chosen = name.trimmed().isEmpty() ? defaultName() : name.trimmed();
    params["name"] = chosen.toStdString();
    const command::CommandResult result = CommandJob::runNow(m_controller, nullptr, "sequence.nest", params);
    setMessage(result.ok() ? tr("Nested into “%1”").arg(chosen) : commandErrorText(result));
    return commandErrorText(result);
}

QVariantMap NestingBridge::clipInfo(const QString& key) const {
    const auto id = parseClipKey(key);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    if (!loc)
        return {};
    const Clip& c = loc->clip();
    QVariantMap out{{QStringLiteral("nested"), c.isNested()}, {QStringLiteral("multicam"), false}};
    if (!c.isNested())
        return out;
    const Sequence* nested = m_controller->snapshot()->findSequence(c.sequence);
    out.insert(QStringLiteral("sequence"), sequenceKey(c.sequence));
    out.insert(QStringLiteral("name"), nested ? text(nested->name) : QString());
    if (nested && nested->multicam) {
        out.insert(QStringLiteral("multicam"), true);
        const std::size_t angle = c.angles.first();
        if (angle < nested->multicam->angles.size()) {
            out.insert(QStringLiteral("angle"), text(nested->multicam->angles[angle].name));
            out.insert(QStringLiteral("angleNumber"), static_cast<int>(angle) + 1);
        }
    }
    return out;
}

bool NestingBridge::openNested(const QString& key) {
    const auto id = parseClipKey(key);
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const auto loc = id && seq ? seq->findClip(*id) : std::nullopt;
    if (!loc || !loc->clip().isNested() || !m_tabs) {
        setMessage(tr("Only a nested or multi-camera clip opens as a sequence"));
        return false;
    }
    const Clip& c = loc->clip();
    const ProjectSnapshot snap = m_controller->snapshot();
    const Sequence* nested = snap->findSequence(c.sequence);
    if (!nested) {
        setMessage(tr("The clip's sequence is gone"));
        return false;
    }
    // The frame the clip shows at the playhead (its start when the playhead is elsewhere).
    const Ticks playhead = m_controller->playhead();
    const Ticks inside = playhead >= c.start() && playhead < c.end() ? playhead : c.start();
    const Ticks nestedTime = std::clamp(c.sourceTimeAt(inside), Ticks{}, std::max(Ticks{}, nested->duration - Ticks{1}));
    const SequenceId parent = seq->id;
    const SequenceId target = c.sequence;
    if (!m_tabs->open(sequenceKey(target)))
        return false;
    m_openedFrom[target.value] = parent;
    m_controller->setPlayhead(nestedTime);
    emit breadcrumbChanged();
    return true;
}

bool NestingBridge::goTo(const QString& sequenceKey) {
    return m_tabs && m_tabs->open(sequenceKey);
}

QString NestingBridge::unnest(const QString& key) {
    const auto id = parseClipKey(key);
    if (!id)
        return tr("Choose a nested clip");
    const command::CommandResult result = CommandJob::runNow(m_controller, nullptr, "sequence.unnest", Json{{"clip", id->value}});
    setMessage(commandErrorText(result));
    return commandErrorText(result);
}

bool NestingBridge::revealInLibrary(const QString& key) {
    const QVariantMap info = clipInfo(key);
    const QString sequence = info.value(QStringLiteral("sequence")).toString();
    if (sequence.isEmpty() || !m_library)
        return false;
    emit libraryRequested();
    return m_library->showItem(sequence);
}

} // namespace wabcut::app
