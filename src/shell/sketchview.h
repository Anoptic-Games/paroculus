// The single Qt-aware canvas seam.
//
// Everything it consumes — interact, render — is toolkit-agnostic. Its whole job
// is translation: QEvents become abstract PointerEvents in both spaces, and the
// renderer paints into a buffer it owns. Nothing here decides anything about
// interaction; the session does, and a script can drive the session identically.
//
// It no longer owns the document, session or view: those moved to Workspace, one
// per open document, and the item binds whichever the active tab names through
// the `workspace` property. What stays here is what only the canvas item can do —
// translate Qt input, paint one surface at the panel's true resolution, and drive
// the pan and zoom, which need the item size a workspace does not have.
#pragma once

#include <QImage>
#include <QMetaObject>
#include <QQuickPaintedItem>
#include <QSize>
#include <QtQml/qqmlregistration.h>

#include "core/geom.h"
#include "interact/events.h"
#include "interact/registry.h"  // KeyStroke, for the exposed strokeOf
#include "render/view.h"
#include "shell/workspace.h"  // complete Workspace, for the metatype of the property

// Only reference/pointer parameters of the exposed translation helpers name
// these, so forward declarations keep the surface minimal.
class QKeyEvent;
class QPointF;

class SketchView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    // The document this canvas shows. Set by the frame to the active tab's
    // workspace and re-set when the tab changes; the canvas reads the workspace's
    // session for paint and hit, forwards events to it, and drives its view.
    Q_PROPERTY(paroculus::Workspace *workspace READ workspace WRITE setWorkspace NOTIFY
                   workspaceChanged)

public:
    explicit SketchView(QQuickItem *parent = nullptr);
    ~SketchView() override;

    void paint(QPainter *painter) override;

    paroculus::Workspace *workspace() const { return workspace_; }
    void setWorkspace(paroculus::Workspace *workspace);

    // View framing, which needs the item size and so lives here rather than on
    // the workspace. resetView is the one thing that re-derives the view from the
    // document — a view that re-frames itself is a view the user cannot keep — and
    // clearing the latch is the only way back to a fit.
    Q_INVOKABLE void resetView();
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();

signals:
    void workspaceChanged();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    // Rebuilds the viewport from the bound workspace's view and the item size, and
    // hands it to that workspace's session. Called whenever either changes — and
    // only then: it reads no document state, so editing the sketch can never move
    // the view. A no-op with no workspace, or while one is replaying a script,
    // which owns its own viewport.
    void syncViewport();
    void syncTextureSize();
    QSize deviceSize() const;
    double devicePixelRatio() const;
    // Zooms toward the viewport centre by `factor`, the shared body of the zoom
    // menu items.
    void zoomBy(double factor);

    paroculus::PointerEvent translate(const QPointF &position, Qt::MouseButtons buttons,
                                      Qt::KeyboardModifiers modifiers,
                                      paroculus::PointerAction action, int clicks) const;
    paroculus::PointerEvent translate(const QPointF &position, Qt::MouseButtons buttons,
                                      Qt::KeyboardModifiers modifiers,
                                      paroculus::PointerAction action) const;

    paroculus::Workspace *workspace_ = nullptr;
    // Repaints on the bound workspace's changed(), so a mutation from any surface
    // — a QML action, the async pump, a script step — reaches the canvas.
    QMetaObject::Connection changedConnection_;
    // Re-syncs the viewport when the workspace replaces its session under a stable
    // pointer (a load into a reused tab), which changed() alone would not — the
    // repaint would then draw through the fresh session's identity viewport.
    QMetaObject::Connection viewResetConnection_;
    // Repaints on the workspace's highlightChanged() — the inspector's hovered
    // relation, a repaint that must not rebuild any model. Kept separate from
    // changedConnection_ so it is unbound with the workspace like the others.
    QMetaObject::Connection highlightConnection_;

    // A middle-button drag in flight. It never reaches the session: a pan is a
    // change of view, not an edit, and what the session sees of it is the
    // viewport it produces.
    bool panning_ = false;
    Eigen::Vector2d panFrom_ = Eigen::Vector2d::Zero();

    QImage surface_;  // retained so Skia is not handed a fresh buffer each frame
};

// The shell's QEvent-to-interact translation, exposed for tests/shell/. Pure —
// they read no SketchView state — and pinned directly by the tests, which link no
// live session. Production keyPressEvent and the pointer handlers call them.
namespace shelltest {

// The digit engraved on a key's face, 1..9, or 0. Read from the physical scan
// code, not text() or key(), so shift and layout cannot disturb it.
int engravedDigit(const QKeyEvent *event);

// One abstract keystroke from a Qt key event: its character, engraved digit, and
// modifier set.
paroculus::KeyStroke strokeOf(const QKeyEvent *event);

// Qt pointer state to an abstract PointerEvent. Buttons and modifiers are a pure
// remap; the document position is filled through the view handed in.
paroculus::PointerEvent translatePointer(const QPointF &position, Qt::MouseButtons buttons,
                                         Qt::KeyboardModifiers modifiers,
                                         const paroculus::ViewTransform &view,
                                         paroculus::PointerAction action, int clicks);

}  // namespace shelltest
