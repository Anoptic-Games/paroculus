#include "shell/sketchview.h"

#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QQuickWindow>
#include <QWheelEvent>

#include <cstdio>

#include "app/scriptplay.h"
#include "interact/registry.h"
#include "interact/surface.h"
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
    // No ItemAcceptsInputMethod: the numeric strip is keystroke-driven and there
    // is no inputMethodEvent handler, so accepting input-method events would only
    // invite an IME to swallow digits and tool letters as preedit. Real text
    // entry, if it is ever wanted, arrives through a real text surface.
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
void SketchView::syncTextureSize() { setTextureSize(deviceSize()); }

// The backing raster's size in device pixels. Rounded once, from the logical
// size and the ratio: rounding width()*dpr at the texture and qRound(width())*dpr
// at the surface differ by a pixel at fractional logical sizes, and the surface
// then rescales onto the texture — a blur with no cause the drawing can show.
QSize SketchView::deviceSize() const {
    const double dpr = devicePixelRatio();
    return QSize(qMax(1, qRound(width() * dpr)), qMax(1, qRound(height() * dpr)));
}

void SketchView::syncViewport() {
    // A running script owns the viewport. Re-fitting here would replace the
    // transform its screen coordinates were recorded against, and every event
    // after that would land somewhere else.
    if(playing_) return;

    const int w = qMax(1, qRound(width()));
    const int h = qMax(1, qRound(height()));

    // The item has no size during construction, so the first framings are
    // provisional and only a real one latches.
    view_.frameOnce(session_->pose(), w, h, width() > 0.0 && height() > 0.0);

    paroculus::Viewport viewport;
    viewport.view = view_.transform(w, h);
    viewport.width = w;
    viewport.height = h;
    session_->setViewport(viewport);
}

PointerEvent SketchView::translate(const QPointF &position, Qt::MouseButtons buttons,
                                   Qt::KeyboardModifiers modifiers,
                                   PointerAction action) const {
    return translate(position, buttons, modifiers, action, 1);
}

PointerEvent SketchView::translate(const QPointF &position, Qt::MouseButtons buttons,
                                   Qt::KeyboardModifiers modifiers, PointerAction action,
                                   int clicks) const {
    // The view in force now is the only thing this reads from SketchView; the
    // rest is a pure remap, split into shelltest::translatePointer so a test can
    // reach it without a live session.
    return shelltest::translatePointer(position, buttons, modifiers,
                                       session_->viewport().view, action, clicks);
}

void SketchView::mousePressEvent(QMouseEvent *event) {
    forceActiveFocus();
    // The middle button pans and is not forwarded. Handing it to the session
    // too would make one gesture both a view change and whatever the session
    // made of a press it has no use for.
    if(event->button() == Qt::MiddleButton) {
        panning_ = true;
        panFrom_ = Eigen::Vector2d(event->position().x(), event->position().y());
        return;
    }
    session_->handle(translate(event->position(), event->button(), event->modifiers(),
                               PointerAction::Press));
    update();
    emit changed();
}

// Qt has already applied the platform's double-click interval and slop, which
// is the whole reason the count is decided out here: the interact layer must not
// grow a clock, and what counts as a double click is the window system's answer
// rather than ours.
void SketchView::mouseDoubleClickEvent(QMouseEvent *event) {
    forceActiveFocus();
    session_->handle(translate(event->position(), event->button(), event->modifiers(),
                               PointerAction::Press, 2));
    update();
    emit changed();
}

void SketchView::mouseMoveEvent(QMouseEvent *event) {
    if(panning_) {
        // Pixel for pixel, because a pan is a hand on the paper: the document
        // point under the cursor is the one that stays under it.
        const Eigen::Vector2d at(event->position().x(), event->position().y());
        view_.pan += at - panFrom_;
        panFrom_ = at;
        syncViewport();
        update();
        emit changed();
        return;
    }
    session_->handle(translate(event->position(), event->buttons(), event->modifiers(),
                               PointerAction::Move));
    update();
    emit changed();
}

