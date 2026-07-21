#include "shell/workspace.h"

#include <cstdio>

#include <QFileInfo>
#include <QStringList>

#include "app/scriptplay.h"
#include "core/persist.h"
#include "interact/registry.h"
#include "interact/surface.h"
#include "shell/sidecar.h"
#include "solve/scheduler.h"

namespace paroculus {
namespace {

// A shell policy, not a document one: the entity count at or above which a
// component solves off-thread. Large, because asynchrony has a feel cost — the
// rubber-banding of a pose that lags the cursor — that only a genuinely heavy
// component should ever pay. A display-only, corpus-free number the discovery
// window tunes; changing it moves no recorded gesture.
constexpr size_t kAsyncEntityThreshold = 256;

// One surface entry, as QML sees it. A plain map: the list is short, rebuilt
// whenever anything changes, and a model would be a second place for
// applicability to be decided. Ported unchanged from the stand-in shell.
QVariantMap entryOf(const SurfaceEntry &entry) {
    QVariantMap map;
    map[QStringLiteral("name")] =
        QString::fromLatin1(entry.action->name.data(), int(entry.action->name.size()));
    map[QStringLiteral("title")] = QString::fromStdString(entry.title);
    map[QStringLiteral("applicable")] = entry.applicable;
    QVariantMap arguments;
    for(const auto &[key, value] : entry.arguments.values) {
        arguments[QString::fromStdString(key)] = value;
    }
    map[QStringLiteral("arguments")] = arguments;
    return map;
}

}  // namespace

Workspace::Workspace(QObject *parent) : QObject(parent) {
    reports_ = new ReportsModel(this);
    session_ = std::make_unique<Session>(document_, journal_);

    // Roughly a frame per step, so a recorded drag replays at about the speed it
    // was performed: fast enough to read as a gesture, slow enough to watch.
    scriptTimer_.setInterval(16);
    connect(&scriptTimer_, &QTimer::timeout, this, &Workspace::stepScript);

    // Frame-adjacent, and running only while a component owes a result. The
    // async integration contract: pump on a timer, never bump the epoch, apply
    // whole results only. pump() honours all three.
    pumpTimer_.setInterval(16);
    connect(&pumpTimer_, &QTimer::timeout, this, &Workspace::pump);
}

// Written at teardown rather than incrementally: a session is what happened
// between opening and closing, and a half-file from a crashed run would replay
// into a state nobody was ever in.
Workspace::~Workspace() {
    if(recorder_ == nullptr || recordPath_.isEmpty()) return;
    GestureScript script;
    script.document = recordedFrom_;
    script.steps = recorder_->steps();
    std::string error;
    if(saveScriptFile(recordPath_.toStdString(), script, error)) {
        std::fprintf(stderr, "recorded %zu steps to %s\n", script.steps.size(),
                     recordPath_.toUtf8().constData());
    } else {
        std::fprintf(stderr, "%s\n", error.c_str());
    }
}

QString Workspace::title() const {
    if(!filePath_.isEmpty()) return QFileInfo(filePath_).fileName();
    return untitledOrdinal_ > 0 ? QStringLiteral("untitled-%1").arg(untitledOrdinal_)
                                : QStringLiteral("untitled");
}

bool Workspace::dirty() const { return journal_.revision() != savedRevision_; }

bool Workspace::busy() const { return session_->asyncBusy(); }

void Workspace::notifyChanged() {
    kickPumpIfBusy();
    captureReports();
    // Drop any imposition ghost: it previews what committing would do to the
    // geometry as it was, so an edit makes it stale, and — the reason this is
    // here rather than left to the hovering surface's onExited — a changed()
    // rebuilds the strip and constraints toolbar, destroying a hovered delegate
    // before its exit can fire and disarm the ghost. Cleared here so no phantom
    // pose survives on the canvas.
    ghostPose_.clear();
    emit changed();
}

void Workspace::kickPumpIfBusy() {
    if(asyncEnabled_ && session_->asyncBusy() && !pumpTimer_.isActive()) pumpTimer_.start();
}

void Workspace::enableAsync() {
    if(asyncEnabled_) return;
    session_->enableAsyncSolving(kAsyncEntityThreshold, SolveScheduler::defaultWorkerCount());
    asyncEnabled_ = true;
}

void Workspace::disableAsync() {
    if(!asyncEnabled_) return;
    session_->disableAsyncSolving();
    asyncEnabled_ = false;
    pumpTimer_.stop();
}

void Workspace::pump() {
    // A threaded result lands only here, and this never bumps the epoch — the
    // two halves of the contract on pumpAsync's declaration. Repaint via changed()
    // on a true return; stop once nothing is outstanding. A pose that moved makes
    // a hovered imposition ghost stale, and the changed() rebuilds the toolbars
    // under the cursor, so drop the ghost the same way notifyChanged does.
    if(session_->pumpAsync()) {
        ghostPose_.clear();
        emit changed();
    }
    if(!session_->asyncBusy()) pumpTimer_.stop();
}

void Workspace::playScript(GestureScript script) {
    script_ = std::move(script);
    scriptStep_ = 0;
    playing_ = true;

    // The script's document replaces ours wholesale and the journal starts empty:
    // a replay is the recorded session, not that session appended to whatever was
    // already showing.
    document_ = script_.document;
    journal_ = UndoJournal();
    session_ = std::make_unique<Session>(document_, journal_);
    savedRevision_ = journal_.revision();
    // The fresh session has async off; drop the flag so a later activate re-enables
    // it, matching loadFrom. A recorder outlives the session, so re-attach it.
    asyncEnabled_ = false;
    if(recorder_ != nullptr) session_->setRecorder(recorder_.get());
    // Resync the report baseline to the new session's zeroed fields, so leftover
    // state from a prior session cannot be reported as the first edit's doing.
    lastReportSnapshot_ = snapshotReports();

    scriptTimer_.start();
    emit changed();
}

void Workspace::stepScript() {
    if(scriptStep_ >= script_.steps.size()) {
        scriptTimer_.stop();
        playing_ = false;
        emit changed();
        return;
    }
    applyStep(*session_, script_.steps[scriptStep_++]);
    // A replayed step can build an over-budget component, so kick the pump the
    // same way an interactive edit does; otherwise its off-thread pose freezes.
    kickPumpIfBusy();
    // Playback is silent — it emits no reports — so track the report baseline as
    // it goes, or the first interactive edit after playback would diff against a
    // stale pre-playback snapshot and report a step the script took.
    lastReportSnapshot_ = snapshotReports();
    emit changed();
}

void Workspace::startRecording(const QString &path) {
    recordPath_ = path;
    // Captured before anything is attached and before the first viewport is
    // pushed, so the file starts from what the session actually started from.
    // Opening a document is not an edit, so this is that document.
    recordedFrom_ = document_;
    recorder_ = std::make_unique<ScriptRecorder>();
    session_->setRecorder(recorder_.get());
}

LoadResult Workspace::loadFrom(const std::string &text, const QString &path) {
    Document loaded;
    const LoadResult result = deserialize(text, loaded);
    if(!result) return result;  // untouched on failure, exactly as the loader promises

    document_ = std::move(loaded);
    journal_.clear();
    session_ = std::make_unique<Session>(document_, journal_);
    savedRevision_ = journal_.revision();
    filePath_ = path;
    // A fresh document asks for a re-fit: clear the latch so the next viewport
    // sync frames it. The sidecar, loaded next by the manager, layers pan and
    // zoom over that fitted base.
    view_ = ViewState{};
    asyncEnabled_ = false;
    // A recorder outlives the session, so a record-then-open keeps recording.
    if(recorder_ != nullptr) session_->setRecorder(recorder_.get());
    // Resync the report baseline to the loaded session, so a report is never
    // attributed to the open.
    lastReportSnapshot_ = snapshotReports();
    // The session was replaced under a stable workspace pointer: a load into a
    // reused tab does not change what App.active returns, so the canvas would not
    // re-sync the viewport on its own and would paint the new document through the
    // fresh session's identity viewport. viewReset tells it to sync.
    emit viewReset();
    emit changed();
    return result;
}

std::string Workspace::serializeDocument() const { return serialize(document_); }

void Workspace::markSaved(const QString &path) {
    filePath_ = path;
    savedRevision_ = journal_.revision();
    emit changed();
}

std::string Workspace::sidecarPath() const {
    if(filePath_.isEmpty()) return {};
    return sidecarPathFor(filePath_.toStdString());
}

void Workspace::loadSidecarFrom(const std::string &path) {
    const Sidecar s = loadSidecar(path);
    view_.pan = Eigen::Vector2d(s.panX, s.panY);
    view_.zoom = s.zoom;
}

void Workspace::writeSidecarTo(const std::string &path) const {
    Sidecar s;
    s.panX = view_.pan.x();
    s.panY = view_.pan.y();
    s.zoom = view_.zoom;
    saveSidecar(path, s);
}

// ---------------------------------------------------------------------------
// Projections, ported unchanged from the stand-in SketchView. The status
// mega-string is dismantled into typed models and toasts in a later U0 step.
// ---------------------------------------------------------------------------

QString Workspace::activeToolAction() const {
    switch(session_->tool()) {
        case ToolKind::Line:      return QStringLiteral("tool.line");
        case ToolKind::Circle:    return QStringLiteral("tool.circle");
        case ToolKind::Arc:       return QStringLiteral("tool.arc");
        case ToolKind::Rectangle: return QStringLiteral("tool.rectangle");
        case ToolKind::Select:    break;
    }
    return QStringLiteral("tool.select");
}

QVariantList Workspace::actions() const {
    QVariantList out;
    // Applicability decided against a snapshot, exactly as invokeAction decides
    // it, so a menu can never offer what the model would refuse.
    const ActionContext context = contextOf(*session_);
    for(const Action &a : paroculus::actions()) {
        QVariantMap map;
        map[QStringLiteral("name")] = QString::fromUtf8(a.name.data(), int(a.name.size()));
        map[QStringLiteral("title")] = QString::fromUtf8(a.title.data(), int(a.title.size()));
        map[QStringLiteral("binding")] =
            QString::fromUtf8(a.binding.data(), int(a.binding.size()));
        map[QStringLiteral("category")] =
            QString::fromUtf8(a.category.data(), int(a.category.size()));
        map[QStringLiteral("description")] =
            QString::fromUtf8(a.description.data(), int(a.description.size()));
        map[QStringLiteral("generated")] = a.generated;
        // The imposition family, for the constraints toolbar's fixed groups. Only
        // meaningful on a generated row; a projection of the taxonomy so the
        // toolbar groups by the same data the catalogue is built from.
        map[QStringLiteral("family")] =
            a.generated ? QString::fromLatin1(familyName(constraintFamily(a.constraintKind)))
                        : QString();
        map[QStringLiteral("strength")] = static_cast<int>(a.strength);
        map[QStringLiteral("applicable")] =
            a.applicable != nullptr ? a.applicable(context, a) : true;
        // Whether it carries a required parameter, so a name-only surface knows to
        // open the numeric entry rather than run it with a remembered value — the
        // rule that an action whose value is a required number cannot be invoked
        // from a surface that knows only its name. (The pending-entry flow itself
        // lands with the numeric-entry widget; this flags the rows for it.)
        bool needsValue = false;
        for(const ActionParameter &p : a.parameters) {
            if(p.required) needsValue = true;
        }
        map[QStringLiteral("needsValue")] = needsValue;
        out.append(map);
    }
    return out;
}

int Workspace::dof() const { return session_->presentation().dof; }

QString Workspace::solveStatus() const {
    return QString::fromLatin1(statusName(session_->presentation().status));
}

QString Workspace::selectionText() const {
    const Signature signature = session_->signature();
    if(signature.empty()) return QStringLiteral("nothing selected");
    return QString::fromStdString(signature.describe());
}

QVariantList Workspace::strip() const {
    QVariantList out;
    for(const SurfaceEntry &entry : stripEntries(*session_)) out.append(entryOf(entry));
    return out;
}

QVariantList Workspace::layers() const {
    QVariantList out;
    for(LayerId id : layerOrder(session_->document())) {
        const LayerRecord *layer = session_->document().layers().find(id);
        if(layer == nullptr) continue;
        QVariantMap row;
        row["id"] = static_cast<int>(layer->id.value());
        row["name"] = QString::fromStdString(layer->name);
        row["visible"] = layer->visible;
        row["locked"] = layer->locked;
        out.prepend(row);
    }
    return out;
}

int Workspace::hiddenInfluences() const {
    return static_cast<int>(session_->presentation().hiddenInfluences.size());
}
int Workspace::brokenRegions() const {
    return static_cast<int>(session_->presentation().brokenRegions.size());
}
int Workspace::brokenTags() const {
    return static_cast<int>(session_->presentation().brokenTags.size());
}

QVariantList Workspace::rectangles() const {
    QVariantList out;
    for(const Session::RectanglePanel &panel : session_->rectanglePanels()) {
        QVariantMap map;
        map[QStringLiteral("tag")] = static_cast<int>(panel.tag.value());
        map[QStringLiteral("width")] = panel.size.width;
        map[QStringLiteral("height")] = panel.size.height;
        map[QStringLiteral("widthDriven")] = panel.size.widthDimension.valid();
        map[QStringLiteral("heightDriven")] = panel.size.heightDimension.valid();
        out.append(map);
    }
    return out;
}

QVariantList Workspace::palette(const QString &query) const {
    QVariantList out;
    const std::string text = query.toStdString();
    for(const SurfaceEntry &entry : paletteEntries(*session_, text)) out.append(entryOf(entry));
    return out;
}

bool Workspace::run(const QString &name, const QVariantMap &arguments) {
    ActionArguments args;
    for(auto it = arguments.begin(); it != arguments.end(); ++it) {
        bool ok = false;
        const double value = it.value().toDouble(&ok);
        if(ok) args.set(it.key().toStdString(), value);
    }
    const bool ran = invokeAction(*session_, name.toStdString(), args);
    notifyChanged();
    return ran;
}

void Workspace::clearPreview() {
    if(ghostPose_.empty()) return;
    ghostPose_.clear();
    emit changed();
}

QString Workspace::previewOf(const QString &name, int assignment) {
    clearPreview();
    const Action *action = findAction(name.toStdString());
    if(action == nullptr || !action->generated || assignment < 0) return {};

    const std::optional<ImpositionPreview> preview =
        session_->previewImposition(action->constraintKind, size_t(assignment));
    if(!preview) return QStringLiteral("not applicable");

    if(preview->check.committable()) {
        ghostPose_ = preview->pose;
        emit changed();
    }

    switch(preview->check.verdict) {
        case CandidateVerdict::Consistent:
            return preview->motion <= session_->surfacePolicy().movementTolerance
                       ? QStringLiteral("holds")
                       : QStringLiteral("holds · moves %1").arg(preview->motion, 0, 'f', 2);
        case CandidateVerdict::Redundant:
            return QStringLiteral("already implied");
        case CandidateVerdict::Inconsistent:
            return preview->check.attributed && !preview->check.conflicting.empty()
                       ? QStringLiteral("conflicts with %1").arg(preview->check.conflicting.size())
                       : QStringLiteral("cannot hold");
        default:
            return QStringLiteral("cannot hold");
    }
}

void Workspace::undo() {
    session_->handle(Key::Undo);
    notifyChanged();
}
void Workspace::redo() {
    session_->handle(Key::Redo);
    notifyChanged();
}
void Workspace::deleteSelection() {
    session_->handle(Key::Delete);
    notifyChanged();
}

double Workspace::zoom() const { return view_.zoom; }

double Workspace::solveMilliseconds() const {
    return session_->presentation().solveMicroseconds / 1000.0;
}

QString Workspace::toolName() const {
    const ToolKind tool = session_->presentation().tool;
    // Qualified: the member and the free taxonomy function share the name.
    return tool == ToolKind::Select ? QString() : QString::fromLatin1(paroculus::toolName(tool));
}

QVariantList Workspace::toolParameters() const {
    QVariantList out;
    for(const ToolParameter &parameter : session_->presentation().toolParameters) {
        QVariantMap map;
        map[QStringLiteral("name")] = QString::fromLatin1(parameter.name);
        map[QStringLiteral("value")] = parameter.value;
        out.append(map);
    }
    return out;
}

QVariantMap Workspace::numericEntry() const {
    const Presentation &p = session_->presentation();
    QVariantMap map;
    map[QStringLiteral("active")] = p.numericActive;
    map[QStringLiteral("text")] = QString::fromStdString(p.numericText);
    map[QStringLiteral("name")] = p.numericTarget < p.toolParameters.size()
                                      ? QString::fromLatin1(p.toolParameters[p.numericTarget].name)
                                      : QString();
    return map;
}

bool Workspace::saturated() const { return session_->presentation().saturated; }
int Workspace::resisting() const {
    return static_cast<int>(session_->presentation().resisting.size());
}
bool Workspace::rippledOffScreen() const { return session_->presentation().rippledOffScreen; }
int Workspace::closedLoopEdges() const {
    return static_cast<int>(session_->presentation().closedLoop.size());
}
bool Workspace::crossing() const { return session_->presentation().crossing.has_value(); }

Workspace::ReportSnapshot Workspace::snapshotReports() const {
    const Presentation &p = session_->presentation();
    const Presentation::StructureReport &st = p.structure;
    ReportSnapshot s;
    s.deletedEntities = p.deletedEntities;
    s.deletedRelations = p.deletedRelations;
    s.degraded = p.degraded;
    s.transformError = static_cast<int>(st.transformError);
    s.compoundError = static_cast<int>(st.compoundError);
    s.retargeted = st.retargeted;
    s.rescaled = st.rescaled;
    s.straddling = st.straddling;
    s.copied = st.copied;
    s.droppedRelations = st.droppedRelations;
    s.droppedRegions = st.droppedRegions;
    s.droppedTags = st.droppedTags;
    if(p.crossing) {
        s.crossingA = p.crossing->first.value();
        s.crossingB = p.crossing->second.value();
    }
    return s;
}

void Workspace::captureReports() {
    const ReportSnapshot now = snapshotReports();
    // Nothing moved: no new event. The fields persist across operations, so this
    // early-out is only the trivial half of the guard — the real work below is
    // per group, because a change in one group must not re-emit another's still-
    // nonzero fields (a rotate that retargeted must not re-report a prior delete).
    if(now == lastReportSnapshot_) return;

    const Presentation &p = session_->presentation();
    const Presentation::StructureReport &st = p.structure;
    QStringList parts;

    // Deletion group: emitted only when its own fields changed.
    if(!lastReportSnapshot_.deletionEquals(now) &&
       (p.deletedEntities > 0 || p.deletedRelations > 0 || p.degraded > 0)) {
        QString s = QStringLiteral("Deleted %1 shapes, %2 relations")
                        .arg(p.deletedEntities)
                        .arg(p.deletedRelations);
        if(p.degraded > 0) s += QStringLiteral(", degraded %1").arg(p.degraded);
        parts << s;
    }

    // Structure group: the whole report is set together by one operation, so it
    // changes and clears as a unit.
    if(!lastReportSnapshot_.structureEquals(now)) {
        if(st.transformError != TransformError::None) {
            parts << QStringLiteral("Transform refused: %1")
                         .arg(QString::fromLatin1(transformErrorName(st.transformError)));
        }
        if(st.compoundError != CompoundError::None) {
            parts << QStringLiteral("Compound refused: %1")
                         .arg(QString::fromLatin1(compoundErrorName(st.compoundError)));
        }
        if(st.retargeted > 0) {
            parts << QStringLiteral("Retargeted %1 axis relations to a cluster frame")
                         .arg(st.retargeted);
        }
        if(st.rescaled > 0) parts << QStringLiteral("Rescaled %1 dimensions").arg(st.rescaled);
        if(st.straddling > 0) {
            parts << QStringLiteral("%1 dimensions reach outside and will resist").arg(st.straddling);
        }
        if(st.copied > 0) parts << QStringLiteral("Copied %1 shapes").arg(st.copied);
        if(st.droppedRelations > 0 || st.droppedRegions > 0 || st.droppedTags > 0) {
            parts << QStringLiteral("Dropped %1 relations, %2 fills, %3 tags at the boundary")
                         .arg(st.droppedRelations)
                         .arg(st.droppedRegions)
                         .arg(st.droppedTags);
        }
    }

    // Crossing: keyed on the edge pair, so a crossing on a different pair reports
    // even while an earlier one is still standing.
    if(!lastReportSnapshot_.crossingEquals(now) && p.crossing) {
        parts << QStringLiteral("Edges cross: intersection points are needed to fill this");
    }

    lastReportSnapshot_ = now;
    if(parts.isEmpty()) return;
    const QString text = parts.join(QStringLiteral("  ·  "));
    reports_->append(text);
    emit reportPosted(text);
}

}  // namespace paroculus
