#include "ClipboardBridge.h"

#include "BridgeIds.h"
#include "ProjectController.h"

#include <wabcut/command/Command.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/Clipboard.h>
#include <wabcut/model/Effects.h>

#include <QCoreApplication>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantList>

#include <algorithm>
#include <map>
#include <optional>
#include <string>

namespace wabcut::app {

namespace {

QString effectTitle(const EffectInstance& effect) {
    const EffectDescriptor* d = EffectRegistry::builtin().find(effect.effect);
    if (!d)
        return QString::fromStdString(effect.effect);
    // The names Effect Controls shows (their translation context).
    const std::string name(d->displayName);
    return QCoreApplication::translate("wabcut::app::EffectControlsModel", name.c_str());
}

QString clipTitle(const ProjectState& state, const Clip& clip) {
    if (!clip.name().empty())
        return QString::fromStdString(clip.name());
    if (const MediaItem* media = clip.media.isValid() ? state.findMedia(clip.media) : nullptr)
        return QString::fromStdString(media->name);
    if (const Sequence* nested = clip.isNested() ? state.findSequence(clip.sequence) : nullptr)
        return QString::fromStdString(nested->name);
    return QStringLiteral("c%1").arg(clip.id.value);
}

bool isVideo(const Clip& clip) {
    return clip.stream.kind == StreamKind::Video;
}

/// The clipboard's first clip of a kind: the one clip.pasteAttributes takes that kind's attributes from.
const Clip* firstClipOf(const ClipboardDocument& document, TrackKind kind) {
    for (const TrackPtr& track : document.sequence().tracks(kind)) {
        if (!track->clips.empty())
            return &track->clips[0];
    }
    return nullptr;
}

/// The selected clips of the current sequence (`seq` keeps them alive), in selection order.
std::vector<const Clip*> selectedClips(const ProjectController& controller, const Sequence& seq) {
    std::vector<const Clip*> clips;
    for (const ClipId id : controller.selection()) {
        if (const auto loc = seq.findClip(id))
            clips.push_back(&loc->clip());
    }
    return clips;
}

std::vector<QString> stringList(const QVariant& value) {
    std::vector<QString> out;
    for (const QVariant& v : value.toList())
        out.push_back(v.toString());
    return out;
}

QString errorOf(const command::CommandResult& result) {
    return result.error ? QString::fromStdString(result.error->message) : QString();
}

QStringList strings(const command::Json& list) {
    QStringList out;
    if (list.is_array()) {
        for (const command::Json& s : list)
            out << QString::fromStdString(s.get<std::string>());
    }
    return out;
}

command::CommandBatch batchOf(std::vector<command::CommandRequest> commands, const char* label, bool dryRun) {
    command::CommandBatch batch;
    batch.commands = std::move(commands);
    batch.label = label;
    batch.dryRun = dryRun;
    return batch;
}

} // namespace

ClipboardBridge::Plan::Plan() = default;
ClipboardBridge::Plan::~Plan() = default;
ClipboardBridge::Plan::Plan(Plan&&) noexcept = default;
ClipboardBridge::Plan& ClipboardBridge::Plan::operator=(Plan&&) noexcept = default;

ClipboardBridge::ClipboardBridge(QObject* parent) : QObject(parent) {}

ClipboardBridge* ClipboardBridge::create(QQmlEngine* qmlEngine, QJSEngine*) {
    auto* bridge = new ClipboardBridge;
    if (qmlEngine)
        bridge->setController(qmlEngine->singletonInstance<ProjectController*>("Wabcut", "ProjectController"));
    return bridge;
}

void ClipboardBridge::setController(ProjectController* controller) {
    if (m_controller == controller)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    m_controller = controller;
    if (controller) {
        connect(controller, &ProjectController::clipboardChanged, this, &ClipboardBridge::availabilityChanged);
        connect(controller, &ProjectController::selectionChanged, this, &ClipboardBridge::availabilityChanged);
        connect(controller, &ProjectController::sequenceChanged, this, &ClipboardBridge::availabilityChanged);
        connect(controller, &ProjectController::clipboardNotice, this, &ClipboardBridge::setMessage);
    }
    emit availabilityChanged();
}

bool ClipboardBridge::canPaste() const {
    return m_controller && m_controller->canPaste();
}

bool ClipboardBridge::canRemoveAttributes() const {
    return m_controller && !m_controller->selection().empty() && m_controller->currentSequence();
}

bool ClipboardBridge::canPasteAttributes() const {
    return canRemoveAttributes() && m_controller->canPaste();
}

void ClipboardBridge::setMessage(const QString& message) {
    m_message = message;
    emit messageChanged(); // every message, also one that repeats the last
}

bool ClipboardBridge::openAttributes(const QString& mode) {
    const bool paste = mode != QLatin1String("remove");
    if (paste && !canPasteAttributes()) {
        setMessage(canPaste() ? tr("Select the clips to paste the attributes onto first")
                              : tr("Copy a clip first, then select the clips to paste its attributes onto"));
        return false;
    }
    if (!paste && !canRemoveAttributes()) {
        setMessage(tr("Select the clips to remove attributes from first"));
        return false;
    }
    emit attributesRequested(paste ? QStringLiteral("paste") : QStringLiteral("remove"));
    return true;
}

QVariantMap ClipboardBridge::attributesInfo(const QString& mode) {
    const bool paste = mode != QLatin1String("remove");
    QVariantMap info{{QStringLiteral("mode"), paste ? QStringLiteral("paste") : QStringLiteral("remove")},
                     {QStringLiteral("available"), false},
                     {QStringLiteral("targets"), 0}};
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq) {
        info[QStringLiteral("reason")] = tr("Open a sequence first");
        return info;
    }
    const std::vector<const Clip*> targets = selectedClips(*m_controller, *seq);
    const bool videoTargets = std::ranges::any_of(targets, [](const Clip* c) { return isVideo(*c); });
    const bool audioTargets = std::ranges::any_of(targets, [](const Clip* c) { return !isVideo(*c); });
    info[QStringLiteral("targets")] = static_cast<int>(targets.size());