void SketchView::mouseReleaseEvent(QMouseEvent *event) {
    // The middle release ends the pan and is consumed. Every other button is an
    // edit and always reaches the session, panning or not — so a left press that
    // opened a drag before the pan began still gets the release that ends it.
    // Swallowing that release stranded the drag: it went on to track the bare
    // cursor and committed on the next unrelated click.
    if(event->button() == Qt::MiddleButton) {
        panning_ = false;
        return;
    }
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

// Anchored on the cursor: the document point under it stays under it. Zooming
// toward the viewport centre instead means the thing being examined slides away
// exactly as it is magnified, so reaching it takes a zoom and then a pan.
//
// Pan is the composition's outermost term, so holding a point fixed is the
// difference between where it lands before and after the zoom — a subtraction
// rather than a second way of converting between the two spaces.
void SketchView::wheelEvent(QWheelEvent *event) {
    const double steps = event->angleDelta().y() / 120.0;
    const double previous = view_.zoom;
    view_.zoomAt(Eigen::Vector2d(event->position().x(), event->position().y()),
                 qBound(0.05, view_.zoom * std::pow(1.15, steps), 40.0),
                 qMax(1, qRound(width())), qMax(1, qRound(height())));
    // At the clamp nothing moved, and pushing an unchanged viewport would write
    // a step into a recording of a session in which nothing happened.
    if(view_.zoom == previous) return;
    syncViewport();
    update();
    emit changed();
}

// The keyboard is a projection of the action registry, not a second copy of it.
// Every binding here comes from the table, so an action added there is reachable
// from the keyboard without this function being edited — which is the property
// that keeps a surface from offering something the model would refuse.
//
// Keys that are not actions stay here: Esc, Tab and Enter drive the interaction
// state machine rather than the catalogue. Everything else is resolved by
// registry.h, so what a keystroke means is decided somewhere a headless test
// can read it rather than inside a Qt event handler.
namespace shelltest {

// The digit engraved on a key's face, 1..9, or 0 for anything else. Read from
// the physical key, never from text() or key(): registry.h defines `digit` as
// the engraved key, independent of shift and layout, and neither of those is.
// text() is the layout's shifted glyph — shift+1 is '!' — and key() is the
// resolved keysym — Key_Exclam — so reading the digit from either leaves the
// rank-one decline (shift+1) unreachable on a US layout and confirm broken on
// layouts whose unshifted number row prints symbols (AZERTY).
//
// On X11 and Wayland the native scan code is the evdev code offset by eight, so
// the top-row digits 1..9 occupy 10..18 whatever the layout or shift state; that
// range maps straight through. Outside it — the numpad, or a platform on another
// scan-code scheme — fall back to the Key_1..Key_9 keysyms, which shift does not
// disturb there.
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
    stroke.digit = engravedDigit(event);
    if(event->modifiers() & Qt::ShiftModifier) stroke.modifiers |= Modifier::Shift;
    if(event->modifiers() & Qt::ControlModifier) stroke.modifiers |= Modifier::Control;
    if(event->modifiers() & Qt::AltModifier) stroke.modifiers |= Modifier::Alt;
    return stroke;
}

// Qt pointer state to an abstract PointerEvent. Buttons and modifiers map
// straight across; the document position is filled through `view`, which is all
// SketchView::translate reads from its own state. Split out so the mapping is
// reachable without a live view — the translation tests build no SketchView.
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
    return PointerEvent::at(action, Eigen::Vector2d(position.x(), position.y()),
                            view, button, mods, clicks);
}

}  // namespace shelltest

