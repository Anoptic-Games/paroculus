#include "shell/sketchview.h"

#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QWheelEvent>

#include <cmath>

#include "interact/registry.h"
#include "interact/surface.h"
#include "render/view.h"
#include "shell/workspace.h"

using paroculus::Button;
using paroculus::Key;
using paroculus::Modifier;
using paroculus::PointerAction;
using paroculus::PointerEvent;
using paroculus::Workspace;

SketchView::SketchView(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);
    // No ItemAcceptsInputMethod: the numeric strip is keystroke-driven and there
    // is no inputMethodEvent handler, so accepting input-method events would only
    // invite an IME to swallow digits and tool letters as preedit.
    setFocus(true);
}

SketchView::~SketchView() = default;

void SketchView::setWorkspace(Workspace *workspace) {
    if(workspace == workspace_) return;
    // The previous workspace's signals stop mattering the moment it is unbound.
    QObject::disconnect(changedConnection_);
    QObject::disconnect(viewResetConnection_);
    QObject::disconnect(highlightConnection_);
    workspace_ = workspace;
    if(workspace_ != nullptr) {
        // Any mutation from any surface — a QML action, the async pump, a script
        // step — reaches the canvas through the one coarse changed() signal.
        changedConnection_ =
            QObject::connect(workspace_, &Workspace::changed, this, [this]() { update(); });
        // A hovered-relation highlight repaints the canvas and nothing else, so it
        // rides its own signal rather than changed(), which would rebuild the
        // relations list under the cursor.
        highlightConnection_ = QObject::connect(workspace_, &Workspace::highlightChanged, this,
                                                [this]() { update(); });
        // A session replaced under the same workspace pointer (a load into a
        // reused tab) needs the viewport re-synced, which changed() does not do.
        viewResetConnection_ = QObject::connect(workspace_, &Workspace::viewReset, this, [this]() {
            syncViewport();
            update();
        });
        // The new tab carries its own view framing, so re-sync the viewport for
        // it against the current item size before the first paint.
        syncViewport();
    }
    update();
    emit workspaceChanged();
}

double SketchView::devicePixelRatio() const {
    return window() != nullptr ? window()->effectiveDevicePixelRatio() : 1.0;
}

void SketchView::syncTextureSize() { setTextureSize(deviceSize()); }

QSize SketchView::deviceSize() const {
    const double dpr = devicePixelRatio();
    return QSize(qMax(1, qRound(width() * dpr)), qMax(1, qRound(height() * dpr)));
}

void SketchView::syncViewport() {
    if(workspace_ == nullptr) return;
    // A running script owns the viewport. Re-fitting here would replace the
    // transform its screen coordinates were recorded against.
    if(workspace_->playing()) return;

    const int w = qMax(1, qRound(width()));
    const int h = qMax(1, qRound(height()));

    // The item has no size during construction, so the first framings are
    // provisional and only a real one latches.
    workspace_->view().frameOnce(workspace_->session().pose(), w, h,
                                 width() > 0.0 && height() > 0.0);

    paroculus::Viewport viewport;
    viewport.view = workspace_->view().transform(w, h);
    viewport.width = w;
    viewport.height = h;
    workspace_->session().setViewport(viewport);
}

PointerEvent SketchView::translate(const QPointF &position, Qt::MouseButtons buttons,
                                   Qt::KeyboardModifiers modifiers, PointerAction action) const {
    return translate(position, buttons, modifiers, action, 1);
}

PointerEvent SketchView::translate(const QPointF &position, Qt::MouseButtons buttons,
                                   Qt::KeyboardModifiers modifiers, PointerAction action,
                                   int clicks) const {
    // The view in force now is the only thing this reads from the workspace; the
    // rest is a pure remap, split into shelltest::translatePointer so a test can
    // reach it without a live session.
    return shelltest::translatePointer(position, buttons, modifiers,
                                       workspace_->session().viewport().view, action, clicks);
}

void SketchView::mousePressEvent(QMouseEvent *event) {
    forceActiveFocus();
    if(workspace_ == nullptr) return;
    // The middle button pans and is not forwarded: one gesture cannot be both a
    // view change and whatever the session made of a press it has no use for.
    if(event->button() == Qt::MiddleButton) {
        panning_ = true;
        panFrom_ = Eigen::Vector2d(event->position().x(), event->position().y());
        return;
    }
    // Inspect mode is navigation-only: the pan above still works, but a pointer
    // edit is inert — not queued, not reinterpreted.
    if(workspace_->inspectMode()) return;
    workspace_->session().handle(translate(event->position(), event->button(), event->modifiers(),
                                           PointerAction::Press));
    workspace_->notifyChanged();
}

