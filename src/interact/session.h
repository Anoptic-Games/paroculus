// The interaction state machine.
//
// This is the seam the whole feel-freezing strategy depends on: it consumes
// abstract input events and emits document commands plus transient presentation
// state, and it knows nothing about Qt or Skia. That is what makes a gesture
// script runnable headlessly in CI, and therefore what makes a feel invariant
// something a test can hold rather than something a person has to re-check.
//
// Selection is the home state. Esc lands there. Everything the shell can do,
// a script can do, through exactly this interface.
#pragma once

#include <optional>
#include <vector>

#include "core/undo.h"
#include "interact/drag.h"
#include "interact/events.h"
#include "interact/hit.h"
#include "interact/selection.h"

namespace paroculus {

// What the shell should show after an event. Transient: regenerated per event,
// never a second source of truth about the document.
struct Presentation {
    // The entity under the cursor, for hover feedback.
    EntityId hovered;

    // Live marquee rectangle in screen pixels, while one is being dragged out.
    bool marqueeActive = false;
    Eigen::Vector2d marqueeFrom = Eigen::Vector2d::Zero();
    Eigen::Vector2d marqueeTo = Eigen::Vector2d::Zero();

    // Drag feedback. `resisting` is the attribution that makes saturation
    // legible rather than merely stiff.
    bool dragging = false;
    bool saturated = false;
    std::vector<ConstraintId> resisting;

    // The solve moved something the user cannot see. The shell pings the edge.
    bool rippledOffScreen = false;

    // Counted, not confirmed: deletion reports what it took with it rather than
    // asking permission.
    size_t deletedEntities = 0;
    size_t deletedRelations = 0;

    double solveMicroseconds = 0.0;

    // Displayed calmly, never as a progress bar or a warning: under-constraint
    // is the normal state and a free degree of freedom is a thing the user can
    // still push by hand.
    int dof = -1;
    SolveStatus status = SolveStatus::Unsolved;
};

enum class Key : uint8_t { Escape, Delete, Undo, Redo };

// Owns the interaction state over a document and its journal. The document is
// mutated only through the journal, so every visible change is undoable.
class Session {
public:
    Session(Document &doc, UndoJournal &journal);

    void setViewport(const Viewport &viewport);
    const Viewport &viewport() const { return viewport_; }

    // The pose currently on screen: committed seeds, overlaid with the in-flight
    // drag if one is running. Rendering and hit testing must both read this, or
    // the user picks one thing and selects another.
    Pose pose() const;

    const Selection &selection() const { return selection_; }
    const Presentation &presentation() const { return presentation_; }
    Signature signature() const { return selection_.signature(*doc_); }

    // Feeds one abstract event through the machine.
    void handle(const PointerEvent &event);
    void handle(Key key, Modifier modifiers = Modifier::None);

    // Rebuilds the derived indexes after the document changed underneath.
    void refresh();

    HitPolicy &policy() { return policy_; }

private:
    void beginDrag(EntityId grabbed, Point cursor);
    void updateDrag(Point cursor);
    void endDrag();
    void deleteSelection();
    // Solves the whole document and stores the result as seeds.
    //
    // Not journalled: opening a document and settling it is not an edit, and
    // there is nothing for the user to undo back to. Seeds record which branch
    // was shown, so writing them here is what makes that record true.
    void settle();

    Document *doc_;
    UndoJournal *journal_;
    Topology topology_;
    SpatialIndex index_;
    Selection selection_;
    Presentation presentation_;
    HitPolicy policy_;
    Viewport viewport_;

    std::optional<DragSession> drag_;
    // Where the press landed, so a drag can be told from a click by distance.
    Eigen::Vector2d pressScreen_ = Eigen::Vector2d::Zero();
    EntityId pressed_;
    bool pressActive_ = false;
    bool dragStarted_ = false;
    // Resistance diagnosis costs a second solve, so it runs on a subset of
    // frames. The counter is deterministic, which keeps scripts reproducible.
    int updateCount_ = 0;
};

}  // namespace paroculus