void SketchView::keyPressEvent(QKeyEvent *event) {
    const bool typing = session_->presentation().numericActive;

    switch(event->key()) {
        case Qt::Key_Escape: session_->handle(Key::Escape); break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            session_->handle(Key::Enter,
                             (event->modifiers() & Qt::ShiftModifier) ? Modifier::Shift
                                                                      : Modifier::None);
            break;
        case Qt::Key_Tab: session_->handle(Key::Tab); break;
        case Qt::Key_Backspace:
            // Backspace edits the field while one is open, and deletes only
            // when there is nothing being typed.
            if(typing) session_->numericBackspace();
            else paroculus::invokeAction(*session_, "edit.delete");
            break;

        case Qt::Key_Delete: paroculus::invokeAction(*session_, "edit.delete"); break;

        default: {
            const paroculus::KeyBinding binding =
                paroculus::resolveKey(paroculus::contextOf(*session_), shelltest::strokeOf(event));
            switch(binding.kind) {
                case paroculus::KeyBinding::Kind::Text:
                    session_->type(binding.character);
                    break;
                case paroculus::KeyBinding::Kind::Action:
                    paroculus::invokeAction(*session_, binding.action->name, binding.arguments);
                    break;
                case paroculus::KeyBinding::Kind::None:
                    QQuickPaintedItem::keyPressEvent(event);
                    return;
            }
            break;
        }
    }
    // No syncViewport: a keystroke edits the sketch, and the view is not a
    // function of the sketch. It used to be, which is how confirming an offer
    // mid-chain came to re-frame the window under the cursor.
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
    update();
    emit changed();
}

void SketchView::redo() {
    session_->handle(Key::Redo);
    update();
    emit changed();
}

void SketchView::deleteSelection() {
    session_->handle(Key::Delete);
    update();
    emit changed();
}

// The one way back to a fitted framing, and the only thing that re-derives the
// view from the document. Nothing else may: a view that re-frames itself is a
// view the user cannot keep.
void SketchView::resetView() {
    view_ = paroculus::ViewState{};
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
        if(p.numericActive) {
            const QString name = p.numericTarget < p.toolParameters.size()
                                     ? QString::fromLatin1(p.toolParameters[p.numericTarget].name)
                                     : QString();
            text += QStringLiteral("  %1 [%2_]").arg(name).arg(
                QString::fromStdString(p.numericText));
        }
        text += QStringLiteral("  ·  ");
    }
    if(!p.closedLoop.empty()) {
        // An offer, not a fill. Making it a solid is stage 5's action; saying
        // so at the moment it closes is what stage 4 owes the user.
        text += QStringLiteral("closed loop (%1 edges)  ·  ").arg(p.closedLoop.size());
    }
    if(!p.inferred.empty()) {
        // Shown at commit, not discovered later. Shift-number takes one back.
        text += QStringLiteral("declared %1  ·  ").arg(p.inferred.size());
    }
    text += QStringLiteral("%1  ·  dof: %2  ·  solve: %3 ms  ·  zoom: %4x")
                       .arg(QString::fromLatin1(paroculus::statusName(p.status)))
                       .arg(p.dof)
                       .arg(p.solveMicroseconds / 1000.0, 0, 'f', 2)
                       .arg(view_.zoom, 0, 'f', 2);
    if(p.saturated) {
        text += QStringLiteral("  ·  resisting: %1").arg(p.resisting.size());
    }
    if(p.rippledOffScreen) text += QStringLiteral("  ·  moved off screen");
    if(p.deletedEntities > 0 || p.deletedRelations > 0 || p.degraded > 0) {
        // Counts, not a confirmation dialog: the user is told what went.
        text += QStringLiteral("  ·  deleted %1 shapes, %2 relations")
                    .arg(p.deletedEntities)
                    .arg(p.deletedRelations);
        // Three numbers rather than two, because a deletion no longer only
        // removes. A region, tag or group that lost a member shrank rather than
        // died, and it is on screen in its broken state saying so — the count
        // was computed for exactly this and read by nothing.
        if(p.degraded > 0) text += QStringLiteral(", degraded %1").arg(p.degraded);
    }
    // What the last structure operation did beyond moving things.
    //
    // A transform that retargeted four axis relations, rewrote three dimensions,
    // or left two resisting has edited the document in ways the motion on screen
    // does not show; a copy that could not bring a relation has quietly given
    // the user something less constrained than what they copied. Counts, in the
    // same register as the deletion counts above and for the same reason.
    {
        const paroculus::Presentation::StructureReport &s = p.structure;
        if(s.transformError != paroculus::TransformError::None) {
            text += QStringLiteral("  ·  transform refused: %1")
                        .arg(QString::fromLatin1(
                            paroculus::transformErrorName(s.transformError)));
        }
        if(s.compoundError != paroculus::CompoundError::None) {
            text += QStringLiteral("  ·  compound refused: %1")
                        .arg(QString::fromLatin1(
                            paroculus::compoundErrorName(s.compoundError)));
        }
        if(s.retargeted > 0) {
            text += QStringLiteral("  ·  retargeted %1 axis relations to a cluster frame")
                        .arg(s.retargeted);
        }
        if(s.rescaled > 0) text += QStringLiteral("  ·  rescaled %1 dimensions").arg(s.rescaled);
        if(s.straddling > 0) {
            text += QStringLiteral("  ·  %1 dimensions reach outside and will resist")
                        .arg(s.straddling);
        }
        if(s.copied > 0) text += QStringLiteral("  ·  copied %1 shapes").arg(s.copied);
        if(s.droppedRelations > 0 || s.droppedRegions > 0 || s.droppedTags > 0) {
            text += QStringLiteral("  ·  dropped %1 relations, %2 fills, %3 tags at the boundary")
                        .arg(s.droppedRelations)
                        .arg(s.droppedRegions)
                        .arg(s.droppedTags);
        }
    }
    // A broken tag costs the affordances and nothing else, which is exactly why
    // it has to be said: nothing on screen is missing, so silence would read as
    // the handles having been turned off rather than the rectangle having
    // stopped being one.
    if(!p.brokenTags.empty()) {
        text += QStringLiteral("  ·  %1 tags broken").arg(p.brokenTags.size());
    }
    // The rectangle panel. A whole tag under the selection offers its width and
    // height, and they are the slots its corner handles drive.
    for(const paroculus::Session::RectanglePanel &panel : session_->rectanglePanels()) {
        text += QStringLiteral("  ·  rect %1 x %2%3")
                    .arg(panel.size.width, 0, 'f', 2)
                    .arg(panel.size.height, 0, 'f', 2)
                    .arg(panel.size.widthDimension.valid() || panel.size.heightDimension.valid()
                             ? QStringLiteral(" (driven)")
                             : QString());
    }
    // Which of the two silences this is. An area enclosed by crossing segments
    // encloses something the model cannot name, and refusing it with the same
    // absence as "these edges enclose nothing" tells the user the wrong thing
    // about a drawing that plainly encloses something.
    if(p.crossing) {
        text += QStringLiteral("  ·  edges cross: intersection points are needed to fill this");
    }
    return text;
}

