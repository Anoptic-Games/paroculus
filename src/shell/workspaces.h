// The open set: every workspace, the active one, and the file lifecycle.
//
// One WorkspaceManager exists, created in main and handed to QML as a context
// property, so the tab bar and every panel project the active workspace's
// session and switching tabs swaps the projection source and the view. It is
// where the pendingScript handoff now lives — QML does not carry argv — and where
// the launch-time file argument opens.
//
// The manager also drives async solving's active-workspace-only posture: it
// enables the async path on whichever workspace is active and never on a hidden
// one, so a single scheduler exists at a time, which is what lets the U0 shell
// defer the cross-session gate audit the second scheduler would require.
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include "shell/workspace.h"  // complete Workspace, for the metatype of the active property

namespace paroculus {

class Settings;

class WorkspaceManager : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The WorkspaceManager is created in main and exposed as a context property")

    Q_PROPERTY(QList<QObject *> tabs READ tabs NOTIFY tabsChanged)
    Q_PROPERTY(int count READ count NOTIFY tabsChanged)
    Q_PROPERTY(int activeIndex READ activeIndex WRITE setActiveIndex NOTIFY activeChanged)
    Q_PROPERTY(paroculus::Workspace *active READ active NOTIFY activeChanged)

public:
    // settings may be null, in which case recents and the default directory are
    // not persisted — the offscreen test runner constructs a manager this way.
    explicit WorkspaceManager(Settings *settings = nullptr, QObject *parent = nullptr);
    ~WorkspaceManager() override;

    // Consumes the process-level handoffs (pending script, record path, launch
    // file argument) and stands the first workspace up. Called once by main after
    // construction, so argv is fully parsed before any workspace exists.
    void startup();

    QList<QObject *> tabs() const;
    int count() const;
    int activeIndex() const { return activeIndex_; }
    Workspace *active() const;

    void setActiveIndex(int index);

    // A new empty document in a new tab, activated. The demo document is gone
    // from here deliberately: a new tab is empty, and demoDocument survives only
    // for --selftest.
    Q_INVOKABLE void newWorkspace();

    // Cycles the active tab by delta, wrapping. Ctrl+Tab and Ctrl+Shift+Tab.
    Q_INVOKABLE void cycleActive(int delta);

    // Closes the tab at index unconditionally. The unsaved-changes prompt is the
    // QML frame's, gated on the workspace's dirty flag before it calls here — the
    // one legitimate modal beyond file choosing, kept out of this toolkit-free
    // decision. Always leaves at least one tab: closing the last opens a fresh
    // empty one, so the window is never a canvas with no document.
    Q_INVOKABLE void closeWorkspace(int index);

    // ---- File lifecycle ----
    // Opens a document from disk into a workspace, activated. Reuses a pristine
    // untitled tab when the active one is empty and clean, else opens a new tab —
    // the conventional "open replaces the scratch tab" behaviour. On a loader
    // refusal the document is never half-loaded: openFailed carries the loader's
    // line diagnostic for the frame to surface as a toast plus report entry, and
    // no tab changes. Returns whether it opened.
    Q_INVOKABLE bool openFile(const QString &path);

    // Whether the active workspace has no path yet, so Save must divert to
    // Save As. Read by the frame before it decides which dialog to open.
    Q_INVOKABLE bool activeNeedsPath() const;

    // Writes the active workspace to a path through persist, marks it saved,
    // writes its sidecar beside it, and records the path in recents. Returns
    // whether the write landed; a short write is a failed save, surfaced by the
    // frame. Save with an existing path passes that path; Save As passes the
    // chosen one.
    Q_INVOKABLE bool saveActiveTo(const QString &path);

    // The active workspace's path, for the frame to pass back into saveActiveTo
    // on a plain Save.
    Q_INVOKABLE QString activePath() const;

    // ---- Interchange and developer surface (U3) ----
    // Traces an SVG into a new workspace, activated: geometry arrives free and
    // unconstrained, and the traced and skipped counts land in that workspace's
    // reports panel. Reuses a pristine scratch tab exactly as Open does, so an
    // untouched empty tab is not left beside the import. On a read failure nothing
    // opens and openFailed carries the diagnostic. Returns whether it traced.
    Q_INVOKABLE bool importSvg(const QString &path);

    // Replays a recorded session into a fresh workspace, activated, with the
    // progress overlay the workspace projects. A new tab always — a replay is a
    // performance of another session, not an edit to the current document — so it
    // never disturbs the active one. On a parse failure nothing opens and
    // openFailed carries the diagnostic. Returns whether it started.
    Q_INVOKABLE bool replayScript(const QString &path);

    // The recent files, most recent first, and the default directory for the
    // open and save dialogs. Projections of settings; empty without one.
    Q_INVOKABLE QStringList recentFiles() const;
    Q_INVOKABLE QString defaultDirectory() const;

    // The glyph-density preference is application-wide, so setting it stores the
    // value and applies it to every open workspace — not only the active one — so
    // a change is honoured on every tab at once rather than tab by tab.
    Q_INVOKABLE void setGlyphDensity(double multiplier);

signals:
    void tabsChanged();
    void activeChanged();
    // A load or save refused, or a launch file could not be opened. The frame
    // surfaces it as a toast and a reports entry, never a modal — a diagnostic is
    // never modal. line is 1-based, 0 when not line-specific.
    void openFailed(const QString &path, const QString &message, int line);
    void saveFailed(const QString &path);

private:
    Workspace *addWorkspace();
    bool pristine(const Workspace *workspace) const;
    void activate(int index);

    QList<Workspace *> workspaces_;
    int activeIndex_ = -1;
    int nextUntitled_ = 1;
    Settings *settings_ = nullptr;
};

}  // namespace paroculus