// Qt has already applied the platform's double-click interval and slop, which is
// the whole reason the count is decided out here: the interact layer must not
// grow a clock.
void SketchView::mouseDoubleClickEvent(QMouseEvent *event) {
    forceActiveFocus();
    if(workspace_ == nullptr) return;
    if(workspace_->inspectMode()) return;
    workspace_->session().handle(translate(event->position(), event->button(), event->modifiers(),
                                           PointerAction::Press, 2));
    workspace_->notifyChanged();
}

void SketchView::mouseMoveEvent(QMouseEvent *event) {
    if(workspace_ == nullptr) return;
    if(panning_) {
        // Pixel for pixel: a pan is a hand on the paper, so the document point
        // under the cursor is the one that stays under it.
        const Eigen::Vector2d at(event->position().x(), event->position().y());
        workspace_->view().pan += at - panFrom_;
        panFrom_ = at;
        syncViewport();
        workspace_->notifyChanged();
        return;
    }
    if(workspace_->inspectMode()) return;
    workspace_->session().handle(translate(event->position(), event->buttons(), event->modifiers(),
                                           PointerAction::Move));
    workspace_->notifyChanged();
}

void SketchView::mouseReleaseEvent(QMouseEvent *event) {
    if(workspace_ == nullptr) return;
    // The middle release ends the pan and is consumed. Every other button is an
    // edit and always reaches the session, panning or not.
    if(event->button() == Qt::MiddleButton) {
        panning_ = false;
        return;
    }
    if(workspace_->inspectMode()) return;
    workspace_->session().handle(translate(event->position(), event->button(), event->modifiers(),
                                           PointerAction::Release));
    workspace_->notifyChanged();
}

void SketchView::hoverMoveEvent(QHoverEvent *event) {
    if(workspace_ == nullptr) return;
    if(workspace_->inspectMode()) return;
    workspace_->session().handle(
        translate(event->position(), Qt::NoButton, event->modifiers(), PointerAction::Move));
    workspace_->notifyChanged();
}

void SketchView::zoomBy(double factor) {
    if(workspace_ == nullptr) return;
    const int w = qMax(1, qRound(width()));
    const int h = qMax(1, qRound(height()));
    paroculus::ViewState &view = workspace_->view();
    const double previous = view.zoom;
    view.zoomAt(Eigen::Vector2d(w / 2.0, h / 2.0), qBound(0.05, view.zoom * factor, 40.0), w, h);
    if(view.zoom == previous) return;
    syncViewport();
    workspace_->notifyChanged();
}

void SketchView::zoomIn() { zoomBy(1.2); }
void SketchView::zoomOut() { zoomBy(1.0 / 1.2); }

// Anchored on the cursor: the document point under it stays under it. Pan is the
// composition's outermost term, so holding a point fixed is the difference
// between where it lands before and after the zoom — a subtraction.
void SketchView::wheelEvent(QWheelEvent *event) {
    if(workspace_ == nullptr) return;
    const double steps = event->angleDelta().y() / 120.0;
    paroculus::ViewState &view = workspace_->view();
    const double previous = view.zoom;
    view.zoomAt(Eigen::Vector2d(event->position().x(), event->position().y()),
                qBound(0.05, view.zoom * std::pow(1.15, steps), 40.0), qMax(1, qRound(width())),
                qMax(1, qRound(height())));
    if(view.zoom == previous) return;
    syncViewport();
    workspace_->notifyChanged();
}

