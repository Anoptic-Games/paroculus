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

#include <memory>

#include "core/undo.h"
#include "interact/drag.h"
#include "interact/events.h"
#include "interact/glyphs.h"
#include "interact/hit.h"
#include "interact/selection.h"
#include "interact/snap.h"
#include "interact/tools.h"

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

    // The tool in force, and what it wants shown. Select is the home state, so
    // this reads Select whenever no creation tool is running.
    ToolKind tool = ToolKind::Select;
    ToolPreview toolPreview;
    // The fixed strip's contents, live. Empty while no tool is running.
    std::vector<ToolParameter> toolParameters;

    // What a placement now would infer, ranked. Auto-committing kinds are what
    // the ghost is already showing; the offered ones are the one-key
    // confirmations. Both are here because the user is entitled to see what is
    // about to be declared before it is declared.
    std::vector<SnapCandidate> snapCandidates;

    // The offered subset, in rank order — what the transient strip near the
    // cursor lists, and what confirmOffer() indexes into.
    std::vector<SnapCandidate> offers() const {
        std::vector<SnapCandidate> out;
        for(const SnapCandidate &c : snapCandidates) {
            if(c.tier() == SnapTier::Offered) out.push_back(c);
        }
        return out;
    }
    // What the last placement actually declared. No silent changes: an inferred
    // constraint is shown at commit, not discovered later.
    std::vector<ConstraintId> inferred;

    double solveMicroseconds = 0.0;

    // Displayed calmly, never as a progress bar or a warning: under-constraint
    // is the normal state and a free degree of freedom is a thing the user can
    // still push by hand.
    int dof = -1;
    SolveStatus status = SolveStatus::Unsolved;
};

enum class Key : uint8_t { Escape, Delete, Undo, Redo };

class ScriptRecorder;

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
    SnapPolicy &snapPolicy() { return snapPolicy_; }
    GlyphPolicy &glyphPolicy() { return glyphPolicy_; }

    // The constraint marks worth drawing this frame: every relation the
    // document holds, reduced to what the overlay's budget can carry, plus
    // ghosts for what a placement in flight would declare.
    //
    // Computed on demand rather than cached in Presentation because it depends
    // on the pose, which a drag changes every frame, and a stale overlay would
    // put marks where the geometry no longer is.
    std::vector<GlyphMark> glyphs() const;

    // Activates a creation tool, or returns to selection with ToolKind::Select.
    // Switching tools abandons whatever the previous one had in flight, which
    // is the same thing Esc would have done.
    void setTool(ToolKind kind);
    ToolKind tool() const { return presentation_.tool; }

    // Confirms the offered candidate at `index` in presentation().offers(), so
    // the placement in flight will declare it. Out-of-range indices are
    // ignored: a script written against a different document should replay as a
    // no-op rather than confirming whatever happens to be at that rank now.
    //
    // The confirmation holds only while the relation is still available.
    // Swinging far enough that the candidate stops being generated lapses it,
    // because a parallel to a segment you have rotated away from is not a
    // relation anyone can declare.
    void confirmOffer(size_t index);

    // Removes one constraint from the placement just committed, as its own
    // undoable step.
    //
    // Deliberately finer-grained than undo. Undo takes back the placement and
    // everything it declared, because that was one gesture; decline takes back
    // one relation and leaves the geometry, because disagreeing with an
    // inference is not the same as regretting the stroke.
    void declineInference(size_t index);

    // Attaches a recorder, or detaches with nullptr. Every event the session is
    // asked to handle from here on is captured, which is what makes a recording
    // a recording of the session rather than of the shell's interpretation of
    // one: a script recorded through this hook replays through the identical
    // path, and re-recording the replay reproduces the file.
    //
    // The recorder outlives the call, not the session. Caller owns it.
    void setRecorder(ScriptRecorder *recorder) { recorder_ = recorder; }

private:
    // Applies whatever a tool asked for, and tells the tool only if it landed.
    void runTool(ToolOutput output, std::vector<ConstraintId> inferred = {});
    void refreshToolPresentation();
    // Runs inference for a placement at `cursor`. One call feeds both the ghost
    // and the commit, so a previewed candidate set that differs from the
    // committed one is not a reachable state.
    SnapResult inferAt(Point cursor) const;
    void rememberSnaps(const std::vector<SnapCandidate> &committed);

    void beginDrag(EntityId grabbed, Point cursor);
    void updateDrag(Point cursor);
    void endDrag();
    void deleteSelection();

    Document *doc_;
    UndoJournal *journal_;
    Topology topology_;
    // The solved pose, kept rather than committed. This is the derived cache
    // PRINCIPLES describes: rendering and hit testing read it through pose(),
    // and it is rebuilt from the document rather than stored in it.
    SolveContext settled_;
    SpatialIndex index_;
    Selection selection_;
    Presentation presentation_;
    HitPolicy policy_;
    SnapPolicy snapPolicy_;
    GlyphPolicy glyphPolicy_;
    Viewport viewport_;

    // The relations captured when a chain's first click landed. They cannot be
    // declared yet — the point they bind has no id until the segment that
    // justifies it exists — so they wait one click and are applied to the start
    // point when it is created.
    std::vector<SnapCandidate> pendingStartSnaps_;
    // Offers confirmed for the placement in flight, cleared when it commits or
    // is abandoned. A confirmation is about one placement, not about the tool.
    std::vector<std::pair<SnapKind, EntityId>> confirmedOffers_;
    // The last placement position inference ran against, so confirming an offer
    // can re-run it and update the ghost without waiting for a mouse move.
    Point lastCursor_;
    bool haveLastCursor_ = false;
    // Kinds committed recently in this document, most recent first. Ranking is
    // contextual and document-local; this is the whole of the context.
    std::vector<SnapKind> recentSnaps_;

    ScriptRecorder *recorder_ = nullptr;
    // Null in the home state. Creation tools are shallow, so there is at most
    // one and it never nests.
    std::unique_ptr<Tool> tool_;

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
