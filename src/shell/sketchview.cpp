#include "shell/sketchview.h"

#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QWheelEvent>

#include <cstdio>

#include "app/scriptplay.h"
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

    // Roughly a frame per step, so a recorded drag replays at about the speed
    // it was performed. Fast enough to read as a gesture, slow enough to watch.
    scriptTimer_.setInterval(16);
    connect(&scriptTimer_, &QTimer::timeout, this, &SketchView::stepScript);

    paroculus::GestureScript pending;
    if(paroculus::pendingScript::take(pending)) {
        playScript(std::move(pending));
        return;
    }

    const std::string record = paroculus::pendingScript::takeRecordPath();
    if(!record.empty()) {
        recordPath_ = QString::fromStdString(record);
        // Captured before anything is attached, and before the first viewport
        // is pushed, so the file starts from what the session actually started
        // from. Opening a document is not an edit, so this is that document.
        recordedFrom_ = document_;
        recorder_ = std::make_unique<paroculus::ScriptRecorder>();
        session_->setRecorder(recorder_.get());
        // The viewport is already set, so record it explicitly: a script whose
        // first pointer step precedes any viewport step has no transform to
        // read its screen coordinates through.
        syncViewport();
    }
}

void SketchView::playScript(paroculus::GestureScript script) {
    script_ = std::move(script);
    scriptStep_ = 0;
    playing_ = true;

    // The script's document replaces ours wholesale, and the journal starts
    // empty: a replay is the recorded session, not that session appended to
    // whatever this window was already showing.
    document_ = script_.document;
    journal_ = paroculus::UndoJournal();
    session_ = std::make_unique<paroculus::Session>(document_, journal_);

    scriptTimer_.start();
    update();
    emit changed();
}

void SketchView::stepScript() {
    if(scriptStep_ >= script_.steps.size()) {
        scriptTimer_.stop();
        playing_ = false;
        // The view stays where the script left it rather than snapping back to
        // a fitted framing: the last frame is the one worth looking at.
        update();
        emit changed();
        return;
    }
    paroculus::applyStep(*session_, script_.steps[scriptStep_++]);
    update();
    emit changed();
}

// Written at teardown rather than incrementally: a session is what happened
// between opening and closing, and a half-file from a crashed run would be a
// script that replays into a state nobody was ever in.
SketchView::~SketchView() {
    if(recorder_ == nullptr || recordPath_.isEmpty()) return;

    paroculus::GestureScript script;
    script.document = recordedFrom_;
    script.steps = recorder_->steps();

    std::string error;
    if(paroculus::saveScriptFile(recordPath_.toStdString(), script, error)) {
        std::fprintf(stderr, "recorded %zu steps to %s\n", script.steps.size(),
                     recordPath_.toUtf8().constData());
    } else {
        std::fprintf(stderr, "%s\n", error.c_str());
    }
}

// Device pixels per logical pixel for the screen this item is on. 1.0 until the
// item has a window, which is the case during construction.
double SketchView::devicePixelRatio() const {
    return window() != nullptr ? window()->effectiveDevicePixelRatio() : 1.0;
}

// QQuickPaintedItem's texture defaults to the item's logical size, so on a
// HiDPI display the sketch would rasterise at 1x and be scaled up. Sizing it
// explicitly is what buys the real resolution; paint() still works in logical
// coordinates, because Qt applies the compensating scale to the painter.
void SketchView::syncTextureSize() {
    const double dpr = devicePixelRatio();
    setTextureSize(QSize(qMax(1, qRound(width() * dpr)), qMax(1, qRound(height() * dpr))));
}

void SketchView::syncViewport() {
    // A running script owns the viewport. Re-fitting here would replace the
    // transform its screen coordinates were recorded against, and every event
    // after that would land somewhere else.
    if(playing_) return;

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
        // Tool activation is a keystroke until the registry projects it into a
        // surface, which is the next item in this stage.
        case Qt::Key_L: session_->setTool(paroculus::ToolKind::Line); break;
        case Qt::Key_V: session_->setTool(paroculus::ToolKind::Select); break;
        case Qt::Key_C: session_->setTool(paroculus::ToolKind::Circle); break;
        case Qt::Key_A: session_->setTool(paroculus::ToolKind::Arc); break;
        case Qt::Key_R: session_->setTool(paroculus::ToolKind::Rectangle); break;
        default:
            // One key per offer, by rank, matching the order the strip lists
            // them in. Shift declines instead: confirming what is proposed and
            // taking back what was declared are opposite actions and share the
            // digit that names the relation.
            if(event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9) {
                const size_t index = static_cast<size_t>(event->key() - Qt::Key_1);
                if(event->modifiers() & Qt::ShiftModifier) {
                    session_->declineInference(index);
                } else {
                    session_->confirmOffer(index);
                }
                break;
            }
            QQuickPaintedItem::keyPressEvent(event);
            return;
    }
    syncViewport();
    update();
    emit changed();
}