// The keyboard is a projection of the action registry. Keys that are not actions
// stay here: Esc, Tab and Enter drive the interaction state machine rather than
// the catalogue. Everything else is resolved by registry.h.
namespace shelltest {

int engravedDigit(const QKeyEvent *event) {
    const quint32 scan = event->nativeScanCode();
    if(scan >= 10 && scan <= 18) return static_cast<int>(scan) - 10 + 1;
    if(event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9) {
        return event->key() - Qt::Key_1 + 1;
    }
    return 0;
}

paroculus::KeyStroke strokeOf(const QKeyEvent *event) {
    paroculus::KeyStroke stroke;
    const QString text = event->text();
    if(!text.isEmpty()) stroke.character = text.at(0).toLatin1();
    // Under Control, some platforms deliver a C0 control character in text()
    // (Ctrl+Z as 0x1a) rather than the letter. Reconstruct the engraved letter
    // from the key symbol so the registry sees the same 'z' a headless caller
    // spells — the same reconstruction the engraved digit does for the shifted
    // number row, and the reason resolveKey can carry ctrl chords at all.
    if((event->modifiers() & Qt::ControlModifier) && stroke.character > 0 &&
       stroke.character < 0x20 && event->key() >= Qt::Key_A && event->key() <= Qt::Key_Z) {
        stroke.character = static_cast<char>('a' + (event->key() - Qt::Key_A));
    }
    stroke.digit = engravedDigit(event);
    if(event->modifiers() & Qt::ShiftModifier) stroke.modifiers |= Modifier::Shift;
    if(event->modifiers() & Qt::ControlModifier) stroke.modifiers |= Modifier::Control;
    if(event->modifiers() & Qt::AltModifier) stroke.modifiers |= Modifier::Alt;
    return stroke;
}

paroculus::PointerEvent translatePointer(const QPointF &position, Qt::MouseButtons buttons,
                                         Qt::KeyboardModifiers modifiers,
                                         const paroculus::ViewTransform &view,
                                         PointerAction action, int clicks) {
    Button button = Button::None;
    if(buttons & Qt::LeftButton) button = Button::Left;
    else if(buttons & Qt::MiddleButton) button = Button::Middle;
    else if(buttons & Qt::RightButton) button = Button::Right;

    Modifier mods = Modifier::None;
    if(modifiers & Qt::ShiftModifier) mods |= Modifier::Shift;
    if(modifiers & Qt::ControlModifier) mods |= Modifier::Control;
    if(modifiers & Qt::AltModifier) mods |= Modifier::Alt;

    // Both spaces filled from one conversion, so they can never disagree.
    return PointerEvent::at(action, Eigen::Vector2d(position.x(), position.y()), view, button,
                            mods, clicks);
}

}  // namespace shelltest

void SketchView::keyPressEvent(QKeyEvent *event) {
    if(workspace_ == nullptr) {
        QQuickPaintedItem::keyPressEvent(event);
        return;
    }
    // Inspect mode swallows edit keystrokes — inert, not queued — and exits on
    // Esc, the same key the toggle offers. Navigation (wheel, middle-pan) is
    // unaffected because it never reaches here.
    if(workspace_->inspectMode()) {
        if(event->key() == Qt::Key_Escape) workspace_->setInspectMode(false);
        return;
    }
    paroculus::Session &session = workspace_->session();
    const bool typing = session.presentation().numericActive;

    switch(event->key()) {
        case Qt::Key_Escape: session.handle(Key::Escape); break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            session.handle(Key::Enter, (event->modifiers() & Qt::ShiftModifier) ? Modifier::Shift
                                                                                : Modifier::None);
            break;
        case Qt::Key_Tab: session.handle(Key::Tab); break;
        case Qt::Key_Backspace:
            // Backspace edits the field while one is open, and deletes only when
            // there is nothing being typed.
            if(typing) session.numericBackspace();
            else paroculus::invokeAction(session, "edit.delete");
            break;
        case Qt::Key_Delete: paroculus::invokeAction(session, "edit.delete"); break;

        default: {
            const paroculus::KeyBinding binding =
                paroculus::resolveKey(paroculus::contextOf(session), shelltest::strokeOf(event));
            switch(binding.kind) {
                case paroculus::KeyBinding::Kind::Text:
                    session.type(binding.character);
                    break;
                case paroculus::KeyBinding::Kind::Action:
                    paroculus::invokeAction(session, binding.action->name, binding.arguments);
                    break;
                case paroculus::KeyBinding::Kind::None:
                    QQuickPaintedItem::keyPressEvent(event);
                    return;
            }
            break;
        }
    }
    // No syncViewport: a keystroke edits the sketch, and the view is not a
    // function of the sketch.
    workspace_->notifyChanged();
}

void SketchView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    // A resize holds the document point at the viewport centre at the centre, so
    // the thing being examined stays put rather than sliding as the pixels
    // around it change. Beside syncViewport, which then rebuilds the transform at
    // the new size; a script owns its own viewport, so leave it be while playing.
    if(workspace_ != nullptr && !workspace_->playing()) {
        workspace_->view().resize(oldGeometry.width(), oldGeometry.height(), newGeometry.width(),
                                  newGeometry.height());
    }
    syncTextureSize();
    syncViewport();
    // The viewport-dependent readouts — the glyph N-of-M the overlay budget
    // recomputes against the new size — refresh only on changed(), which a resize
    // does not otherwise emit. The paint below is fresh regardless; this keeps the
    // HUD from lagging it.
    if(workspace_ != nullptr) workspace_->notifyChanged();
}