int SketchView::dof() const { return session_->presentation().dof; }

QString SketchView::solveStatus() const {
    return QString::fromLatin1(paroculus::statusName(session_->presentation().status));
}

namespace {

// One surface entry, as QML sees it. A plain map rather than a model type: the
// list is short, it is rebuilt whenever anything changes, and a model would be
// a second place for applicability to be decided.
QVariantMap entryOf(const paroculus::SurfaceEntry &entry) {
    QVariantMap map;
    map[QStringLiteral("name")] = QString::fromLatin1(entry.action->name.data(),
                                                      int(entry.action->name.size()));
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

QVariantList SketchView::strip() const {
    QVariantList out;
    for(const paroculus::SurfaceEntry &entry : paroculus::stripEntries(*session_)) {
        out.append(entryOf(entry));
    }
    return out;
}

// Back to front, so the list reads the way the drawing stacks. The implicit
// base layer is left out: it has no record, so there is nothing to toggle and a
// row offering to would be a row that cannot do anything.
QVariantList SketchView::layers() const {
    QVariantList out;
    for(paroculus::LayerId id : paroculus::layerOrder(session_->document())) {
        const paroculus::LayerRecord *layer = session_->document().layers().find(id);
        if(layer == nullptr) continue;
        QVariantMap row;
        row["id"] = static_cast<int>(layer->id.value());
        row["name"] = QString::fromStdString(layer->name);
        row["visible"] = layer->visible;
        row["locked"] = layer->locked;
        out.prepend(row);
    }
    return out;
}

int SketchView::hiddenInfluences() const {
    return static_cast<int>(session_->presentation().hiddenInfluences.size());
}

int SketchView::brokenRegions() const {
    return static_cast<int>(session_->presentation().brokenRegions.size());
}

int SketchView::brokenTags() const {
    return static_cast<int>(session_->presentation().brokenTags.size());
}

QVariantList SketchView::rectangles() const {
    QVariantList out;
    for(const paroculus::Session::RectanglePanel &panel : session_->rectanglePanels()) {
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

QVariantList SketchView::palette(const QString &query) const {
    QVariantList out;
    const std::string text = query.toStdString();
    for(const paroculus::SurfaceEntry &entry : paroculus::paletteEntries(*session_, text)) {
        out.append(entryOf(entry));
    }
    return out;
}

bool SketchView::run(const QString &name, const QVariantMap &arguments) {
    paroculus::ActionArguments args;
    for(auto it = arguments.begin(); it != arguments.end(); ++it) {
        bool ok = false;
        const double value = it.value().toDouble(&ok);
        if(ok) args.set(it.key().toStdString(), value);
    }
    const bool ran = paroculus::invokeAction(*session_, name.toStdString(), args);
    // No syncViewport: an action edits the sketch, and the view is not a
    // function of the sketch. Growing a drawing is not a request to look
    // somewhere else.
    update();
    emit changed();
    return ran;
}

void SketchView::clearPreview() {
    if(ghostPose_.empty()) return;
    ghostPose_.clear();
    update();
}

QString SketchView::previewOf(const QString &name, int assignment) {
    clearPreview();
    const paroculus::Action *action = paroculus::findAction(name.toStdString());
    if(action == nullptr || !action->generated || assignment < 0) return {};

    const std::optional<paroculus::ImpositionPreview> preview =
        session_->previewImposition(action->constraintKind, size_t(assignment));
    if(!preview) return QStringLiteral("not applicable");

    // The ghost, which is the half of a preview PRINCIPLES calls the payoff:
    // the geometry moves to where commit would put it and the catalogue becomes
    // learnable by looking. Armed for anything that could hold — a relation that
    // cannot is answered by the verdict, and showing a pose the commit would
    // refuse would be promising the one thing preview must never promise.
    if(preview->check.committable()) {
        ghostPose_ = preview->pose;
        update();
    }

    switch(preview->check.verdict) {
        case paroculus::CandidateVerdict::Consistent:
            // Movement-free is the promise, so the exception is what gets said.
            // The near-parallel snap-shut is the one imposition that moves
            // geometry, and showing the motion is the whole of why it is
            // allowed to.
            return preview->motion <= session_->surfacePolicy().movementTolerance
                       ? QStringLiteral("holds")
                       : QStringLiteral("holds · moves %1").arg(preview->motion, 0, 'f', 2);
        case paroculus::CandidateVerdict::Redundant:
            // Flagged, not refused: redundancy is where later edits go to die
            // rather than a fault today.
            return QStringLiteral("already implied");
        case paroculus::CandidateVerdict::Inconsistent:
            return preview->check.attributed && !preview->check.conflicting.empty()
                       ? QStringLiteral("conflicts with %1")
                             .arg(preview->check.conflicting.size())
                       : QStringLiteral("cannot hold");
        default:
            return QStringLiteral("cannot hold");
    }
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
    if(width() <= 0.0 || height() <= 0.0) return;

    const double dpr = devicePixelRatio();
    const QSize device = deviceSize();

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
    // A tag's handles appear when its geometry is selected, which is the same
    // rule that decides a fill is selected: a tag has no handle of its own, so
    // it is named by naming what it is over. Showing them always would put a
    // ring of squares on every rectangle in the document.
    adornment.handledTags = session_->selectedTags();
    adornment.ghostPose = ghostPose_;
    // The drawn grid is the snap policy's, so the lines are where placement
    // actually lands. Disabled snapping draws none rather than a grid that
    // nothing falls on.
    const paroculus::SnapPolicy &snap = session_->snapPolicy();
    adornment.gridStep = snap.gridEnabled ? snap.gridStep : 0.0;
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
