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
    // The session was replaced under a stable workspace pointer (a load into a
    // reused tab), so the canvas must re-sync the viewport for the new session
    // rather than assume a pointer change already did. Distinct from changed(),
    // which only repaints — a repaint through an unset viewport draws the
    // document at raw coordinates.
    void viewReset();
    // A no-silent-changes event was just logged, for the frame to flash as a
    // toast. The reports model is the memory; this is the notice.
    void reportPosted(const QString &text);

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

    Document document_;
    UndoJournal journal_;
    std::unique_ptr<Session> session_;
    ViewState view_;

    QString filePath_;
    // The revision the document was last saved at. dirty() is revision != this.
    // Zero for a fresh empty document, which reads clean until the first edit.
    uint64_t savedRevision_ = 0;
    // The ordinal for an untitled document's tab label, assigned by the manager.
    int untitledOrdinal_ = 0;

    // The previewed pose held between a hover and the paint. Transient view
    // state: the session does not know it exists and the document is untouched.
    std::vector<SeedSpan> ghostPose_;

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
        size_t deletedEntities = 0, deletedRelations = 0, degraded = 0;
        int transformError = 0, compoundError = 0;
        size_t retargeted = 0, rescaled = 0, straddling = 0, copied = 0;
        size_t droppedRelations = 0, droppedRegions = 0, droppedTags = 0;
        uint64_t crossingA = 0, crossingB = 0;

        bool deletionEquals(const ReportSnapshot &o) const {
            return deletedEntities == o.deletedEntities &&
                   deletedRelations == o.deletedRelations && degraded == o.degraded;
        }
        bool structureEquals(const ReportSnapshot &o) const {
            return transformError == o.transformError && compoundError == o.compoundError &&
                   retargeted == o.retargeted && rescaled == o.rescaled &&
                   straddling == o.straddling && copied == o.copied &&
                   droppedRelations == o.droppedRelations && droppedRegions == o.droppedRegions &&
                   droppedTags == o.droppedTags;
        }
        bool crossingEquals(const ReportSnapshot &o) const {
            return crossingA == o.crossingA && crossingB == o.crossingB;
        }
        bool operator==(const ReportSnapshot &o) const {
            return deletionEquals(o) && structureEquals(o) && crossingEquals(o);
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
