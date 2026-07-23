// One open document, with everything that is per-document.
//
// This is the unit a tab is a view of. It owns what SketchView used to own as
// stand-in furniture — the document, its journal, its session, the view framing,
// the recorder — and exposes the projections QML binds and the entrances QML
// dispatches through. SketchView shrinks to what it alone can do: translate Qt
// events, paint one surface, and drive the pan. The registry-through-run()
// discipline is untouched: QML still mutates only through run()/invokeAction, and
// every read is a Q_PROPERTY projection or a model.
//
// A QObject, and referenced-but-not-created from QML: the manager creates
// workspaces as documents open, and QML binds to whichever the active tab names.
//
// Owns the two timers the never-blocks contract needs — the async pump and the
// script replay stepper — and emits one coarse changed() when anything a surface
// reads may have moved, matching the single changed() cadence the shell keeps for
// now. Dirty tracking rides the journal revision, so a save prompt cannot miss a
// redo-tail truncation that lands at the saved depth with different bytes.
#pragma once

#include <memory>
#include <vector>

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "core/document.h"
#include "core/persist.h"
#include "core/pose.h"
#include "core/undo.h"
#include "interact/script.h"
#include "interact/session.h"
#include "render/view.h"
#include "shell/models/reports.h"

namespace paroculus {

class Workspace : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Workspaces are opened through the WorkspaceManager, not constructed in QML")

