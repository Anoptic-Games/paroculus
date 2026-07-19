#include "interact/session.h"

#include <algorithm>

#include "interact/script.h"

namespace paroculus {
namespace {

// Every Nth drag frame runs the extra hard-pin solve that names the resisting
// constraints. A counter rather than a clock, so a scripted gesture produces
// the same diagnoses on every machine and every run.
constexpr int RESISTANCE_INTERVAL = 4;

}  // namespace

Session::Session(Document &doc, UndoJournal &journal)
    : doc_(&doc), journal_(&journal), topology_(doc) {
    refresh();
}

void Session::setViewport(const Viewport &viewport) {
    viewport_ = viewport;
    // Recorded as a step rather than as file-level preamble: a session that
    // pans or zooms mid-gesture ran under several views, and replaying its
    // screen coordinates under only the first one would put every event after
    // the change somewhere else.
    if(recorder_ != nullptr) recorder_->viewport(viewport);
}

Pose Session::pose() const {
    // Three layers, and the order is the whole story: the document's committed
    // seeds, the solved result over them, the in-flight drag over that.
    Pose pose(*doc_);
    pose.overlay(settled_.params());
    if(drag_) pose.overlay(drag_->context().params());
    return pose;
}

void Session::refresh() {
    topology_.markDirty();

    // Solved geometry is a derived cache, so it is kept here and never written
    // back as seeds. Committing it on open would dirty a document nobody
    // edited — and because a Newton solve is not a fixpoint in the last bits,
    // opening the same file twice would produce two different files. Seeds are
    // authored intent and branch choice; they change when the user edits.
    settled_ = SolveContext::forWholeDocument(*doc_);
    if(!settled_.empty()) {
        SolveOptions options;
        options.diagnoseFailures = false;
        const SolveOutcome outcome = solve(*doc_, settled_, options);
        // The readout follows the document, so an undo or a deletion updates it
        // rather than leaving a stale number on screen.
        presentation_.dof = outcome.dof;
        presentation_.status = outcome.status;
        // A failed solve leaves the parameters at the seeds it was handed, so
        // an overlay of them would only restate the document. Drop it and show
        // what the document says, which is the honest thing to show.
        if(!outcome.ok()) settled_ = SolveContext();
    }

    index_.rebuild(pose());
}

void Session::handle(const PointerEvent &event) {
    if(recorder_ != nullptr) recorder_->pointer(event);
    presentation_.rippledOffScreen = false;
    const Pose current = pose();

    switch(event.action) {
        case PointerAction::Move: {
            if(pressActive_) {
                const double travelled = (event.screen - pressScreen_).norm();
                // A sloppy click stays a click until it has travelled far
                // enough to mean something else.
                if(!dragStarted_ && travelled > policy_.dragThreshold) {
                    dragStarted_ = true;
                    if(pressed_.valid()) beginDrag(pressed_, event.document);
                }
                if(drag_) {
                    updateDrag(event.document);
                } else if(dragStarted_) {
                    presentation_.marqueeActive = true;
                    presentation_.marqueeTo = event.screen;
                }
                return;
            }
            const std::optional<Hit> hit =
                hitTest(current, index_, viewport_.view, event.screen, policy_,
                        selection_.items());
            presentation_.hovered = hit ? hit->entity : EntityId();
            return;
        }

        case PointerAction::Press: {
            if(event.button != Button::Left) return;
            pressActive_ = true;
            dragStarted_ = false;
            pressScreen_ = event.screen;
            presentation_.marqueeFrom = event.screen;
            presentation_.marqueeTo = event.screen;

            const std::optional<Hit> hit =
                hitTest(current, index_, viewport_.view, event.screen, policy_,
                        selection_.items());
            pressed_ = hit ? hit->entity : EntityId();

            if(!pressed_.valid()) {
                // Empty space: a click clears, a drag becomes a marquee.
                if(!has(event.modifiers, Modifier::Shift)) selection_.clear();
                return;
            }
            if(has(event.modifiers, Modifier::Shift)) {
                selection_.toggle(pressed_);
            } else if(!selection_.contains(pressed_)) {
                // Clicking outside the selection replaces it; clicking inside
                // keeps it, so a multi-selection can be dragged as one thing.
                selection_.set(connectedRun(*doc_, topology_, pressed_));
            }
            return;
        }

        case PointerAction::Release: {
            if(event.button != Button::Left) return;
            if(drag_) {
                endDrag();
            } else if(dragStarted_ && presentation_.marqueeActive) {
                std::vector<EntityId> caught =
                    marquee(current, viewport_.view, presentation_.marqueeFrom, event.screen);
                if(has(event.modifiers, Modifier::Shift)) {
                    for(EntityId id : caught) selection_.add(id);
                } else {
                    selection_.set(std::move(caught));
                }
            }
            pressActive_ = false;
            dragStarted_ = false;
            pressed_ = EntityId();
            presentation_.marqueeActive = false;
            return;
        }
    }
}

void Session::handle(Key key, Modifier modifiers) {
    if(recorder_ != nullptr) recorder_->key(key, modifiers);
    presentation_.rippledOffScreen = false;
    switch(key) {
        case Key::Escape:
            // Esc ascends a level of depth, and clears once at the home state.
            // Selection is where Esc always eventually lands.
            if(!selection_.ascend(*doc_, topology_)) selection_.clear();
            return;

        case Key::Delete:
            deleteSelection();
            return;

        case Key::Undo:
            if(journal_->undo(*doc_)) refresh();
            return;

        case Key::Redo:
            if(journal_->redo(*doc_)) refresh();
            return;
    }
    (void)modifiers;
}

void Session::beginDrag(EntityId grabbed, Point cursor) {
    drag_ = DragSession::begin(*doc_, topology_, grabbed, policy_);
    if(!drag_) return;
    presentation_.dragging = true;
    updateCount_ = 0;
    updateDrag(cursor);
}

void Session::updateDrag(Point cursor) {
    if(!drag_) return;
    const bool diagnose = (updateCount_++ % RESISTANCE_INTERVAL) == 0;
    const DragUpdate update = drag_->update(*doc_, viewport_, cursor, diagnose);

    presentation_.saturated = update.saturated;
    presentation_.rippledOffScreen = update.rippledOffScreen;
    presentation_.solveMicroseconds = update.microseconds;
    // Attribution persists between diagnosis frames, so the highlight does not
    // strobe at the interval rather than at the resistance.
    if(diagnose || !update.saturated) presentation_.resisting = update.resisting;
}

void Session::endDrag() {
    if(!drag_) return;
    // Release commits what is on screen. Nothing springs back, and the whole
    // gesture is one undo step.
    std::vector<Command> commit = drag_->commit(*doc_);
    drag_.reset();
    presentation_.dragging = false;
    presentation_.saturated = false;
    presentation_.resisting.clear();

    if(!commit.empty()) journal_->applyStep(*doc_, "drag", std::move(commit));
    refresh();
}

void Session::deleteSelection() {
    presentation_.deletedEntities = 0;
    presentation_.deletedRelations = 0;
    if(selection_.empty()) return;

    // One gesture, one undo step, whatever it took with it.
    //
    // Each cascade is computed against the document as it stands, so two
    // selected entities sharing a dependent name it twice. The second removal
    // would be refused and roll the whole step back, so dedupe by
    // (command alternative, record id) before applying.
    std::vector<std::pair<size_t, uint32_t>> seen;
    std::vector<Command> step;

    auto identify = [](const Command &c) {
        return std::visit(
            [](const auto &command) -> uint32_t {
                using C = std::decay_t<decltype(command)>;
                if constexpr(requires { command.id; }) {
                    return command.id.value();
                } else {
                    return command.record.id.value();
                }
            },
            c);
    };

    for(EntityId id : selection_.items()) {
        for(const Command &c : deletionStep(*doc_, id)) {
            const std::pair<size_t, uint32_t> key{c.index(), identify(c)};
            if(std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            step.push_back(c);
        }
    }
    if(step.empty()) return;

    // Counts, not a confirmation dialog. The user is told what went, after it
    // went, and undo is one keystroke away.
    for(const Command &c : step) {
        if(std::holds_alternative<RemoveRecord<EntityRecord>>(c)) {
            presentation_.deletedEntities++;
        } else {
            presentation_.deletedRelations++;
        }
    }

    if(journal_->applyStep(*doc_, "delete", std::move(step)) == CommandError::None) {
        selection_.clear();
        refresh();
    } else {
        presentation_.deletedEntities = 0;
        presentation_.deletedRelations = 0;
    }
}

}  // namespace paroculus
