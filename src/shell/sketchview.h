// The single Qt-aware seam.
//
// Everything it consumes — interact, render — is toolkit-agnostic. Its whole
// job is translation: QEvents become abstract PointerEvents in both spaces, and
// the renderer paints into a buffer it owns. Nothing here decides anything about
// interaction; the session does, and a script can drive the session identically.
//
// View state — pan and zoom — is owned here, per the seam layout: core owns the
// transform *type*, the shell owns what it currently is.
#pragma once

#include <QImage>
#include <QQuickPaintedItem>
#include <QString>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <memory>

#include "core/undo.h"
#include "interact/script.h"
#include "interact/session.h"

class SketchView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString selectionText READ selectionText NOTIFY changed)

public:
    explicit SketchView(QQuickItem *parent = nullptr);
    ~SketchView() override;

    void paint(QPainter *painter) override;

    QString status() const;
    QString selectionText() const;

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void resetView();

signals:
    void changed();

protected:
    void mousePressEvent(QMouseEvent *event) override;
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
    // it to the session. Called whenever either changes.
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
                                      paroculus::PointerAction action) const;

    paroculus::Document document_;
    paroculus::UndoJournal journal_;
    std::unique_ptr<paroculus::Session> session_;

    // View state: a pan in pixels and a zoom factor over the fitted framing.
    // Kept here rather than in interact because what the view currently is, is
    // a shell concern; how to convert through it is core's.
    Eigen::Vector2d pan_ = Eigen::Vector2d::Zero();
    double zoom_ = 1.0;
    paroculus::ViewTransform base_;

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