    // What the tab shows: a title from the file name (or "untitled-N"), the path
    // once saved, and a dirty dot from the journal revision versus the last save.
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString filePath READ filePath NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)

    // The status mega-string is gone: each fragment it concatenated now has a
    // typed projection with its own home, per the spec's mapping table. The
    // readouts land in the status bar and HUD, the tool fragment in the tool
    // options row, the numeric field in the numeric entry, and the report-shaped
    // fragments in the reports model and its toast.
    Q_PROPERTY(QString selectionText READ selectionText NOTIFY changed)
    Q_PROPERTY(int dof READ dof NOTIFY changed)
    Q_PROPERTY(QString solveStatus READ solveStatus NOTIFY changed)
    Q_PROPERTY(double zoom READ zoom NOTIFY changed)
    Q_PROPERTY(double solveMilliseconds READ solveMilliseconds NOTIFY changed)
    Q_PROPERTY(QVariantList strip READ strip NOTIFY changed)
    Q_PROPERTY(QVariantList layers READ layers NOTIFY changed)
    Q_PROPERTY(int hiddenInfluences READ hiddenInfluences NOTIFY changed)
    Q_PROPERTY(int brokenRegions READ brokenRegions NOTIFY changed)
    Q_PROPERTY(int brokenTags READ brokenTags NOTIFY changed)
    // The tool fragment: the active tool's name and its live parameters, for the
    // tool options row under the toolbar. Empty while Select is the tool.
    Q_PROPERTY(QString toolName READ toolName NOTIFY changed)
    Q_PROPERTY(QVariantList toolParameters READ toolParameters NOTIFY changed)
    // The numeric field in flight: {active, name, text}, for the numeric entry.
    Q_PROPERTY(QVariantMap numericEntry READ numericEntry NOTIFY changed)
    // Canvas-tint diagnostics that also earn a calm status-bar note: a drag the
    // constraints are resisting, and geometry the solve pushed off-screen.
    Q_PROPERTY(bool saturated READ saturated NOTIFY changed)
    Q_PROPERTY(int resisting READ resisting NOTIFY changed)
    Q_PROPERTY(bool rippledOffScreen READ rippledOffScreen NOTIFY changed)
    // The fill offers a placement surfaced, as notes near the readouts.
    Q_PROPERTY(int closedLoopEdges READ closedLoopEdges NOTIFY changed)
    Q_PROPERTY(bool crossing READ crossing NOTIFY changed)
    // The append-only session log, the reports panel's and the toast's source.
    Q_PROPERTY(paroculus::ReportsModel *reports READ reports CONSTANT)
    // A calm busy glyph in the status bar while a component solves off-thread.
    // Last coherent pose stays on screen; this only says work is outstanding.
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    // The registry row of the tool in force, so a projected tools toolbar can
    // hold the active tool visibly down. Tracks the session, never a local toggle.
    Q_PROPERTY(QString activeToolAction READ activeToolAction NOTIFY changed)
    // The whole action table with metadata and live applicability. A property
    // rather than a call so a menu re-dims when the selection changes, the same
    // reactive cadence strip and layers ride.
    Q_PROPERTY(QVariantList actions READ actions NOTIFY changed)

    // The deep-panel projections U1 binds, each a view of a session query,
    // rebuilt on changed() like the rest. The inspector reads relations,
    // appearance and rectanglePanels; the style toolbar reads appearance and
    // namedStyles; the parameters panel reads parameters; the history panel reads
    // history and historyPosition; the layers panel reads activeLayer for its
    // highlight. None mutate — every edit still goes through run().
    Q_PROPERTY(QVariantList relations READ relations NOTIFY changed)
    Q_PROPERTY(QVariantMap appearance READ appearance NOTIFY changed)
    Q_PROPERTY(QVariantList namedStyles READ namedStyles NOTIFY changed)
    Q_PROPERTY(QVariantList parameters READ parameters NOTIFY changed)
    Q_PROPERTY(QVariantList axisReferences READ axisReferences NOTIFY changed)
    Q_PROPERTY(QVariantList rectanglePanels READ rectanglePanels NOTIFY changed)
    Q_PROPERTY(QVariantList history READ history NOTIFY changed)
    Q_PROPERTY(int historyPosition READ historyPosition NOTIFY changed)
    Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY changed)
    Q_PROPERTY(QString redoLabel READ redoLabel NOTIFY changed)
    Q_PROPERTY(int activeLayer READ activeLayer NOTIFY changed)

    // The U2 canvas-depth projections. The HUD readouts — how many relations the
    // overlay drew of how many it could, and the declared-direction-class count —
    // and the per-workspace presentation toggles the canvas and the View menu
    // share. All presentation: none reaches the document or is recorded, and each
    // rides the sidecar rather than the journal.
    // One map {shown, total} rather than two properties, so the HUD reads it once
    // per changed() — each read runs the whole overlay layout, and the coarse
    // changed() fires on every hover-move.
    Q_PROPERTY(QVariantMap glyphReadout READ glyphReadout NOTIFY changed)
    Q_PROPERTY(int directionClassCount READ directionClassCount NOTIFY changed)
    Q_PROPERTY(bool hasBackground READ hasBackground NOTIFY changed)
    Q_PROPERTY(QString background READ backgroundHex NOTIFY changed)
    Q_PROPERTY(bool showAllFrames READ showAllFrames NOTIFY changed)
    Q_PROPERTY(bool extensions READ extensions NOTIFY changed)
    Q_PROPERTY(bool gridVisible READ gridVisible NOTIFY changed)
    Q_PROPERTY(bool inspectMode READ inspectMode NOTIFY changed)

    // The U3 developer-surface projections. Whether a recording is being written,
    // for the Developer menu's start/stop state, and the replay progress the
    // overlay reads while a script plays into this workspace. All presentation:
    // recording is teardown-written and replay is transient, neither touching the
    // document or the journal.
    Q_PROPERTY(bool recording READ recording NOTIFY changed)
    Q_PROPERTY(bool replaying READ playing NOTIFY changed)
    Q_PROPERTY(int replayStep READ replayStep NOTIFY changed)
    Q_PROPERTY(int replayTotal READ replayTotal NOTIFY changed)

