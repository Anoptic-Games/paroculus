#include "shell/workspace.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include "app/scriptplay.h"
#include "core/bake.h"
#include "core/persist.h"
#include "core/svg.h"
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

// A packed 0xAARRGGBB colour as the "#aarrggbb" QML reads as a color. The raw
// value rides alongside for the setter, which passes it back to run() as a
// number — QML colour bindings want the string, the action wants the number.
QString argbHex(uint32_t colour) {
    return QString::asprintf("#%08x", colour);
}

// The loss a bake would cost, in words, computed once and shared by the export
// preview (the dialog, before the write) and the export report (the log, after
// it), so the two surfaces cannot word the same loss differently. `survived` is
// what reaches the file; `lost` is the parts that flattened or dropped, empty
// when the export is lossless.
struct ExportSummary {
    QString survived;
    QStringList lost;
    bool lossy() const { return !lost.isEmpty(); }
};

ExportSummary summarizeBake(const Bake &bake) {
    ExportSummary s;
    s.survived = QStringLiteral("%1 strokes, %2 fills")
                     .arg(bake.strokes.size())
                     .arg(bake.fills.size());
    if(bake.regionsFlattened > 0)
        s.lost << QStringLiteral("%1 regions flattened to paths").arg(bake.regionsFlattened);
    if(bake.constraintsDropped > 0)
        s.lost << QStringLiteral("%1 constraints").arg(bake.constraintsDropped);
    if(bake.parametersDropped > 0)
        s.lost << QStringLiteral("%1 parameters").arg(bake.parametersDropped);
    if(bake.tagsDropped > 0) s.lost << QStringLiteral("%1 tags").arg(bake.tagsDropped);
    if(bake.regionsBroken > 0)
        s.lost << QStringLiteral("%1 regions no longer enclose an area").arg(bake.regionsBroken);
    return s;
}

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
    applyGlyphPolicy();

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

bool Workspace::dirty() const { return modified_ || journal_.revision() != savedRevision_; }

bool Workspace::busy() const { return session_->asyncBusy(); }

void Workspace::notifyChanged() {
    kickPumpIfBusy();
    captureReports();
    // A ⋯ overflow pick asks the shell to reveal the inspector. Emitted before
    // changed() so the frame shows the panel, then the projection it reads is the
    // selection the pick just made.
    if(session_->presentation().overflowPicked) emit revealInspector();
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
    applyGlyphPolicy();
    // A replay is a fresh presentation of the recorded session; inspect mode from
    // whatever was showing before does not carry into it.
    inspectMode_ = false;
    savedRevision_ = journal_.revision();
    modified_ = false;  // a replay is the recorded session, clean until edited
    // The fresh session has async off; drop the flag so a later activate re-enables
    // it, matching loadFrom. A recorder outlives the session, so re-point it.
    asyncEnabled_ = false;
    reattachRecorder();
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
    emit changed();  // the Developer menu's recording state
}

void Workspace::reattachRecorder() {
    if(recorder_ == nullptr) return;
    // The session was replaced, so the recorder's accumulated steps belong to a
    // document that is no longer here. A script carries one starting document;
    // keeping the old steps under the new base would write a recording whose steps
    // reference ids the base does not hold. Restart the recording from the arrived
    // document instead — record-then-open keeps recording, of the opened document.
    recorder_->clear();
    recordedFrom_ = document_;
    session_->setRecorder(recorder_.get());
}

void Workspace::stopRecording() {
    if(recorder_ == nullptr) return;
    // Write now rather than at teardown, and detach so the destructor writes
    // nothing more — a stopped recording is a finished one. A recorder that was
    // attached to a since-replaced session is detached from whatever session is
    // current, which is the one it is attached to now (playScript/loadFrom
    // re-attach it), so this is always the right handle to clear.
    if(!recordPath_.isEmpty()) {
        GestureScript script;
        script.document = recordedFrom_;
        script.steps = recorder_->steps();
        std::string error;
        if(saveScriptFile(recordPath_.toStdString(), script, error)) {
            const QString text = QStringLiteral("Recorded %1 steps to %2")
                                     .arg(script.steps.size())
                                     .arg(QFileInfo(recordPath_).fileName());
            reports_->append(text);
            emit reportPosted(text);
        } else {
            // A file the user asked for that did not land is surfaced, not
            // swallowed to stderr — the same policy the export failure follows.
            std::fprintf(stderr, "%s\n", error.c_str());
            const QString text =
                QStringLiteral("Recording failed to write: %1").arg(QFileInfo(recordPath_).fileName());
            reports_->append(text);
            emit reportPosted(text);
        }
    }
    session_->setRecorder(nullptr);
    recorder_.reset();
    recordPath_.clear();
    emit changed();
}

