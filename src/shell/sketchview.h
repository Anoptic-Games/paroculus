// The single Qt-aware seam.
//
// Everything it consumes — interact, render — is toolkit-agnostic. Its whole
// job is translation: QEvents become abstract PointerEvents in both spaces, and
// the renderer paints into a buffer it owns. Nothing here decides anything about
// interaction; the session does, and a script can drive the session identically.
//
// View state — pan and zoom — is owned here, per the seam layout: core owns the
// transform *type*, render owns how a framing and a pan and zoom compose into
// one, and the shell owns what they currently are.
#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "core/undo.h"
#include "interact/script.h"
#include "core/composition.h"
#include "interact/session.h"
#include "render/view.h"

class SketchView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString selectionText READ selectionText NOTIFY changed)
    // Under-constraint is the normal state, so this is displayed calmly rather
    // than as a progress bar or a warning: a free degree of freedom is a thing
    // the user can still push by hand.
    Q_PROPERTY(int dof READ dof NOTIFY changed)
    Q_PROPERTY(QString solveStatus READ solveStatus NOTIFY changed)
    // The transient strip near the work: what the current selection admits,
    // ranked. A projection of the registry and nothing else — the shell
    // decides how an entry looks and never whether it applies.
    Q_PROPERTY(QVariantList strip READ strip NOTIFY changed)
    // The layers, back to front, with what the surface needs to toggle them.
    // The implicit base layer is not in here: it has no record to change.
    Q_PROPERTY(QVariantList layers READ layers NOTIFY changed)
    // Two diagnostics, both faces of no-silent-changes. Something invisible
    // moved something visible; something the user deleted left a region unable
    // to enclose what it still says it encloses.
    Q_PROPERTY(int hiddenInfluences READ hiddenInfluences NOTIFY changed)
    Q_PROPERTY(int brokenRegions READ brokenRegions NOTIFY changed)

public:
    explicit SketchView(QQuickItem *parent = nullptr);
    ~SketchView() override;

    void paint(QPainter *painter) override;

    QString status() const;
    QString selectionText() const;
    int dof() const;
    QString solveStatus() const;
    QVariantList strip() const;
    QVariantList layers() const;
    int hiddenInfluences() const;
    int brokenRegions() const;

    // The whole catalogue, filtered. Inapplicable entries come back marked
    // rather than missing, because a command that vanishes is a command the
    // user cannot learn.
    Q_INVOKABLE QVariantList palette(const QString &query) const;

    // Runs a registered action by name. The one entrance every surface uses:
    // a surface that called the session directly could offer what the model
    // would refuse, and the bug would look like a model bug rather than a UI
    // one.
    Q_INVOKABLE bool run(const QString &name, const QVariantMap &arguments = {});

    // What imposing the named relation would do, for hover preview. Leaves the
    // document byte-identical, so a hover storm costs nothing but time.
    Q_INVOKABLE QString previewOf(const QString &name, int assignment) const;

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void resetView();

signals:
    void changed();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    // Catches the item gaining a window and being dragged to a screen of a
    // different density, both of which change the resolution to rasterise at.
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    // Rebuilds the viewport from the current pan/zoom and item size, and hands
    // it to the session. Called whenever either changes — and only then: it
    // reads no document state, so editing the sketch can never move the view.
    void syncViewport();
    // Keeps the backing texture at the panel's true resolution.
    void syncTextureSize();
    double devicePixelRatio() const;

    // Adopts a recorded session and starts stepping through it. Playback is
    // faithful: the script's own viewport steps apply, so what is shown is the
    // session as recorded rather than as this window would have framed it.
    void playScript(paroculus::GestureScript script);
    void stepScript();
    paroculus::PointerEvent translate(const QPointF &position, Qt::MouseButtons buttons,
                                      Qt::KeyboardModifiers modifiers,
                                      paroculus::PointerAction action, int clicks) const;
    paroculus::PointerEvent translate(const QPointF &position, Qt::MouseButtons buttons,
                                      Qt::KeyboardModifiers modifiers,
                                      paroculus::PointerAction action) const;

    paroculus::Document document_;
    paroculus::UndoJournal journal_;
    std::unique_ptr<paroculus::Session> session_;

    // What the view currently is. Kept here rather than in interact because
    // that is a shell concern; how the parts compose into a transform is
    // render's, beside the fitting it composes over.
    paroculus::ViewState view_;
    // A middle-button drag in flight. It never reaches the session: a pan is a
    // change of view, not an edit, and what the session sees of it is the
    // viewport it produces.
    bool panning_ = false;
    Eigen::Vector2d panFrom_ = Eigen::Vector2d::Zero();

    // Script playback state. While a script is running the view is a spectator:
    // it must not re-frame the viewport, because the script carries the one it
    // was recorded under and screen coordinates mean nothing without it.
    paroculus::GestureScript script_;
    size_t scriptStep_ = 0;
    bool playing_ = false;
    QTimer scriptTimer_;

    // Recording, when --record asked for it. The starting document is kept
    // alongside, because a recording without the state it started from replays
    // into a different sketch.
    std::unique_ptr<paroculus::ScriptRecorder> recorder_;
    paroculus::Document recordedFrom_;
    QString recordPath_;

    QImage surface_;  // retained so Skia is not handed a fresh buffer each frame
};