public:
    explicit Workspace(QObject *parent = nullptr);
    ~Workspace() override;

    // ---- C++-facing accessors, for SketchView and the manager ----
    Session &session() { return *session_; }
    const Session &session() const { return *session_; }
    Document &document() { return document_; }
    const Document &document() const { return document_; }
    UndoJournal &journal() { return journal_; }
    ViewState &view() { return view_; }
    const ViewState &view() const { return view_; }
    // The pose a hovered relation would produce, for the canvas ghost. Empty when
    // nothing is being previewed.
    const std::vector<SeedSpan> &ghostPose() const { return ghostPose_; }

    // A running script owns the viewport, so SketchView must not re-fit it.
    bool playing() const { return playing_; }

    // ---- QML property reads ----
    QString title() const;
    QString filePath() const { return filePath_; }
    bool dirty() const;
    QString selectionText() const;
    int dof() const;
    QString solveStatus() const;
    double zoom() const;
    double solveMilliseconds() const;
    QVariantList strip() const;
    QVariantList layers() const;
    int hiddenInfluences() const;
    int brokenRegions() const;
    int brokenTags() const;
    QString toolName() const;
    QVariantList toolParameters() const;
    QVariantMap numericEntry() const;
    bool saturated() const;
    int resisting() const;
    bool rippledOffScreen() const;
    int closedLoopEdges() const;
    bool crossing() const;
    ReportsModel *reports() const { return reports_; }
    bool busy() const;
    QString activeToolAction() const;
    // The whole action table as the menus and toolbars project it: every row with
    // its metadata — name, title, binding, category, description — and whether it
    // applies to the current selection right now. Menus group by category, dim
    // what does not apply, and show bindings, all from this one projection, so the
    // menu bar is a view of the registry rather than a second list that drifts.
    QVariantList actions() const;

    QVariantList relations() const;
    QVariantMap appearance() const;
    QVariantList namedStyles() const;
    QVariantList parameters() const;
    QVariantList axisReferences() const;
    QVariantList rectanglePanels() const;
    QVariantList history() const;
    int historyPosition() const;
    QString undoLabel() const;
    QString redoLabel() const;
    int activeLayer() const;

    QVariantMap glyphReadout() const;
    int directionClassCount() const;
    bool hasBackground() const { return background_ != 0; }
    QString backgroundHex() const;
    bool showAllFrames() const { return showAllFrames_; }
    bool extensions() const { return extensions_; }
    bool gridVisible() const { return gridVisible_; }
    bool inspectMode() const { return inspectMode_; }
    bool recording() const { return recorder_ != nullptr; }
    int replayStep() const { return static_cast<int>(scriptStep_); }
    int replayTotal() const { return static_cast<int>(script_.steps.size()); }
    // The packed background for render, and the axis frames the canvas draws.
    // C++-facing, for SketchView: the canvas reads these to fill the Adornment.
    uint32_t backgroundColor() const { return background_; }
    // The relation whose operands the inspector is hovering, or a null id when
    // none. C++-facing, read by SketchView into the Adornment.
    ConstraintId hoveredRelation() const { return hoveredRelation_; }

    // ---- Presentation toggles (unrecorded, sidecar-persisted) ----
    // Each updates transient view state and repaints; none touches the document
    // or the journal, so a script is unaffected and the determinism property
    // holds. They persist to the sidecar on save, exactly as pan and zoom do.
    Q_INVOKABLE void setBackground(const QString &hex);
    Q_INVOKABLE void clearBackground();
    Q_INVOKABLE void setShowAllFrames(bool on);
    Q_INVOKABLE void setExtensions(bool on);
    Q_INVOKABLE void setGridVisible(bool on);
    // Inspect mode is transient and not even sidecar-persisted: it is a momentary
    // way of looking, per the spec, not a preference. Esc and the toggle exit.
    Q_INVOKABLE void setInspectMode(bool on);
    Q_INVOKABLE void toggleInspectMode();

    // The glyph-density preference (application settings, display-only). A
    // multiplier over the policy default, applied to the session's glyph policy
    // and re-applied whenever the session is replaced, so a loaded or replayed
    // document honours it too.
    Q_INVOKABLE void setGlyphDensity(double multiplier);

    // ---- Interchange (U3) ----
    // The loss the bake would cost, computed from the bake and shown before a
    // write so the lossy step is consented to rather than discovered. Const: a
    // preview reads the document and touches no file. The map carries the geometry
    // that survives (strokes, fills) and the structure that does not (constraints,
    // parameters, flattened and broken regions, tags), the counts the bake already
    // holds. Margin and precision are the SvgOptions the dialog collects; they
    // scale coordinates, not the loss, and are carried so one call answers the
    // whole dialog.
    Q_INVOKABLE QVariantMap exportReport(double margin, int precision) const;
    // Bakes the visible document at its current pose and writes the SVG to `path`,
    // checked end to end — a short write is a failed export, surfaced loudly
    // through exportFailed rather than a truncated file passed off as done. The
    // background colour is never baked, so it never reaches the bytes. On success
    // the loss report joins the reports model and flashes as a toast, the same
    // no-silent-changes surface every producer reaches. Returns whether it landed.
    Q_INVOKABLE bool exportSvg(const QString &path, double margin, int precision);

    // Selects the records a reports entry names, so a click on it lands the user on
    // the geometry it is about — "where it names records, click-to-select." Not
    // recorded: a report is a recall surface over what an edit already did, the
    // same seam selectRelation uses. Empty lists clear the selection.
    Q_INVOKABLE void selectReported(const QVariantList &entities, const QVariantList &constraints);

    // Stops an in-progress recording and writes the file now, rather than waiting
    // for teardown — the Developer menu's Stop. Idempotent: a stop with nothing
    // recording does nothing. After it, the destructor writes nothing, since the
    // recorder is detached.
    Q_INVOKABLE void stopRecording();

    // ---- QML entrances (the registry, and the state machine) ----
    Q_INVOKABLE QVariantList palette(const QString &query) const;
    Q_INVOKABLE bool run(const QString &name, const QVariantMap &arguments = {});
    Q_INVOKABLE QString previewOf(const QString &name, int assignment);
    Q_INVOKABLE void clearPreview();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE QVariantList rectangles() const;

    // Selects a relation from the inspector so an action can act on it. Selection
    // is presentation, not a document mutation, so it rides an invokable like undo
    // and delete rather than run() — the same seam those already use. Not
    // recorded: a script records the click, not the panel; the inspector is a
    // recall surface over what the click selected.
    Q_INVOKABLE void selectRelation(int id, bool additive = false);

    // Highlights a relation's operands on the canvas while its inspector row is
    // hovered, so what a constraint points to is legible by eye. Transient and
    // repaint-only: it emits highlightChanged() rather than changed(), because a
    // changed() rebuilds the relations list and destroys the very delegate being
    // hovered before its exit could clear this — the same hazard the imposition
    // ghost dodges. Not recorded, not the document's business.
    Q_INVOKABLE void setHoveredRelation(int id);
    Q_INVOKABLE void clearHoveredRelation();

    // Whether assigning `expression` to parameter `id` would close a cycle, so the
    // parameters panel can refuse inline at commit rather than after. A read, not
    // an edit: it computes the check without touching the document.
    Q_INVOKABLE bool parameterWouldCycle(int id, const QString &expression) const;
    // Walks the journal to a history position through the ordinary undo/redo path,
    // so branch fidelity holds. Not a jump: repeated single steps, which is what
    // clicking a history row means.
    Q_INVOKABLE void walkHistory(int position);
    // View framing (reset, zoom, fit) stays on SketchView: it needs the item
    // size and must re-sync the viewport, neither of which a workspace has.

    // ---- Lifecycle, driven by the manager ----
    // Replaces the document from serialized text. Returns the loader diagnostic;
    // on failure the workspace is untouched, exactly as the loader promises. The
    // path names where it came from and seeds title, dirty and the sidecar.
    LoadResult loadFrom(const std::string &text, const QString &path);
    // Adopts a traced document into this workspace: the other half of import,
    // where the manager has already read and traced the SVG. The document arrives
    // free of any file — an import is new authored content, not a reopened file —
    // so the workspace is left untitled and dirty, which keeps it from being taken
    // for a pristine scratch tab the next Open would reuse and discard. `report`
    // is the trace's count and the geometry-arrives-free statement, appended to the
    // reports model and flashed. The view re-fits to frame the trace.
    void adoptTraced(Document traced, const QString &report);

    // Serializes the current document. Const: a save is not an edit.
    std::string serializeDocument() const;
    // Marks the current revision as the saved one and records the path, so the
    // dirty dot clears and the title follows the new name.
    void markSaved(const QString &path);
    // The sidecar path for this workspace's document, or empty when unsaved.
    std::string sidecarPath() const;
    void loadSidecarFrom(const std::string &path);
    void writeSidecarTo(const std::string &path) const;

    // Adopts a recorded session and steps through it. The script's own document
    // and viewport apply, so what is shown is the session as recorded.
    void playScript(GestureScript script);
    // Captures everything the session does to a file written at teardown. The
    // starting document is kept alongside, since a recording without its starting
    // state replays into a different sketch.
    void startRecording(const QString &path);

    // Turns the async solve path on and off for this workspace, idempotently. The
    // manager enables it on the workspace it activates and disables it on the one
    // it leaves, so exactly one scheduler is alive — the active-workspace-only
    // posture the plan fixes for U0, before the cross-session gate audit a second
    // live scheduler would need. Disabling stops the pump and joins the workers.
    void enableAsync();
    void disableAsync();

    // Emits changed() after SketchView mutated the session through handle(), and
    // starts the async pump if a component went off-thread. The one place the
    // canvas tells the workspace a pointer event landed.
    void notifyChanged();

