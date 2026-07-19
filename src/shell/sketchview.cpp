#include "shell/sketchview.h"

#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include "render/view.h"
#include "solve/demosketch.h"

using paroculus::Button;
using paroculus::Key;
using paroculus::Modifier;
using paroculus::PointerAction;
using paroculus::PointerEvent;

SketchView::SketchView(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);
    setFlag(ItemAcceptsInputMethod, true);
    setFocus(true);

    // The demo, as a document. Stage 4's tools replace this with geometry the
    // user draws; until then it is what there is to drag.
    document_ = paroculus::demoDocument(1.618);
    session_ = std::make_unique<paroculus::Session>(document_, journal_);
    syncViewport();
}

SketchView::~SketchView() = default;

void SketchView::syncViewport() {
    const int w = qMax(1, qRound(width()));
    const int h = qMax(1, qRound(height()));

    const paroculus::Pose pose = session_->pose();
    base_ = paroculus::fitView(pose, w, h);

    // Pan and zoom compose on top of the fitted framing, in screen space, so
    // zooming keeps the viewport centre put rather than drifting toward the
    // document origin.
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(w * 0.5, h * 0.5) + pan_);
    m.scale(Eigen::Vector2d(zoom_, zoom_));
    m.translate(-Eigen::Vector2d(w * 0.5, h * 0.5));

    paroculus::Viewport viewport;
    viewport.view = paroculus::ViewTransform(m * base_.matrix());
    viewport.width = w;
    viewport.height = h;
    session_->setViewport(viewport);
}

PointerEvent SketchView::translate(const QPointF &position, Qt::MouseButtons buttons,
                                   Qt::KeyboardModifiers modifiers,
                                   PointerAction action) const {
    Button button = Button::None;
    if(buttons & Qt::LeftButton) button = Button::Left;
    else if(buttons & Qt::MiddleButton) button = Button::Middle;
    else if(buttons & Qt::RightButton) button = Button::Right;

    Modifier mods = Modifier::None;
    if(modifiers & Qt::ShiftModifier) mods |= Modifier::Shift;
    if(modifiers & Qt::ControlModifier) mods |= Modifier::Control;
    if(modifiers & Qt::AltModifier) mods |= Modifier::Alt;

    // Both spaces filled from one conversion, so they can never disagree.
    return PointerEvent::at(action, Eigen::Vector2d(position.x(), position.y()),
                            session_->viewport().view, button, mods);
}

void SketchView::mousePressEvent(QMouseEvent *event) {
    forceActiveFocus();
    session_->handle(translate(event->position(), event->button(), event->modifiers(),
                               PointerAction::Press));
    update();
    emit changed();
}

void SketchView::mouseMoveEvent(QMouseEvent *event) {
    session_->handle(translate(event->position(), event->buttons(), event->modifiers(),
                               PointerAction::Move));
    update();
    emit changed();
}

void SketchView::mouseReleaseEvent(QMouseEvent *event) {
    session_->handle(translate(event->position(), event->button(), event->modifiers(),
                               PointerAction::Release));
    update();
    emit changed();
}

void SketchView::hoverMoveEvent(QHoverEvent *event) {
    session_->handle(translate(event->position(), Qt::NoButton, event->modifiers(),
                               PointerAction::Move));
    update();
    emit changed();
}

void SketchView::wheelEvent(QWheelEvent *event) {
    const double steps = event->angleDelta().y() / 120.0;
    zoom_ = qBound(0.05, zoom_ * std::pow(1.15, steps), 40.0);
    syncViewport();
    update();
    emit changed();
}

void SketchView::keyPressEvent(QKeyEvent *event) {
    switch(event->key()) {
        case Qt::Key_Escape: session_->handle(Key::Escape); break;
        case Qt::Key_Delete:
        case Qt::Key_Backspace: session_->handle(Key::Delete); break;
        case Qt::Key_Z:
            // Undo and redo through the session, so the journal and the derived
            // indexes stay in step.
            session_->handle((event->modifiers() & Qt::ShiftModifier) ? Key::Redo : Key::Undo);
            break;
        default: QQuickPaintedItem::keyPressEvent(event); return;
    }
    syncViewport();
    update();
    emit changed();
}

void SketchView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    syncViewport();
}

void SketchView::undo() {
    session_->handle(Key::Undo);
    syncViewport();
    update();
    emit changed();
}

void SketchView::redo() {
    session_->handle(Key::Redo);
    syncViewport();
    update();
    emit changed();
}

void SketchView::deleteSelection() {
    session_->handle(Key::Delete);
    syncViewport();
    update();
    emit changed();
}

void SketchView::resetView() {
    pan_ = Eigen::Vector2d::Zero();
    zoom_ = 1.0;
    syncViewport();
    update();
    emit changed();
}

QString SketchView::status() const {
    const paroculus::Presentation &p = session_->presentation();
    QString text = QStringLiteral("%1  ·  dof: %2  ·  solve: %3 ms  ·  zoom: %4x")
                       .arg(QString::fromLatin1(paroculus::statusName(p.status)))
                       .arg(p.dof)
                       .arg(p.solveMicroseconds / 1000.0, 0, 'f', 2)
                       .arg(zoom_, 0, 'f', 2);
    if(p.saturated) {
        text += QStringLiteral("  ·  resisting: %1").arg(p.resisting.size());
    }
    if(p.rippledOffScreen) text += QStringLiteral("  ·  moved off screen");
    if(p.deletedEntities > 0 || p.deletedRelations > 0) {
        // Counts, not a confirmation dialog: the user is told what went.
        text += QStringLiteral("  ·  deleted %1 shapes, %2 relations")
                    .arg(p.deletedEntities)
                    .arg(p.deletedRelations);
    }
    return text;
}

QString SketchView::selectionText() const {
    const paroculus::Signature signature = session_->signature();
    if(signature.empty()) return QStringLiteral("nothing selected");
    return QString::fromStdString(signature.describe());
}

// Skia renders into the QImage's own pixels, so the only copy is Qt's final
// blit. Format_ARGB32_Premultiplied is byte-for-byte kBGRA_8888/kPremul on
// little-endian, which is what renderDocument writes.
void SketchView::paint(QPainter *painter) {
    const QSize size = QSize(qRound(width()), qRound(height()));
    if(size.isEmpty()) return;

    if(surface_.size() != size) {
        surface_ = QImage(size, QImage::Format_ARGB32_Premultiplied);
    }

    const paroculus::Presentation &p = session_->presentation();
    paroculus::Adornment adornment;
    adornment.selected = session_->selection().items();
    adornment.hovered = p.hovered;
    adornment.resisting = p.resisting;
    adornment.marqueeActive = p.marqueeActive;
    adornment.marqueeFrom = p.marqueeFrom;
    adornment.marqueeTo = p.marqueeTo;

    paroculus::renderDocument(session_->pose(), session_->viewport().view, adornment,
                              surface_.bits(), size.width(), size.height(),
                              static_cast<size_t>(surface_.bytesPerLine()));
    painter->drawImage(0, 0, surface_);
}