LoadResult Workspace::loadFrom(const std::string &text, const QString &path) {
    Document loaded;
    const LoadResult result = deserialize(text, loaded);
    if(!result) return result;  // untouched on failure, exactly as the loader promises

    document_ = std::move(loaded);
    journal_.clear();
    session_ = std::make_unique<Session>(document_, journal_);
    applyGlyphPolicy();
    // Inspect mode is transient and belongs to the session being replaced, not to
    // the document arriving. A file opened into a tab left in inspect mode would
    // otherwise land inert; a fresh document is a fresh editing session.
    inspectMode_ = false;
    savedRevision_ = journal_.revision();
    modified_ = false;  // a loaded document is the file on disk, clean until edited
    filePath_ = path;
    // A fresh document asks for a re-fit: clear the latch so the next viewport
    // sync frames it. The sidecar, loaded next by the manager, layers pan and
    // zoom over that fitted base.
    view_ = ViewState{};
    asyncEnabled_ = false;
    // A recorder outlives the session, so a record-then-open keeps recording — of
    // the opened document, from its start (reattachRecorder resets the steps).
    reattachRecorder();
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

void Workspace::adoptTraced(Document traced, const QString &report) {
    document_ = std::move(traced);
    journal_.clear();
    session_ = std::make_unique<Session>(document_, journal_);
    applyGlyphPolicy();
    inspectMode_ = false;
    // No file: an import is new authored content, not a reopened one. Saving it
    // routes through Save As, the same as any untitled document.
    filePath_.clear();
    savedRevision_ = journal_.revision();
    // But not clean: the trace is unsaved geometry, and a clean untitled tab reads
    // as a pristine scratch tab the next Open would reuse and discard. modified_
    // holds dirty until the user saves, which is when it earns a file.
    modified_ = true;
    // A fresh document asks for a re-fit, exactly as loadFrom does.
    view_ = ViewState{};
    asyncEnabled_ = false;
    reattachRecorder();
    // Resync the report baseline to the traced session before the trace report is
    // appended, so the import's own count is the only thing this operation logs.
    lastReportSnapshot_ = snapshotReports();
    reports_->append(report);
    emit reportPosted(report);
    emit viewReset();
    emit changed();
}

std::string Workspace::serializeDocument() const { return serialize(document_); }

void Workspace::markSaved(const QString &path) {
    filePath_ = path;
    savedRevision_ = journal_.revision();
    // A save is what clears imported-but-never-on-disk content: it now has a file
    // and a revision to be past, so ordinary journal-revision dirtiness takes over.
    modified_ = false;
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
    background_ = s.background;
    showAllFrames_ = s.showAllFrames;
    extensions_ = s.extensions;
    gridVisible_ = s.gridVisible;
    // The saved framing only reaches the canvas when the viewport is rebuilt, and
    // that happens in SketchView::syncViewport, which the canvas runs on
    // viewReset. loadFrom already emitted a viewReset, but against the fitted base
    // it set before this — so without a second one here the pan and zoom just
    // loaded never take, and the tab opens at the fit rather than the saved view.
    emit viewReset();
    emit changed();
}

void Workspace::writeSidecarTo(const std::string &path) const {
    Sidecar s;
    s.panX = view_.pan.x();
    s.panY = view_.pan.y();
    s.zoom = view_.zoom;
    s.background = background_;
    s.showAllFrames = showAllFrames_;
    s.extensions = extensions_;
    s.gridVisible = gridVisible_;
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
    // Entity counts per layer, computed once for the whole panel: the badge the
    // layers list carries beside the visible and lock toggles.
    std::map<uint32_t, int> counts;
    for(const EntityRecord &e : session_->document().entities().records()) counts[e.layer.value()]++;
    const uint32_t active = session_->activeLayer().value();

    QVariantList out;
    for(LayerId id : layerOrder(session_->document())) {
        const LayerRecord *layer = session_->document().layers().find(id);
        if(layer == nullptr) continue;
        QVariantMap row;
        row["id"] = static_cast<int>(layer->id.value());
        row["name"] = QString::fromStdString(layer->name);
        row["visible"] = layer->visible;
        row["locked"] = layer->locked;
        row["active"] = layer->id.value() == active;
        const auto it = counts.find(layer->id.value());
        row["count"] = it != counts.end() ? it->second : 0;
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

QVariantList Workspace::relations() const {
    QVariantList out;
    for(const Session::ConstraintRow &r : session_->constraintRows()) {
        QVariantMap m;
        m[QStringLiteral("id")] = static_cast<int>(r.id.value());
        m[QStringLiteral("kind")] = QString::fromStdString(r.name);
        m[QStringLiteral("operands")] = QString::fromStdString(r.operands);
        m[QStringLiteral("valued")] = r.valued;
        m[QStringLiteral("value")] = r.value;
        m[QStringLiteral("driving")] = r.driving;
        m[QStringLiteral("hasAlternative")] = r.hasAlternative;
        m[QStringLiteral("conflicting")] = r.conflicting;
        m[QStringLiteral("redundant")] = r.redundant;
        m[QStringLiteral("frozen")] = r.frozenByLock;
        out.append(m);
    }
    return out;
}

QVariantMap Workspace::appearance() const {
    const Session::StyleAppearance a = session_->resolvedAppearance();
    QVariantMap m;
    m[QStringLiteral("any")] = a.any;
    m[QStringLiteral("entities")] = static_cast<int>(a.entities);
    m[QStringLiteral("regions")] = static_cast<int>(a.regions);
    m[QStringLiteral("strokeColor")] = argbHex(a.strokeColor);
    m[QStringLiteral("strokeColorValue")] = static_cast<double>(a.strokeColor);
    m[QStringLiteral("strokeMixed")] = a.strokeMixed;
    m[QStringLiteral("fillColor")] = argbHex(a.fillColor);
    m[QStringLiteral("fillColorValue")] = static_cast<double>(a.fillColor);
    m[QStringLiteral("fillMixed")] = a.fillMixed;
    m[QStringLiteral("strokeWidth")] = a.strokeWidth;
    m[QStringLiteral("strokeWidthExpr")] = a.strokeWidthExpr;
    m[QStringLiteral("strokeWidthMixed")] = a.strokeWidthMixed;
    m[QStringLiteral("opacity")] = a.opacity;
    m[QStringLiteral("opacityExpr")] = a.opacityExpr;
    m[QStringLiteral("opacityMixed")] = a.opacityMixed;
    m[QStringLiteral("filled")] = a.filled;
    m[QStringLiteral("filledMixed")] = a.filledMixed;
    return m;
}

QVariantList Workspace::namedStyles() const {
    QVariantList out;
    for(const Session::NamedStyle &s : session_->namedStyles()) {
        QVariantMap m;
        m[QStringLiteral("id")] = static_cast<int>(s.id.value());
        m[QStringLiteral("name")] = QString::fromStdString(s.name);
        m[QStringLiteral("strokeColor")] = argbHex(s.strokeColor);
        m[QStringLiteral("fillColor")] = argbHex(s.fillColor);
        m[QStringLiteral("strokeWidth")] = s.strokeWidth;
        m[QStringLiteral("opacity")] = s.opacity;
        m[QStringLiteral("filled")] = s.filled;
        m[QStringLiteral("usage")] = static_cast<int>(s.usage);
        out.append(m);
    }
    return out;
}

QVariantList Workspace::parameters() const {
    QVariantList out;
    for(const Session::ParameterInfo &p : session_->parameters()) {
        QVariantMap m;
        m[QStringLiteral("id")] = static_cast<int>(p.id.value());
        m[QStringLiteral("name")] = QString::fromStdString(p.name);
        m[QStringLiteral("expression")] = QString::fromStdString(p.expression);
        m[QStringLiteral("value")] = p.value;
        m[QStringLiteral("evaluable")] = p.evaluable;
        m[QStringLiteral("usage")] = static_cast<int>(p.usage);
        out.append(m);
    }
    return out;
}

QVariantList Workspace::axisReferences() const {
    QVariantList out;
    for(const Session::AxisReference &r : session_->axisReferences()) {
        QVariantMap m;
        m[QStringLiteral("id")] = static_cast<int>(r.id.value());
        m[QStringLiteral("kind")] =
            r.kind == ConstraintKind::Horizontal ? QStringLiteral("horizontal")
                                                  : QStringLiteral("vertical");
        m[QStringLiteral("frame")] = static_cast<int>(r.frame.value());
        m[QStringLiteral("frameName")] = QString::fromStdString(r.frameName);
        out.append(m);
    }
    return out;
}

QVariantList Workspace::rectanglePanels() const { return rectangles(); }

QVariantList Workspace::history() const {
    QVariantList out;
    for(const std::string &label : session_->historyLabels()) {
        out.append(QString::fromStdString(label));
    }
    return out;
}

int Workspace::historyPosition() const { return static_cast<int>(session_->historyPosition()); }
QString Workspace::undoLabel() const { return QString::fromStdString(session_->undoLabel()); }
QString Workspace::redoLabel() const { return QString::fromStdString(session_->redoLabel()); }
int Workspace::activeLayer() const { return static_cast<int>(session_->activeLayer().value()); }

// ---- U2 canvas-depth projections and presentation toggles ----

QVariantMap Workspace::glyphReadout() const {
    // Computed once here — the whole overlay layout — rather than through two
    // properties each running it, because the coarse changed() cadence fires on
    // every hover-move and the HUD reads this in both a visibility and a text
    // binding.
    const Session::GlyphReadout r = session_->glyphReadout();
    QVariantMap m;
    m[QStringLiteral("shown")] = static_cast<int>(r.shown);
    m[QStringLiteral("total")] = static_cast<int>(r.total);
    return m;
}
int Workspace::directionClassCount() const {
    return static_cast<int>(session_->directionClassCount());
}

QString Workspace::backgroundHex() const {
    return background_ != 0 ? argbHex(background_) : QString();
}

void Workspace::setBackground(const QString &hex) {
    const QColor colour(hex);
    if(!colour.isValid()) return;
    // Packed 0xAARRGGBB, the same layout render and the style colours use. An
    // opaque pick is forced opaque so it can never collide with the zero
    // sentinel that means "theme default".
    const uint32_t alpha = colour.alpha() == 0 ? 0xffu : static_cast<uint32_t>(colour.alpha());
    const uint32_t packed = (alpha << 24) | (static_cast<uint32_t>(colour.red()) << 16) |
                            (static_cast<uint32_t>(colour.green()) << 8) |
                            static_cast<uint32_t>(colour.blue());
    if(packed == background_) return;
    background_ = packed;
    emit changed();
}

void Workspace::clearBackground() {
    if(background_ == 0) return;
    background_ = 0;
    emit changed();
}

void Workspace::setShowAllFrames(bool on) {
    if(showAllFrames_ == on) return;
    showAllFrames_ = on;
    emit changed();
}

void Workspace::setExtensions(bool on) {
    if(extensions_ == on) return;
    extensions_ = on;
    emit changed();
}

void Workspace::setGridVisible(bool on) {
    if(gridVisible_ == on) return;
    gridVisible_ = on;
    emit changed();
}

void Workspace::setInspectMode(bool on) {
    if(inspectMode_ == on) return;
    inspectMode_ = on;
    // Entering the mode ends any gesture in flight, keeping the tool: a drag or
    // placement begun a moment before would otherwise have its release swallowed
    // by the mode's input inertness and dangle.
    if(on) session_->cancelInFlight();
    emit changed();
}

void Workspace::toggleInspectMode() { setInspectMode(!inspectMode_); }

void Workspace::applyGlyphPolicy() {
    // A multiplier over the policy default rather than an absolute, so the
    // default lives in one place — the policy struct — and the setting only
    // scales it.
    session_->glyphPolicy().density = GlyphPolicy{}.density * glyphDensity_;
}

void Workspace::setGlyphDensity(double multiplier) {
    if(!(multiplier > 0.0) || multiplier == glyphDensity_) return;
    glyphDensity_ = multiplier;
    applyGlyphPolicy();
    emit changed();
}

bool Workspace::parameterWouldCycle(int id, const QString &expression) const {
    return session_->wouldParameterCycle(ParameterId(static_cast<uint32_t>(id)),
                                         expression.toStdString());
}

void Workspace::walkHistory(int position) {
    if(position < 0 || inspectMode_) return;  // walking the journal is undo/redo
    const size_t target = static_cast<size_t>(position);
    // Repeated single steps through the ordinary undo/redo path, so branch
    // fidelity holds — a jump would bypass the mechanism that keeps it. The guard
    // is a runaway backstop, never reached in practice.
    size_t guard = 0;
    while(session_->historyPosition() > target && session_->canUndo() && guard++ < 1000000) {
        session_->handle(Key::Undo);
    }
    while(session_->historyPosition() < target && session_->canRedo() && guard++ < 1000000) {
        session_->handle(Key::Redo);
    }
    // A walk is navigation, not an edit: resync so a report the landed state
    // recomputes is not attributed to the walk. See undo()/redo().
    lastReportSnapshot_ = snapshotReports();
    notifyChanged();
}

QVariantList Workspace::palette(const QString &query) const {
    QVariantList out;
    const std::string text = query.toStdString();
    for(const SurfaceEntry &entry : paletteEntries(*session_, text)) out.append(entryOf(entry));
    return out;
}

bool Workspace::run(const QString &name, const QVariantMap &arguments) {
    // Inspect mode is document-as-output, and the spec's premise is that no edit
    // can occur inside it — that premise is what makes it unrecorded presentation
    // state. run() is the one entrance every editing surface dispatches through
    // (toolbars, palette, inspector, context menus), so refusing here makes the
    // mode genuinely edit-proof rather than only inert to canvas input. The
    // presentation toggles and navigation do not come through run().
    if(inspectMode_) return false;
    // Routed by the action's own schema rather than by what the QVariant happens
    // to coerce to: a rename whose new name spells a number ("2024") is a string,
    // not a value, and only the parameter table can say which channel it reads.
    const Action *action = findAction(name.toStdString());
    auto isText = [&](const std::string &key) {
        if(action == nullptr) return false;
        for(const ActionParameter &p : action->parameters) {
            if(p.name == key) return p.text;
        }
        return false;
    };

    ActionArguments args;
    for(auto it = arguments.begin(); it != arguments.end(); ++it) {
        const std::string key = it.key().toStdString();
        if(isText(key)) {
            args.setText(key, it.value().toString().toStdString());
            continue;
        }
        bool ok = false;
        const double value = it.value().toDouble(&ok);
        if(ok) args.set(key, value);
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

void Workspace::selectRelation(int id, bool additive) {
    session_->selectConstraint(ConstraintId(static_cast<uint32_t>(id)), additive);
    notifyChanged();
}

void Workspace::selectReported(const QVariantList &entities, const QVariantList &constraints) {
    // select() clears the whole selection and sets the entities, so an empty list
    // clears; the constraints are then added additively. A report names one
    // vocabulary or the other in practice, but handling both keeps it honest.
    std::vector<EntityId> ids;
    for(const QVariant &v : entities) ids.push_back(EntityId(static_cast<uint32_t>(v.toInt())));
    session_->select(std::move(ids));
    for(const QVariant &v : constraints) {
        session_->selectConstraint(ConstraintId(static_cast<uint32_t>(v.toInt())), /*additive=*/true);
    }
    notifyChanged();
}

QVariantMap Workspace::exportReport(double, int) const {
    // The bake is what the loss is computed from — the same value the write turns
    // into a file — so the report describes exactly what will be written. Margin
    // and precision scale coordinates, not the loss, so they are not read here; the
    // dialog collects them and hands them to exportSvg. The wording is built once,
    // in summarizeBake, so the preview and the after-write log agree.
    const ExportSummary s = summarizeBake(bakeForExport(document_, session_->pose()));
    QVariantMap m;
    m[QStringLiteral("survived")] = s.survived;
    m[QStringLiteral("lost")] = s.lost.join(QStringLiteral(", "));
    m[QStringLiteral("lossy")] = s.lossy();
    return m;
}

bool Workspace::exportSvg(const QString &path, double margin, int precision) {
    // Baked once, from the current pose: the background colour is not in the bake,
    // so it never reaches the bytes — the viewing aid stays a viewing aid.
    const Bake bake = bakeForExport(document_, session_->pose());
    SvgOptions options;
    options.margin = std::max(0.0, margin);  // a negative margin is not a framing
    options.precision = precision;
    const std::string svg = writeSvg(bake, options);

    QDir().mkpath(QFileInfo(path).absolutePath());
    std::ofstream out(path.toStdString());
    if(!out) {
        emit exportFailed(path);
        return false;
    }
    out << svg;
    // Close and check: a disk-full short write surfaces only on the flush, so an
    // export that only tested the open is not an export that landed.
    out.close();
    if(!out) {
        emit exportFailed(path);
        return false;
    }

    // The loss report, the same no-silent-changes surface every producer reaches:
    // what survived and what the bake cost, worded by the shared summarizer so the
    // log cannot disagree with the preview the dialog showed.
    const ExportSummary s = summarizeBake(bake);
    QString text = QStringLiteral("Exported %1 to %2").arg(s.survived).arg(QFileInfo(path).fileName());
    if(s.lossy()) text += QStringLiteral(" · dropped ") + s.lost.join(QStringLiteral(", "));
    reports_->append(text, QStringLiteral("export"));
    emit reportPosted(text);
    return true;
}

void Workspace::undo() {
    if(inspectMode_) return;  // no edit occurs in inspect mode, undo included
    session_->handle(Key::Undo);
    // Navigation is not an edit: resync the baseline so a report the landed state
    // recomputes (a hidden-influence present there, say) is not posted as though
    // this undo caused it. The event was already reported when the edit first ran.
    lastReportSnapshot_ = snapshotReports();
    notifyChanged();
}
void Workspace::redo() {
    if(inspectMode_) return;
    session_->handle(Key::Redo);
    lastReportSnapshot_ = snapshotReports();
    notifyChanged();
}
void Workspace::deleteSelection() {
    if(inspectMode_) return;
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
    s.deletionSerial = p.deletionSerial;
    s.structureSerial = p.structureSerial;
    s.deletedEntities = p.deletedEntities;
    s.deletedRelations = p.deletedRelations;
    s.degraded = p.degraded;
    s.transformError = static_cast<int>(st.transformError);
    s.compoundError = static_cast<int>(st.compoundError);
    s.structureOp = static_cast<int>(st.op);
    s.moved = st.moved;
    s.retargeted = st.retargeted;
    s.rescaled = st.rescaled;
    s.straddling = st.straddling;
    s.copied = st.copied;
    s.droppedRelations = st.droppedRelations;
    s.droppedRegions = st.droppedRegions;
    s.droppedTags = st.droppedTags;
    s.frame = st.frame.value();
    if(p.crossing) {
        s.crossingA = p.crossing->first.value();
        s.crossingB = p.crossing->second.value();
    }
    s.styleForked = p.styleForked;
    s.forkedStyle = p.forkedStyle.value();
    s.hiddenCount = p.hiddenInfluences.size();
    for(EntityId id : p.hiddenInfluences) s.hiddenKey = s.hiddenKey * 1000003u + id.value();
    // The downgrade offer is set only by the real impose and set-value paths, so a
    // present optional here is always an edit that refused a driving imposition —
    // the const preview query leaves it untouched.
    s.downgraded = p.downgrade.has_value();
    if(p.downgrade) {
        s.downgradeKind = static_cast<int>(p.downgrade->kind);
        s.downgradeAssignment = p.downgrade->assignment;
    }
    for(ConstraintId id : p.conflicting) s.conflictKey = s.conflictKey * 1000003u + id.value();
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
    // The records an entry names, for click-to-select. Accumulated across the
    // groups a single edit produced, so a click on the joined entry lands the user
    // on everything it is about.
    QVariantList entityIds;
    QVariantList constraintIds;

    // Deletion group: emitted only when its own fields changed. Its records are
    // gone, so it names none — a deletion is not click-to-select.
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
            // The frame the retarget created, named so the entry points at the
            // construction geometry the cluster now belongs to.
            if(st.frame.valid()) entityIds.append(static_cast<int>(st.frame.value()));
        }
        if(st.rescaled > 0) parts << QStringLiteral("Rescaled %1 dimensions").arg(st.rescaled);
        if(st.straddling > 0) {
            parts << QStringLiteral("%1 dimensions reach outside and will resist").arg(st.straddling);
        }
        // A transform that only moved geometry still owes the log an entry. The
        // copied count is named by the operation that produced it, because a
        // distribute copied nothing — its records are the construction gaps it
        // added — and reading "Copied N" off the count alone would be wrong.
        if(st.moved > 0) parts << QStringLiteral("Moved %1 shapes").arg(st.moved);
        if(st.op == Presentation::StructureOp::Distribute) {
            parts << QStringLiteral("Distributed the selection evenly");
        } else if(st.copied > 0) {
            parts << (st.op == Presentation::StructureOp::Mirror
                          ? QStringLiteral("Mirrored %1 shapes").arg(st.copied)
                          : QStringLiteral("Copied %1 shapes").arg(st.copied));
        }
        if(st.droppedRelations > 0 || st.droppedRegions > 0 || st.droppedTags > 0) {
            parts << QStringLiteral("Dropped %1 relations, %2 fills, %3 tags at the boundary")
                         .arg(st.droppedRelations)
                         .arg(st.droppedRegions)
                         .arg(st.droppedTags);
        }
    }

    // Crossing: keyed on the edge pair, so a crossing on a different pair reports
    // even while an earlier one is still standing. It names the two edges.
    if(!lastReportSnapshot_.crossingEquals(now) && p.crossing) {
        parts << QStringLiteral("Edges cross: intersection points are needed to fill this");
        entityIds.append(static_cast<int>(p.crossing->first.value()));
        entityIds.append(static_cast<int>(p.crossing->second.value()));
    }

    // Forked style: the edit gave the selection its own style rather than mutating
    // the shared one it was editing. The selection is already the affected
    // geometry, so the entry names no extra record.
    if(!lastReportSnapshot_.styleEquals(now) && p.styleForked > 0) {
        parts << QStringLiteral("Forked style: this selection now has its own style");
    }

    // Hidden influence: invisible geometry moved something visible. The invisible
    // operands are what the entry points at, so hidden structure can be found.
    if(!lastReportSnapshot_.hiddenEquals(now) && !p.hiddenInfluences.empty()) {
        parts << QStringLiteral("%1 hidden shapes influenced this edit")
                     .arg(p.hiddenInfluences.size());
        for(EntityId id : p.hiddenInfluences) entityIds.append(static_cast<int>(id.value()));
    }

    // Refused imposition: a driving relation could not hold, so the reference
    // measurement is on offer. The conflicting relations are what it disagreed
    // with, walkable from the entry.
    if(!lastReportSnapshot_.downgradeEquals(now) && p.downgrade) {
        QString s = QStringLiteral("Imposition refused: cannot hold as driving; reference on offer");
        if(p.conflictAttributed && !p.conflicting.empty()) {
            s += QStringLiteral(" · conflicts with %1").arg(p.conflicting.size());
        }
        parts << s;
        for(ConstraintId id : p.conflicting) constraintIds.append(static_cast<int>(id.value()));
    }

    lastReportSnapshot_ = now;
    if(parts.isEmpty()) return;
    const QString text = parts.join(QStringLiteral("  ·  "));
    reports_->append(text, QString(), entityIds, constraintIds);
    emit reportPosted(text);
}

}  // namespace paroculus