signals:
    void changed();
    // A transient highlight changed (the inspector's hovered relation), asking
    // the canvas to repaint without rebuilding any model — distinct from
    // changed(), which would destroy the hovered relation delegate. No model
    // binds to this; only SketchView does.
    void highlightChanged();
    // The session was replaced under a stable workspace pointer (a load into a
    // reused tab), so the canvas must re-sync the viewport for the new session
    // rather than assume a pointer change already did. Distinct from changed(),
    // which only repaints — a repaint through an unset viewport draws the
    // document at raw coordinates.
    void viewReset();
    // A no-silent-changes event was just logged, for the frame to flash as a
    // toast. The reports model is the memory; this is the notice.
    void reportPosted(const QString &text);
    // A per-anchor ⋯ overflow mark was just picked, so the frame reveals the
    // inspector — the ⋯ opens the crowd there, per the spec. The session already
    // selected the anchor's operand, so the inspector's relation list is filtered
    // to it the moment it is shown.
    void revealInspector();
    // An export could not be written — the destination did not open, or a short
    // write left it truncated. Surfaced as a toast, never a silent half-file: a
    // file the user asked for that did not land is exactly the no-silent-changes
    // policy's business.
    void exportFailed(const QString &path);

private:
    void stepScript();
    // Examines the report-shaped presentation fields after an interactive edit
    // and appends any newly-produced event to the reports model, toasting it.
    // Snapshot-diffed because the fields persist across operations rather than
    // resetting per refresh, so a bare read would re-surface a stale count on the
    // next unrelated edit.
    void captureReports();
    void pump();
    void kickPumpIfBusy();
    // Re-points an active recorder at the current session after a document swap.
    // A script has one starting document, so a recording that spanned an open
    // would replay its steps against the wrong geometry — the recorder is reset
    // and its base re-captured, restarting the recording from the arrived document.
    void reattachRecorder();

    Document document_;
    UndoJournal journal_;
    std::unique_ptr<Session> session_;
    ViewState view_;

    QString filePath_;
    // The revision the document was last saved at. dirty() is revision != this.
    // Zero for a fresh empty document, which reads clean until the first edit.
    uint64_t savedRevision_ = 0;
    // Content that has no journal revision to be past — a traced import is new
    // authored geometry with an empty journal — is still unsaved. This forces
    // dirty until a save clears it, so an import is not mistaken for a pristine
    // scratch tab and reused away by the next Open. Cleared by markSaved and by
    // any lifecycle that installs a saved-from-disk document (loadFrom, replay).
    bool modified_ = false;
    // The ordinal for an untitled document's tab label, assigned by the manager.
    int untitledOrdinal_ = 0;

    // The previewed pose held between a hover and the paint. Transient view
    // state: the session does not know it exists and the document is untouched.
    std::vector<SeedSpan> ghostPose_;
    // The relation whose row the inspector is hovering, so the canvas can tint
    // its operands. Transient, repaint-only, cleared whenever a changed() would
    // otherwise strand it — the same treatment ghostPose_ gets.
    ConstraintId hoveredRelation_;

    // U2 canvas-depth presentation state. Sidecar-persisted (all but inspect
    // mode), never journalled, never seen by a script — presentation, not edits.
    uint32_t background_ = 0;   // 0 == theme default
    bool showAllFrames_ = false;
    bool extensions_ = false;
    bool gridVisible_ = true;
    bool inspectMode_ = false;  // transient, not even sidecar-persisted
    // The glyph-density multiplier (application setting), applied to the session
    // glyph policy and re-applied when the session is replaced.
    double glyphDensity_ = 1.0;
    void applyGlyphPolicy();

    // Script playback, faithful to the recording's own viewport.
    GestureScript script_;
    size_t scriptStep_ = 0;
    bool playing_ = false;
    QTimer scriptTimer_;

    // Recording, written at teardown.
    std::unique_ptr<ScriptRecorder> recorder_;
    Document recordedFrom_;
    QString recordPath_;

    // The async pump, running only while a component owes a result.
    QTimer pumpTimer_;
    bool asyncEnabled_ = false;

    // The append-only report log, a child so QML can hold a stable pointer.
    ReportsModel *reports_ = nullptr;
    // The last-seen values of the report-shaped fields. These persist across
    // operations — each group is cleared only by its own operation kind, not by a
    // refresh — so captureReports must emit a fragment only when that group's own
    // fields changed, never merely because some other group did. The comparison
    // is therefore per group, and the crossing carries its edge-pair identity
    // rather than a bool, so a crossing on a different pair is a new event.
    struct ReportSnapshot {
        // A monotonic per-operation serial keys deletion and structure rather than
        // their count fields, so an identically-counted repeat (delete a lone
        // point, then another) is still a fresh event and not swallowed as a no-op.
        // The count fields ride along for the text only.
        size_t deletionSerial = 0, structureSerial = 0;
        size_t deletedEntities = 0, deletedRelations = 0, degraded = 0;
        int transformError = 0, compoundError = 0, structureOp = 0;
        size_t moved = 0;
        size_t retargeted = 0, rescaled = 0, straddling = 0, copied = 0;
        size_t droppedRelations = 0, droppedRegions = 0, droppedTags = 0;
        uint32_t frame = 0;
        uint64_t crossingA = 0, crossingB = 0;
        // Forked style: the count and the record, so an edit that forks a
        // different style than the last one reports again.
        size_t styleForked = 0;
        uint32_t forkedStyle = 0;
        // Hidden influence: how many invisible operands moved something visible,
        // folded with their ids so a different set is a new event. The ids
        // themselves ride the report for click-to-select and are snapshotted here
        // only to decide whether to emit.
        size_t hiddenCount = 0;
        uint64_t hiddenKey = 0;
        // A refused driving imposition with the reference measurement on offer.
        // The downgrade is set only by the real impose and set-value paths — the
        // const preview query never touches it — so a transition to present is an
        // edit that refused, never a hover. Keyed with the conflicting set so a
        // refusal against different relations reports again.
        bool downgraded = false;
        int downgradeKind = 0;
        size_t downgradeAssignment = 0;
        uint64_t conflictKey = 0;

        // Keyed on the serial: a deletion op with the same counts as the last one
        // still differs here, so the second is reported rather than swallowed.
        bool deletionEquals(const ReportSnapshot &o) const {
            return deletionSerial == o.deletionSerial;
        }
        bool structureEquals(const ReportSnapshot &o) const {
            return structureSerial == o.structureSerial;
        }
        bool crossingEquals(const ReportSnapshot &o) const {
            return crossingA == o.crossingA && crossingB == o.crossingB;
        }
        bool styleEquals(const ReportSnapshot &o) const {
            return styleForked == o.styleForked && forkedStyle == o.forkedStyle;
        }
        bool hiddenEquals(const ReportSnapshot &o) const {
            return hiddenCount == o.hiddenCount && hiddenKey == o.hiddenKey;
        }
        bool downgradeEquals(const ReportSnapshot &o) const {
            return downgraded == o.downgraded && downgradeKind == o.downgradeKind &&
                   downgradeAssignment == o.downgradeAssignment && conflictKey == o.conflictKey;
        }
        bool operator==(const ReportSnapshot &o) const {
            return deletionEquals(o) && structureEquals(o) && crossingEquals(o) &&
                   styleEquals(o) && hiddenEquals(o) && downgradeEquals(o);
        }
    };
    ReportSnapshot lastReportSnapshot_;
    // The report fields as they stand now. Kept apart so the non-capturing paths
    // (a session swap, a silent playback step) can resync the baseline without
    // emitting, which is what stops leftover playback state from being reported
    // as the first interactive edit's doing.
    ReportSnapshot snapshotReports() const;

    friend class WorkspaceManager;
};

}  // namespace paroculus