void SketchView::itemChange(ItemChange change, const ItemChangeData &value) {
    QQuickPaintedItem::itemChange(change, value);
    if(change == ItemSceneChange || change == ItemDevicePixelRatioHasChanged) {
        syncTextureSize();
        update();
    }
}

void SketchView::resetView() {
    if(workspace_ == nullptr) return;
    // The one way back to a fitted framing, and the only thing that re-derives
    // the view from the document. Nothing else may.
    workspace_->view() = paroculus::ViewState{};
    syncViewport();
    workspace_->notifyChanged();
}

// Skia renders into the QImage's own pixels, so the only copy is Qt's final blit.
// Format_ARGB32_Premultiplied is byte-for-byte kBGRA_8888/kPremul on little-
// endian, which is what renderDocument writes.
void SketchView::paint(QPainter *painter) {
    if(workspace_ == nullptr || width() <= 0.0 || height() <= 0.0) return;

    const double dpr = devicePixelRatio();
    const QSize device = deviceSize();

    if(surface_.size() != device) {
        surface_ = QImage(device, QImage::Format_ARGB32_Premultiplied);
        surface_.setDevicePixelRatio(dpr);
    }

    paroculus::Session &session = workspace_->session();
    const paroculus::Presentation &p = session.presentation();
    paroculus::Adornment adornment;
    // The background applies in every mode, inspect included — it is the one
    // presentation preference that survives the WYSIWYG toggle.
    adornment.background = workspace_->backgroundColor();

    if(workspace_->inspectMode()) {
        // Document as output: no adorners, no construction, no grid, no tint. The
        // adorner lists stay empty and documentOnly governs the vertices and the
        // construction geometry drawn from the document regardless of them.
        adornment.documentOnly = true;
    } else {
        adornment.selected = session.selection().items();
        adornment.hovered = p.hovered;
        adornment.resisting = p.resisting;
        adornment.marqueeActive = p.marqueeActive;
        adornment.marqueeFrom = p.marqueeFrom;
        adornment.marqueeTo = p.marqueeTo;
        adornment.glyphs = session.glyphs();
        // The inspector's hovered relation lights up the geometry it names: its
        // operands tint (render walks adornment.highlighted) and its own glyph
        // emphasises, so a row in the list points at the drawing both ways.
        if(workspace_->hoveredRelation().valid()) {
            adornment.highlighted.push_back(workspace_->hoveredRelation());
            for(paroculus::GlyphMark &m : adornment.glyphs) {
                if(m.constraint == workspace_->hoveredRelation()) m.hovered = true;
            }
        }
        adornment.handledTags = session.selectedTags();
        adornment.ghostPose = workspace_->ghostPose();
        const paroculus::SnapPolicy &snap = session.snapPolicy();
        // Grid display follows the sidecar preference, not grid snapping: the two
        // share the step so the drawn grid can never lie about where a click
        // lands, but showing it and snapping to it are separate questions.
        adornment.gridStep = workspace_->gridVisible() ? snap.gridStep : 0.0;
        adornment.extensions = workspace_->extensions();
        const paroculus::Session::AxisFrames frames =
            session.axisFrames(workspace_->showAllFrames());
        adornment.documentFrame = frames.documentFrame;
        adornment.axisFrames = frames.clusterFrames;
        adornment.ghostActive = p.toolPreview.active;
        adornment.ghostFrom = p.toolPreview.from;
        adornment.ghostTo = p.toolPreview.to;
        switch(p.tool) {
            case paroculus::ToolKind::Circle:
                adornment.ghostShape = paroculus::Adornment::GhostShape::Circle;
                break;
            case paroculus::ToolKind::Rectangle:
                adornment.ghostShape = paroculus::Adornment::GhostShape::Rectangle;
                break;
            case paroculus::ToolKind::Arc:
                adornment.ghostShape = p.toolPreview.arcActive
                                           ? paroculus::Adornment::GhostShape::Arc
                                           : paroculus::Adornment::GhostShape::Line;
                adornment.ghostCentre = p.toolPreview.arcCentre;
                adornment.ghostRadius = p.toolPreview.arcRadius;
                adornment.ghostStart = p.toolPreview.arcStart;
                adornment.ghostSweep = p.toolPreview.arcSweep;
                break;
            default:
                adornment.ghostShape = paroculus::Adornment::GhostShape::Line;
                break;
        }
    }

    paroculus::renderDocument(session.pose(), session.viewport().view, adornment, surface_.bits(),
                              device.width(), device.height(),
                              static_cast<size_t>(surface_.bytesPerLine()), dpr);
    painter->drawImage(0, 0, surface_);
}
