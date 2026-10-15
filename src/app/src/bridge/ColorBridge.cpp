#include "ColorBridge.h"

#include "BridgeIds.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/color/Cdl.h>
#include <wabcut/color/ColorCommands.h>
#include <wabcut/color/Looks.h>
#include <wabcut/command/Engine.h>
#include <wabcut/editor/ColorFrames.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Timecode.h>

#include <QJsonDocument>
#include <QQmlEngine>

#include <algorithm>
#include <cmath>

namespace wabcut::app {

namespace {

QString toQString(const std::string& s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

const EffectDescriptor& gradeDescriptor() { return *EffectRegistry::builtin().find(color::kGradeEffect); }

constexpr std::array<std::pair<const char*, std::string_view>, 9> kCurveKeys{{
    {"master", color::gradekey::master},
    {"red", color::gradekey::red},
    {"green", color::gradekey::green},
    {"blue", color::gradekey::blue},
    {"hueSat", color::gradekey::hueSat},
    {"hueHue", color::gradekey::hueHue},
    {"hueLuma", color::gradekey::hueLuma},
    {"lumaSat", color::gradekey::lumaSat},
    {"satSat", color::gradekey::satSat},
}};

std::string_view curveKeyOf(const QString& name) {
    for (const auto& [n, key] : kCurveKeys) {
        if (name == QLatin1String(n)) {
            return key;
        }
    }
    return {};
}

const EffectInstance* findInstance(const Clip& clip, EffectInstanceId id) {
    for (const EffectInstance& e : clip.effects()) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

} // namespace

ColorBridge::ColorBridge(QObject* parent) : QObject(parent) {
    // QML constructs the singleton with this constructor (it is default-constructible), and a singleton cannot create
    // the others while it is being made: attach to the project once the engine is done.
    QMetaObject::invokeMethod(this, &ColorBridge::refresh, Qt::QueuedConnection);
}

ColorBridge::~ColorBridge() = default;

ColorBridge* ColorBridge::create(QQmlEngine*, QJSEngine*) { return new ColorBridge; }

void ColorBridge::attach() {
    if (m_attached) {
        return;
    }
    ProjectController* c = controller();
    if (!c) {
        return;
    }
    m_attached = true;
    connect(c, &ProjectController::historyChanged, this, &ColorBridge::onHistory);
    connect(c, &ProjectController::playheadChanged, this, &ColorBridge::refresh);
    connect(c, &ProjectController::selectionChanged, this, &ColorBridge::refresh);
    connect(c, &ProjectController::sessionClosing, this, [this] { m_clip = {}; });
}

QString ColorBridge::clip() const { return clipKey(m_clip); }

void ColorBridge::onHistory() {
    ProjectController* c = controller();
    if (!c)
        return;
    const TimelineChange& change = c->lastChange();
    bool relevant = change.projectWide() || change.touches(m_clip) || change.touches(c->primaryClip());
    if (!relevant && (!m_targetFromSelection || !m_clip.isValid()))
        relevant = c->lastChangeAtPlayhead(); // another clip may be under the playhead now
    if (relevant)
        refresh();
}

QVariantMap ColorBridge::paramInfo(const QString& key) const {
    const EffectParamInfo* info = gradeDescriptor().param(key.toStdString());
    if (!info) {
        return {};
    }
    QString unit;
    switch (info->unit) {
    case ParamUnit::Stops:
        unit = QStringLiteral(" st");
        break;
    case ParamUnit::Degrees:
        unit = QStringLiteral("°");
        break;
    case ParamUnit::Pixels:
        unit = QStringLiteral(" px");
        break;
    default:
        break;
    }
    return {{QStringLiteral("name"), QString::fromUtf8(info->displayName.data(), static_cast<qsizetype>(info->displayName.size()))},
            {QStringLiteral("from"), info->minValue},
            {QStringLiteral("to"), info->maxValue},
            {QStringLiteral("default"), info->defaultValue},
            {QStringLiteral("unit"), unit}};
}

ProjectController* ColorBridge::controller() const {
    QQmlEngine* engine = qmlEngine(this);
    return engine ? engine->singletonInstance<ProjectController*>("Wabcut", "ProjectController") : nullptr;
}

PlaybackBridge* ColorBridge::playback() const {
    QQmlEngine* engine = qmlEngine(this);
    return engine ? engine->singletonInstance<PlaybackBridge*>("Wabcut", "Playback") : nullptr;
}

void ColorBridge::refresh() {
    attach();
    ProjectController* c = controller();
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    ClipId target;
    if (seq) {
        // The first selected video clip, else the topmost enabled video clip under the playhead.
        for (const QString& key : c->selectedClips()) {
            const auto id = parseClipKey(key);
            const auto loc = id ? seq->findClip(*id) : std::nullopt;
            if (loc && loc->track->kind == TrackKind::Video) {
                target = *id;
                break;
            }
        }
        m_targetFromSelection = target.isValid();
        if (!target.isValid()) {
            for (auto it = seq->videoTracks.rbegin(); it != seq->videoTracks.rend() && !target.isValid(); ++it) {
                if (const auto i = (*it)->clipIndexAt(c->playhead()); i && (*it)->clips[*i].enabled) {
                    target = (*it)->clips[*i].id;
                }
            }
        }
    }
    m_clip = target;
    m_grade = {};
    m_clipName.clear();
    m_animated.clear();
    m_curves.clear();
    m_inputLut.clear();
    m_look.clear();
    m_missingLuts = 0;
    m_gradeEnabled = true;
    color::GradeParams params;
    if (target.isValid()) {
        const Clip& clip = seq->findClip(target)->clip();
        m_clipName = toQString(clip.name());
        if (const EffectInstance* g = color::gradeOf(clip)) {
            m_grade = g->id;
            m_gradeEnabled = g->enabled;
            const Ticks t = c->curveTimeAtPlayhead(clip);
            const color::GradeSpec spec = color::evaluateGrade(*g, t, color::LutLibrary::shared().resolver(*c->session().snapshot()));
            params = spec.params;
            m_missingLuts = spec.missingLuts;
            for (const EffectParam& p : g->params) {
                if (p.value.isCurve()) {
                    m_animated << toQString(p.key);
                }
            }
            for (const auto& [name, key] : kCurveKeys) {
                if (const EffectData* d = g->findData(key); d && d->points()) {
                    QVariantList points;
                    for (const EffectPoint& p : *d->points()) {
                        points.append(QVariant(QVariantList{p.x, p.y}));
                    }
                    m_curves.insert(QString::fromLatin1(name), points);
                }
            }
            if (const EffectData* d = g->findData(color::gradekey::inputLut); d && d->text()) {
                m_inputLut = toQString(*d->text());
            }
            if (const EffectData* d = g->findData(color::gradekey::look); d && d->text()) {
                m_look = toQString(*d->text());
            }
        }
    }
    m_params.clear();
    for (const EffectParam& p : color::gradeParamsToEffect(params, false)) {
        m_params.insert(toQString(p.key), p.value.constant());
    }
    emit stateChanged();
    applyView();
}

QVariantList ColorBridge::lutChoices() const {
    QVariantList out;
    for (const color::BuiltinLut& b : color::builtinLuts()) {
        out.append(QVariantMap{{QStringLiteral("reference"), QStringLiteral("builtin:") + QString::fromLatin1(b.name.data(), static_cast<qsizetype>(b.name.size()))},
                               {QStringLiteral("name"), QString::fromLatin1(b.displayName.data(), static_cast<qsizetype>(b.displayName.size()))},
                               {QStringLiteral("kind"), b.input ? QStringLiteral("input") : QStringLiteral("look")},
                               {QStringLiteral("offline"), false}});
    }
    if (ProjectController* c = controller()) {
        for (const ProjectAsset& a : c->session().snapshot()->assets) {
            if (a.kind == kAssetKindLut) {
                out.append(QVariantMap{{QStringLiteral("reference"), toQString(assetReference(a.id))},
                                       {QStringLiteral("name"), toQString(a.name)},
                                       {QStringLiteral("kind"), QStringLiteral("file")},
                                       {QStringLiteral("offline"), a.locator.offline}});
            }
        }
    }
    return out;
}

// ---- Views ----------------------------------------------------------------------------------------------------

void ColorBridge::setCompareMode(int mode) {
    mode = std::clamp(mode, 0, 2);
    if (mode == m_compareMode) {
        return;
    }
    m_compareMode = mode;
    if (mode == 2 && !m_reference) {
        setReference(); // side by side needs a reference: the program as it is now
    }
    emit viewChanged();
    applyView();
}

void ColorBridge::setSplitPosition(double position) {
    position = std::clamp(position, 0.0, 1.0);
    if (position == m_split) {
        return;
    }
    m_split = position;
    emit viewChanged();
    applyView();
}

void ColorBridge::setMaskView(int view) {
    view = std::clamp(view, 0, 3);
    if (view == m_maskView) {
        return;
    }
    m_maskView = view;
    emit viewChanged();
    applyView();
}

void ColorBridge::setEyedropper(bool on) {
    if (on != m_eyedropper) {
        m_eyedropper = on;
        emit viewChanged();
    }
}

void ColorBridge::applyView() {
    PlaybackBridge* p = playback();
    if (!p) {
        return;
    }
    playback::ColorPreview preview;
    preview.view.maskView = m_maskView;
    preview.view.maskClip = m_clip.value;
    preview.view.split = m_compareMode == 1 ? m_split : -1.0;
    preview.reference = m_compareMode == 2 ? m_reference : nullptr;
    p->setColorPreview(std::move(preview));
}

bool ColorBridge::setReference() {
    ProjectController* c = controller();
    QQmlEngine* engine = qmlEngine(this);
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    if (!seq || !engine) {
        return false;
    }
    std::filesystem::path projectFile;
    if (auto* file = engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile")) {
        projectFile = file->document().file();
    }
    auto frame = editor::renderSequenceFrame(*c->session().snapshot(), seq->id, c->playhead(), 960, projectFile);
    if (!frame) {
        setStatus(tr("Cannot capture the reference: %1").arg(toQString(frame.error().message)));
        return false;
    }
    m_reference = std::make_shared<const render::StillImage>(frame->width, frame->height, std::move(frame->rgba));
    m_referenceTime = frame->time;
    m_referenceLabel = toQString(ticksToTimecode(frame->time, seq->frameRate, seq->dropFrame));
    emit viewChanged();
    applyView();
    return true;
}

void ColorBridge::clearReference() {
    m_reference.reset();
    m_referenceLabel.clear();
    if (m_compareMode == 2) {
        m_compareMode = 0;
    }
    emit viewChanged();
    applyView();
}

// ---- Edits ----------------------------------------------------------------------------------------------------

void ColorBridge::beginGesture() {
    ++m_gesture;
    m_inGesture = true;
}

void ColorBridge::endGesture() {
    m_inGesture = false;
    if (ProjectController* c = controller()) {
        c->endPropertyGesture();
    }
}

std::string ColorBridge::mergeKey(const QString& what) const {
    if (!m_inGesture) {
        return {};
    }
    return "color:" + std::to_string(m_clip.value) + ":" + std::to_string(m_gesture) + ":" + what.toStdString();
}

EffectInstanceId ColorBridge::ensureGrade() {
    if (m_grade.isValid()) {
        return m_grade;
    }
    ProjectController* c = controller();
    if (!c || !m_clip.isValid()) {
        return {};
    }
    const SequenceId seq = c->currentSequence()->id;
    const ClipId clip = m_clip;
    EffectInstanceId created;
    const bool ok = c->applyEditFn(
        "Add Color Grade",
        [&](const ProjectState& state) {
            const std::vector<ClipId> clips{clip};
            auto r = edit::addEffect(state, seq, clips, std::string(color::kGradeEffect));
            if (r) {
                created = r->createdEffects.front();
            }
            return r;
        },
        mergeKey(QStringLiteral("add")));
    if (ok) {
        m_grade = created;
    }
    return m_grade;
}

bool ColorBridge::setParam(const QString& key, double value) {
    ProjectController* c = controller();
    const EffectParamInfo* info = gradeDescriptor().param(key.toStdString());
    if (!c || !info || !std::isfinite(value)) {
        return false;
    }
    const EffectInstanceId grade = ensureGrade();
    if (!grade.isValid()) {
        return false;
    }
    value = std::clamp(value, info->minValue, info->maxValue);
    const std::string label = "Color: " + std::string(info->displayName);
    return c->setParameterAtPlayhead(m_clip, edit::ParamRef::ofEffect(grade, key.toStdString()), value, label.c_str(),
                                     mergeKey(key));
}

bool ColorBridge::setParams(const QVariantMap& values) {
    ProjectController* c = controller();
    if (!c || values.isEmpty()) {
        return false;
    }
    const EffectInstanceId grade = ensureGrade();
    if (!grade.isValid()) {
        return false;
    }
    const SequenceId seq = c->currentSequence()->id;
    const ClipId clipId = m_clip;
    QStringList keys = values.keys();
    keys.sort();
    return c->applyEditFn(
        "Color Wheel",
        [&](const ProjectState& state) -> edit::EditOutcome {
            ProjectState s = state;
            edit::EditOutcome last = std::unexpected(Error{ErrorCode::InvalidArgument, "no parameters"});
            for (const QString& key : keys) {
                const EffectParamInfo* info = gradeDescriptor().param(key.toStdString());
                if (!info) {
                    continue;
                }
                const double v = std::clamp(values.value(key).toDouble(), info->minValue, info->maxValue);
                const auto loc = s.findSequence(seq)->findClip(clipId);
                if (!loc) {
                    break;
                }
                const edit::ParamRef ref = edit::ParamRef::ofEffect(grade, key.toStdString());
                const ParamValue current = ProjectController::parameterValue(loc->clip(), ref);
                ParamValue next(v);
                if (const Curve* curve = current.curve()) {
                    Curve keysCurve = *curve;
                    const Ticks t = c->curveTimeAtPlayhead(loc->clip());
                    if (const auto at = keysCurve.keyAt(t)) {
                        keysCurve.keys[*at].value = v;
                    } else {
                        keysCurve.keys.insert(keysCurve.keys.begin() + static_cast<std::ptrdiff_t>(keysCurve.insertionIndex(t)),
                                              Keyframe{t, v});
                    }
                    next = ParamValue(std::move(keysCurve));
                }
                last = edit::setParameter(s, seq, clipId, ref, next);
                if (!last) {
                    return last;
                }
                s = last->state;
            }
            return last;
        },
        mergeKey(QStringLiteral("wheel:") + keys.join(QLatin1Char(','))));
}

bool ColorBridge::setStage(const QString& stage, bool enabled) {
    return setParam(stage + QStringLiteral(".enabled"), enabled ? 1.0 : 0.0);
}

bool ColorBridge::setGradeEnabled(bool enabled) {
    ProjectController* c = controller();
    if (!c || !m_grade.isValid()) {
        return false;
    }
    const SequenceId seq = c->currentSequence()->id;
    const ClipId clip = m_clip;
    const EffectInstanceId grade = m_grade;
    return c->applyEditFn(enabled ? "Enable Color Grade" : "Disable Color Grade", [&](const ProjectState& state) {
        edit::EffectChange change;
        change.enabled = enabled;
        return edit::setEffect(state, seq, clip, grade, change);
    });
}

bool ColorBridge::resetStage(const QString& stage) {
    ProjectController* c = controller();
    if (!c || !m_grade.isValid()) {
        return false;
    }
    const std::string prefix = stage == QLatin1String("all") ? std::string() : stage.toStdString() + ".";
    const SequenceId seq = c->currentSequence()->id;
    const ClipId clipId = m_clip;
    const EffectInstanceId grade = m_grade;
    const std::string label = "Reset " + (prefix.empty() ? std::string("Color Grade") : stage.toStdString());
    return c->applyEditFn(label.c_str(), [&](const ProjectState& state) -> edit::EditOutcome {
        const auto loc = state.findSequence(seq)->findClip(clipId);
        const EffectInstance* g = loc ? findInstance(loc->clip(), grade) : nullptr;
        if (!g) {
            return std::unexpected(Error{ErrorCode::NotFound, "the grade is gone"});
        }
        // Parameters of the stage back to their defaults (their keys removed, curves included), its data too.
        EffectInstance next = *g;
        std::erase_if(next.params, [&](const EffectParam& p) {
            return (prefix.empty() || p.key.starts_with(prefix)) && !p.key.ends_with(".enabled");
        });
        std::erase_if(next.data, [&](const EffectData& d) {
            return prefix.empty() || d.key.starts_with(prefix) ||
                   (stage == QLatin1String("basic") && d.key == color::gradekey::inputLut) ||
                   (stage == QLatin1String("creative") && d.key == color::gradekey::look);
        });
        ProjectState s = state;
        auto sequence = std::make_shared<Sequence>(*s.findSequence(seq));
        for (TrackPtr& t : sequence->videoTracks) {
            if (t->id != loc->track->id) {
                continue;
            }
            auto track = std::make_shared<Track>(*t);
            EffectStack stack = track->clips[loc->index].effects();
            for (EffectInstance& e : stack) {
                if (e.id == grade) {
                    e = next;
                }
            }
            track->clips.mutableAt(loc->index).setEffects(std::move(stack));
            t = std::move(track);
        }
        edit::EditResult r;
        r.state = s.withSequence(std::move(sequence));
        r.changes = diff(state, r.state);
        return r;
    });
}

bool ColorBridge::setCurve(const QString& name, const QVariantList& points) {
    ProjectController* c = controller();
    const std::string_view key = curveKeyOf(name);
    if (!c || key.empty()) {
        return false;
    }
    std::vector<EffectPoint> pts;
    for (const QVariant& v : points) {
        const QVariantList p = v.toList();
        if (p.size() == 2) {
            pts.push_back({std::clamp(p[0].toDouble(), 0.0, 1.0), std::clamp(p[1].toDouble(), 0.0, 1.0)});
        }
    }
    std::sort(pts.begin(), pts.end(), [](const EffectPoint& a, const EffectPoint& b) { return a.x < b.x; });
    // Strictly increasing x (a dragged point never lands on its neighbour).
    for (std::size_t i = 1; i < pts.size(); ++i) {
        pts[i].x = std::max(pts[i].x, pts[i - 1].x + 1e-4);
    }
    std::erase_if(pts, [](const EffectPoint& p) { return p.x > 1.0 + 1e-9; });
    if (pts.size() > kMaxEffectPoints) {
        pts.resize(kMaxEffectPoints);
    }
    const EffectInstanceId grade = ensureGrade();
    if (!grade.isValid()) {
        return false;
    }
    const SequenceId seq = c->currentSequence()->id;
    const ClipId clip = m_clip;
    return c->applyEditFn(
        "Color Curve",
        [&](const ProjectState& state) {
            edit::EffectChange change;
            change.data = {EffectData{std::string(key), pts}};
            return edit::setEffect(state, seq, clip, grade, change);
        },
        mergeKey(QStringLiteral("curve:") + name));
}

QVariantList ColorBridge::curveSamples(const QString& name, int count) const {
    QVariantList out;
    count = std::clamp(count, 2, 2048);
    std::vector<EffectPoint> pts;
    for (const QVariant& v : m_curves.value(name).toList()) {
        const QVariantList p = v.toList();
        if (p.size() == 2) {
            pts.push_back({p[0].toDouble(), p[1].toDouble()});
        }
    }
    const bool hue = name.startsWith(QLatin1String("hue"));
    const bool neutral = name == QLatin1String("lumaSat") || name == QLatin1String("satSat");
    for (int i = 0; i < count; ++i) {
        const double x = static_cast<double>(i) / (count - 1);
        out.append(hue ? color::evaluateHueCurve(pts, x) : neutral ? color::evaluateNeutralCurve(pts, x) : color::evaluateCurve(pts, x));
    }
    return out;
}

bool ColorBridge::setLut(const QString& slot, const QString& reference) {
    ProjectController* c = controller();
    if (!c || (slot != QLatin1String("input") && slot != QLatin1String("look"))) {
        return false;
    }
    const EffectInstanceId grade = ensureGrade();
    if (!grade.isValid()) {
        return false;
    }
    const SequenceId seq = c->currentSequence()->id;
    const ClipId clip = m_clip;
    const std::string key(slot == QLatin1String("input") ? color::gradekey::inputLut : color::gradekey::look);
    return c->applyEditFn(slot == QLatin1String("input") ? "Input LUT" : "Look", [&](const ProjectState& state) {
        edit::EffectChange change;
        change.data = {EffectData{key, reference.toStdString()}};
        return edit::setEffect(state, seq, clip, grade, change);
    });
}

void ColorBridge::setStatus(const QString& status) {
    m_status = status;
    emit busyChanged();
}

bool ColorBridge::runCommand(const char* name, const QVariantMap& params, QString* summary) {
    ProjectController* c = controller();
    QQmlEngine* engine = qmlEngine(this);
    if (!c || !engine) {
        return false;
    }
    std::filesystem::path projectFile;
    if (auto* file = engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile")) {
        projectFile = file->document().file();
    }
    command::EnginePolicy policy;
    policy.services = std::make_shared<editor::ColorFrameService>(projectFile);
    const command::CommandRegistry& registry = ai::edit::editingCommands();
    command::CommandEngine commands(c->session(), registry, policy);
    command::CommandRequest request;
    request.command = name;
    request.params = command::Json::parse(QJsonDocument::fromVariant(params).toJson(QJsonDocument::Compact).toStdString());
    // The sequence on screen, for the commands that take one (lut.import and lut.list are project-wide).
    if (const command::CommandSpec* spec = registry.find(name);
        spec && spec->params.contains("properties") && spec->params["properties"].contains("sequence")) {
        request.params["sequence"] = c->currentSequence()->id.value;
    }
    m_busy = true;
    emit busyChanged();
    const command::CommandResult result = commands.execute(request);
    m_busy = false;
    emit busyChanged();
    if (!result.ok()) {
        setStatus(toQString(result.error->message));
        return false;
    }
    if (summary) {
        *summary = toQString(result.data.dump());
    }
    return true;
}

bool ColorBridge::autoColor() {
    if (!m_clip.isValid()) {
        return false;
    }
    QString data;
    const bool ok = runCommand("color.auto", {{QStringLiteral("clip"), static_cast<qulonglong>(m_clip.value)}}, &data);
    if (ok) {
        const auto json = command::Json::parse(data.toStdString(), nullptr, false);
        setStatus(json.is_object() && json.contains("clips") && !json["clips"].empty()
                      ? tr("Auto: %1").arg(toQString(json["clips"][0].value("summary", std::string())))
                      : tr("Auto applied"));
    }
    return ok;
}

bool ColorBridge::matchToReference(double strength, bool skinPriority) {
    if (!m_clip.isValid() || !m_reference) {
        setStatus(tr("Set a reference frame first"));
        return false;
    }
    QString data;
    const bool ok = runCommand("color.match",
                               {{QStringLiteral("clip"), static_cast<qulonglong>(m_clip.value)},
                                {QStringLiteral("reference"), static_cast<qlonglong>(m_referenceTime.value)},
                                {QStringLiteral("strength"), std::clamp(strength, 0.0, 1.0)},
                                {QStringLiteral("skinPriority"), skinPriority}},
                               &data);
    if (ok) {
        setStatus(tr("Matched to the reference at %1").arg(m_referenceLabel));
    }
    return ok;
}

bool ColorBridge::matchScene(double strength, bool skinPriority) {
    if (!m_clip.isValid()) {
        return false;
    }
    QString data;
    const bool ok = runCommand("color.matchScene",
                               {{QStringLiteral("reference"), static_cast<qulonglong>(m_clip.value)},
                                {QStringLiteral("strength"), std::clamp(strength, 0.0, 1.0)},
                                {QStringLiteral("skinPriority"), skinPriority}},
                               &data);
    if (ok) {
        const auto json = command::Json::parse(data.toStdString(), nullptr, false);
        setStatus(tr("Matched %n clip(s) of the scene", nullptr, json.is_object() ? static_cast<int>(json["clips"].size()) : 0));
    }
    return ok;
}

QString ColorBridge::importLut(const QUrl& file, const QString& slot) {
    QString data;
    if (!runCommand("lut.import", {{QStringLiteral("path"), file.toLocalFile()}}, &data)) {
        return m_status;
    }
    const auto json = command::Json::parse(data.toStdString(), nullptr, false);
    if (!json.is_object() || !json.contains("reference")) {
        return tr("The LUT was not added");
    }
    refresh();
    if (!slot.isEmpty() && !setLut(slot, toQString(json["reference"].get<std::string>()))) {
        return tr("The LUT was added but could not be applied");
    }
    return {};
}

QString ColorBridge::exportCdl(const QUrl& file) {
    if (!m_clip.isValid()) {
        return tr("No clip");
    }
    QString data;
    if (!runCommand("cdl.export",
                    {{QStringLiteral("clip"), static_cast<qulonglong>(m_clip.value)},
                     {QStringLiteral("path"), file.toLocalFile()},
                     {QStringLiteral("overwrite"), true}},
                    &data)) {
        return m_status;
    }
    setStatus(tr("CDL written to %1").arg(file.fileName()));
    return {};
}

QString ColorBridge::importCdl(const QUrl& file) {
    if (!m_clip.isValid()) {
        return tr("No clip");
    }
    QString data;
    if (!runCommand("cdl.import", {{QStringLiteral("clip"), static_cast<qulonglong>(m_clip.value)}, {QStringLiteral("path"), file.toLocalFile()}},
                    &data)) {
        return m_status;
    }
    setStatus(tr("Wheels set from %1").arg(file.fileName()));
    return {};
}

bool ColorBridge::pickColor(double nx, double ny) {
    ProjectController* c = controller();
    QQmlEngine* engine = qmlEngine(this);
    const SequencePtr seq = c ? c->currentSequence() : nullptr;
    if (!seq || !engine || !m_clip.isValid()) {
        return false;
    }
    std::filesystem::path projectFile;
    if (auto* file = engine->singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile")) {
        projectFile = file->document().file();
    }
    // The picture without the secondary: what the qualifier looks at.
    ProjectState shown = *c->session().snapshot();
    if (m_grade.isValid()) {
        auto off = edit::setParameter(shown, seq->id, m_clip, edit::ParamRef::ofEffect(m_grade, "hsl.enabled"), ParamValue(0.0));
        if (off) {
            shown = off->state;
        }
    }
    auto frame = editor::renderSequenceFrame(shown, seq->id, c->playhead(), 640, projectFile);
    if (!frame) {
        setStatus(tr("Cannot read the picture: %1").arg(toQString(frame.error().message)));
        return false;
    }
    const int x = std::clamp(static_cast<int>(nx * frame->width), 0, frame->width - 1);
    const int y = std::clamp(static_cast<int>(ny * frame->height), 0, frame->height - 1);
    // A 5x5 average around the click.
    color::Rgb sum{0, 0, 0};
    int n = 0;
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int px = std::clamp(x + dx, 0, frame->width - 1), py = std::clamp(y + dy, 0, frame->height - 1);
            const std::uint8_t* p = frame->rgba.data() + (static_cast<std::size_t>(py) * frame->width + px) * 4;
            sum = {sum[0] + p[0] / 255.0, sum[1] + p[1] / 255.0, sum[2] + p[2] / 255.0};
            ++n;
        }
    }
    const color::Rgb picked{sum[0] / n, sum[1] / n, sum[2] / n};
    const color::Hsv hsv = color::toHsv(picked);
    const double luma = std::clamp(color::luma(picked), 0.0, 1.0) * 100.0;
    const double sat = hsv.s * 100.0;
    QVariantMap q{{QStringLiteral("hsl.hue.center"), hsv.h * 360.0},
                  {QStringLiteral("hsl.hue.width"), 40.0},
                  {QStringLiteral("hsl.hue.softness"), 20.0},
                  {QStringLiteral("hsl.sat.low"), std::max(0.0, sat - 25.0)},
                  {QStringLiteral("hsl.sat.high"), std::min(100.0, sat + 25.0)},
                  {QStringLiteral("hsl.sat.softness"), 10.0},
                  {QStringLiteral("hsl.luma.low"), std::max(0.0, luma - 30.0)},
                  {QStringLiteral("hsl.luma.high"), std::min(100.0, luma + 30.0)},
                  {QStringLiteral("hsl.luma.softness"), 10.0}};
    const bool ok = setParams(q);
    setEyedropper(false);
    if (ok) {
        setStatus(tr("Qualifier picked: hue %1°, saturation %2 %, luma %3 %")
                      .arg(hsv.h * 360.0, 0, 'f', 0)
                      .arg(sat, 0, 'f', 0)
                      .arg(luma, 0, 'f', 0));
    }
    return ok;
}

} // namespace wabcut::app