    QVariantMap sections[2]; // video, audio
    std::optional<ClipboardDocument> document;
    if (paste) {
        m_controller->adoptSystemClipboard();
        document = m_controller->clipboardServices()->get();
    }
    for (int k = 0; k < 2; ++k) {
        const bool video = k == 0;
        const bool selected = video ? videoTargets : audioTargets;
        QVariantMap& section = sections[k];
        QVariantList effects;
        QString reason = selected ? QString() : video ? tr("No video clip is selected") : tr("No audio clip is selected");
        if (paste) {
            const Clip* source = document ? firstClipOf(*document, video ? TrackKind::Video : TrackKind::Audio) : nullptr;
            if (!source)
                reason = video ? tr("The clipboard has no video clip") : tr("The clipboard has no audio clip");
            if (source) {
                section[QStringLiteral("source")] = clipTitle(document->content, *source);
                for (std::size_t i = 0; i < source->effects().size(); ++i)
                    effects.append(QVariantMap{{QStringLiteral("key"), QString::number(i)},
                                               {QStringLiteral("name"), effectTitle(source->effects()[i])}});
            }
        } else {
            // The selection's own effects of this kind, once per effect id.
            std::vector<std::string> seen;
            for (const Clip* clip : targets) {
                if (isVideo(*clip) != video)
                    continue;
                for (const EffectInstance& e : clip->effects()) {
                    if (std::ranges::find(seen, e.effect) != seen.end())
                        continue;
                    seen.push_back(e.effect);
                    effects.append(QVariantMap{{QStringLiteral("key"), QString::fromStdString(e.effect)},
                                               {QStringLiteral("name"), effectTitle(e)}});
                }
            }
        }
        section[QStringLiteral("available")] = reason.isEmpty();
        section[QStringLiteral("reason")] = reason;
        section[QStringLiteral("effects")] = effects;
    }
    info[QStringLiteral("video")] = sections[0];
    info[QStringLiteral("audio")] = sections[1];
    if (paste) {
        const QString source = sections[0].value(QStringLiteral("source")).toString();
        info[QStringLiteral("source")] = source.isEmpty() ? sections[1].value(QStringLiteral("source")).toString() : source;
    }
    const bool any = sections[0].value(QStringLiteral("available")).toBool() ||
                     sections[1].value(QStringLiteral("available")).toBool();
    QString reason;
    if (targets.empty())
        reason = paste ? tr("Select the clips to paste the attributes onto") : tr("Select the clips to remove attributes from");
    else if (paste && !document)
        reason = tr("The clipboard is empty: copy a clip first");
    else if (!any)
        reason = paste ? tr("The clipboard has no clip of the selected clips' kind") : tr("Nothing to remove");
    info[QStringLiteral("available")] = reason.isEmpty();
    info[QStringLiteral("reason")] = reason;
    return info;
}