void SketchView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    syncTextureSize();
    syncViewport();
}

void SketchView::itemChange(ItemChange change, const ItemChangeData &value) {
    QQuickPaintedItem::itemChange(change, value);
    if(change == ItemSceneChange || change == ItemDevicePixelRatioHasChanged) {
        syncTextureSize();
        update();
    }
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
    QString text;
    if(!script_.steps.empty()) {
        // Progress is worth showing while watching: a gesture that looks wrong
        // is worth locating in the file, and the step number is how.
        text += QStringLiteral("%1 step %2/%3  ·  ")
                    .arg(playing_ ? QStringLiteral("script") : QStringLiteral("script done"))
                    .arg(scriptStep_)
                    .arg(script_.steps.size());
    }
    if(p.tool != paroculus::ToolKind::Select) {
        // The fixed strip, as a line of text until there is a surface to put it
        // in. Live: it tracks the placement in flight rather than the last one.
        text += QString::fromLatin1(paroculus::toolName(p.tool));
        for(const paroculus::ToolParameter &parameter : p.toolParameters) {
            text += QStringLiteral("  %1 %2")
                        .arg(QString::fromLatin1(parameter.name))
                        .arg(parameter.value, 0, 'f', 2);
        }
        // The transient strip: what a placement now would declare, and what it
        // is offering. Numbered because the number is the key that confirms it.
        const std::vector<paroculus::SnapCandidate> offers = p.offers();
        for(size_t i = 0; i < offers.size() && i < 9; i++) {
            const std::string_view name = paroculus::snapInfo(offers[i].kind).name;
            text += QStringLiteral("  [%1]%2%3")
                        .arg(i + 1)
                        .arg(QString::fromLatin1(name.data(), int(name.size())))
                        .arg(offers[i].confirmed ? QStringLiteral("*") : QString());
        }
        for(const paroculus::SnapCandidate &c : p.snapCandidates) {
            if(!c.autoCommits() || c.confirmed) continue;
            const std::string_view name = paroculus::snapInfo(c.kind).name;
            text += QStringLiteral("  %1").arg(
                QString::fromLatin1(name.data(), int(name.size())));
        }
        text += QStringLiteral("  ·  ");
    }
    if(!p.inferred.empty()) {
        // Shown at commit, not discovered later. Shift-number takes one back.
        text += QStringLiteral("declared %1  ·  ").arg(p.inferred.size());
    }
    text += QStringLiteral("%1  ·  dof: %2  ·  solve: %3 ms  ·  zoom: %4x")
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
//
// Two coordinate systems meet here and only here. Qt hands this item logical
// pixels — which is what interact wants, since a hit radius describes the hand
// and must not change with display density — while the raster has to be built
// at the panel's true resolution or the sketch is drawn at 1x and upscaled.
// syncTextureSize() gives QQuickPaintedItem a texture big enough to hold it;
// paint() keeps its own coordinates logical regardless, per Qt's contract for
// an explicit textureSize.
void SketchView::paint(QPainter *painter) {
    const QSize size = QSize(qRound(width()), qRound(height()));
    if(size.isEmpty()) return;

    const double dpr = devicePixelRatio();
    const QSize device(qRound(size.width() * dpr), qRound(size.height() * dpr));

    if(surface_.size() != device) {
        surface_ = QImage(device, QImage::Format_ARGB32_Premultiplied);
        // So drawImage below lays it down at logical size rather than blowing
        // it up to device size and overflowing the item.
        surface_.setDevicePixelRatio(dpr);
    }

    const paroculus::Presentation &p = session_->presentation();
    paroculus::Adornment adornment;
    adornment.selected = session_->selection().items();
    adornment.hovered = p.hovered;
    adornment.resisting = p.resisting;
    adornment.marqueeActive = p.marqueeActive;
    adornment.marqueeFrom = p.marqueeFrom;
    adornment.marqueeTo = p.marqueeTo;
    adornment.glyphs = session_->glyphs();
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
            // Only once the gesture defines one; before that it is still a chord.
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

    paroculus::renderDocument(session_->pose(), session_->viewport().view, adornment,
                              surface_.bits(), device.width(), device.height(),
                              static_cast<size_t>(surface_.bytesPerLine()), dpr);
    painter->drawImage(0, 0, surface_);
}