ClipboardBridge::Plan ClipboardBridge::plan(bool paste, const QVariantMap& choices) const {
    Plan p;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    if (!seq) {
        p.error = tr("Open a sequence first");
        return p;
    }
    const std::vector<const Clip*> targets = selectedClips(*m_controller, *seq);
    for (const Clip* clip : targets)
        p.order.push_back(clip->id);
    if (targets.empty()) {
        p.error = paste ? tr("Select the clips to paste the attributes onto") : tr("Select the clips to remove attributes from");
        return p;
    }
    const auto flag = [&](command::Json& attributes, const char* key) {
        if (choices.value(QString::fromLatin1(key)).toBool())
            attributes[key] = true;
    };
    const auto clipIds = [](const std::vector<ClipId>& ids) {
        command::Json out = command::Json::array();
        for (const ClipId id : ids)
            out.push_back(id.value);
        return out;
    };

    if (!paste) {
        command::Json attributes = command::Json::object();
        for (const char* key : {"motion", "opacity", "timeRemap", "volume", "panner", "allEffects"})
            flag(attributes, key);
        command::Json effects = command::Json::array();
        for (const char* key : {"videoEffects", "audioEffects"}) {
            for (const QString& id : stringList(choices.value(QString::fromLatin1(key)))) {
                const command::Json effect = id.toStdString();
                if (std::find(effects.begin(), effects.end(), effect) == effects.end())
                    effects.push_back(effect);
            }
        }
        if (!effects.empty() && !attributes.contains("allEffects"))
            attributes["effects"] = std::move(effects);
        if (attributes.empty()) {
            p.error = tr("Choose the attributes to remove");
            return p;
        }
        command::CommandRequest request;
        request.command = "clip.removeAttributes";
        request.params = command::Json{{"clips", clipIds(p.order)}, {"attributes", std::move(attributes)}};
        p.parts.push_back(std::move(request));
        p.partClips.push_back(p.order);
        return p;
    }

    const std::optional<ClipboardDocument> document = m_controller->clipboardServices()->get();
    if (!document) {
        p.error = tr("The clipboard is empty: copy a clip first");
        return p;
    }
    for (const bool video : {true, false}) {
        std::vector<ClipId> group;
        for (const Clip* clip : targets) {
            if (isVideo(*clip) == video)
                group.push_back(clip->id);
        }
        if (group.empty())
            continue;
        const Clip* source = firstClipOf(*document, video ? TrackKind::Video : TrackKind::Audio);
        if (!source) {
            for (const ClipId id : group)
                p.left.emplace_back(id, video ? tr("the clipboard has no video clip") : tr("the clipboard has no audio clip"));
            continue;
        }
        command::Json attributes = command::Json::object();
        if (video) {
            for (const char* key : {"motion", "opacity", "timeRemap"})
                flag(attributes, key);
        } else {
            for (const char* key : {"volume", "panner"})
                flag(attributes, key);
        }
        command::Json effects = command::Json::array();
        for (const QString& key : stringList(choices.value(video ? QStringLiteral("videoEffects") : QStringLiteral("audioEffects")))) {
            bool ok = false;
            const int index = key.toInt(&ok);
            if (ok && index >= 0 && static_cast<std::size_t>(index) < source->effects().size())
                effects.push_back(index);
        }
        if (!effects.empty())
            attributes["effects"] = std::move(effects);
        if (attributes.empty()) {
            for (const ClipId id : group)
                p.left.emplace_back(id, video ? tr("no video attributes chosen") : tr("no audio attributes chosen"));
            continue;
        }
        command::CommandRequest request;
        request.command = "clip.pasteAttributes";
        request.params = command::Json{{"clips", clipIds(group)},
                                       {"attributes", std::move(attributes)},
                                       {"scaleTimes", choices.value(QStringLiteral("scaleTimes")).toBool()}};
        p.parts.push_back(std::move(request));
        p.partClips.push_back(std::move(group));
    }
    if (p.parts.empty())
        p.error = tr("Choose the attributes to paste");
    return p;
}

QVariantMap ClipboardBridge::previewAttributes(const QString& mode, const QVariantMap& choices) {
    const bool paste = mode != QLatin1String("remove");
    const Plan p = plan(paste, choices);
    struct Row {
        QStringList applied;
        QStringList skipped;
    };
    std::map<std::uint64_t, Row> rows;
    bool any = false;
    QString firstError;
    for (std::size_t i = 0; i < p.parts.size(); ++i) {
        const command::CommandResult result = m_controller->runClipboardBatch(batchOf({p.parts[i]}, "", true));
        if (!result.ok()) {
            const QString error = errorOf(result);
            if (firstError.isEmpty())
                firstError = error;
            for (const ClipId id : p.partClips[i])
                rows[id.value].skipped << error;
            continue;
        }
        const command::Json& data = result.steps.empty() ? result.data : result.steps.front().data;
        for (const command::Json& change : data.value("changes", command::Json::array())) {
            Row& row = rows[change.value("clip", std::uint64_t{0})];
            row.applied << strings(change.value("applied", command::Json::array()));
            row.skipped << strings(change.value("skipped", command::Json::array()));
            any = any || !row.applied.isEmpty();
        }
    }
    for (const auto& [id, reason] : p.left)
        rows[id.value].skipped << reason;

    QVariantList list;
    const SequencePtr seq = m_controller ? m_controller->currentSequence() : nullptr;
    const ProjectSnapshot snap = m_controller ? m_controller->snapshot() : nullptr;
    for (const ClipId id : p.order) {
        const auto loc = seq ? seq->findClip(id) : std::nullopt;
        if (!loc)
            continue;
        const Row& row = rows[id.value];
        list.append(QVariantMap{{QStringLiteral("clip"), clipKey(id)},
                                {QStringLiteral("name"), clipTitle(*snap, loc->clip())},
                                {QStringLiteral("kind"), isVideo(loc->clip()) ? QStringLiteral("video") : QStringLiteral("audio")},
                                {QStringLiteral("applied"), row.applied},
                                {QStringLiteral("skipped"), row.skipped}});
    }
    QString error;
    if (!any)
        error = !p.error.isEmpty() ? p.error : !firstError.isEmpty() ? firstError : tr("Nothing would change");
    return {{QStringLiteral("ok"), any}, {QStringLiteral("error"), error}, {QStringLiteral("rows"), list}};
}

QString ClipboardBridge::applyAttributes(const QString& mode, const QVariantMap& choices) {
    const bool paste = mode != QLatin1String("remove");
    Plan p = plan(paste, choices);
    // Only the parts that change something: one that would change nothing fails, and would fail the whole batch.
    std::vector<command::CommandRequest> parts;
    QString error = p.error;
    for (command::CommandRequest& part : p.parts) {
        const command::CommandResult dry = m_controller->runClipboardBatch(batchOf({part}, "", true));
        if (dry.ok())
            parts.push_back(std::move(part));
        else if (error.isEmpty())
            error = errorOf(dry);
    }
    if (parts.empty()) {
        if (error.isEmpty())
            error = tr("Nothing would change");
        setMessage(error);
        return error;
    }
    const command::CommandResult result = m_controller->runClipboardBatch(
        batchOf(std::move(parts),
                paste ? QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Paste Attributes")
                      : QT_TRANSLATE_NOOP("wabcut::app::ProjectController", "Remove Attributes"),
                false));
    if (!result.ok()) {
        error = errorOf(result);
        setMessage(error);
        return error;
    }
    int changed = 0;
    for (const command::StepResult& step : result.steps) {
        for (const command::Json& change : step.data.value("changes", command::Json::array())) {
            if (!change.value("applied", command::Json::array()).empty())
                ++changed;
        }
    }
    const int total = static_cast<int>(p.order.size());
    setMessage(paste ? tr("Paste Attributes: %1 of %2 clips changed").arg(changed).arg(total)
                     : tr("Remove Attributes: %1 of %2 clips changed").arg(changed).arg(total));
    return {};
}

} // namespace wabcut::app
