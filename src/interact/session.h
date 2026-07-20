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
#include <string>
#include <vector>

#include <memory>

#include "core/bake.h"
#include "core/composition.h"
#include "core/undo.h"
#include "interact/drag.h"
#include "interact/events.h"
#include "interact/glyphs.h"
#include "interact/hit.h"
#include "interact/impose.h"
#include "interact/loops.h"
#include "interact/numeric.h"
#include "solve/diagnose.h"
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
    // asking permission. Degraded is separate from deleted because a region,
    // tag or group that lost a member is still there — visibly, in its broken
    // state — and reporting it as gone would be the opposite of true.
    size_t deletedEntities = 0;
    size_t deletedRelations = 0;
    size_t degraded = 0;

    // Invisible geometry that took part in moving something visible.
    //
    // Hidden still constrains, which is what makes hiding different from
    // deleting; this is the other half of that bargain. A drawing that
    // rearranges itself under something not on screen is the haunted feeling
    // the no-silent-changes policy exists to rule out, so the influence is
    // named and the geometry doing it can be pointed at.
    std::vector<EntityId> hiddenInfluences;

    // Regions and tags that no longer have the parts to mean what they say.
    // Recomputed per refresh from the document, never accumulated: a region is
    // broken because of what it is now, not because of what once happened.
    std::vector<RegionId> brokenRegions;
    std::vector<TagId> brokenTags;

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
    // The numeric entry in flight: which parameter, and the text so far. Empty
    // text with active true is a field the user has just opened.
    bool numericActive = false;
    size_t numericTarget = 0;
    std::string numericText;

    // What the last placement actually declared. No silent changes: an inferred
    // constraint is shown at commit, not discovered later.
    std::vector<ConstraintId> inferred;

    // How the last imposed dimension or relation fared against what was already
    // declared, and the constraints it disagreed with when it could not hold.
    // Consistent with an empty set whenever nothing was imposed.
    //
    // The conflicting set is walkable rather than merely rendered: it is filled
    // by the suppression counterfactual, and selectConflicting() turns it into
    // an ordinary selection the user steps through. Empty with `attributed`
    // false means the conflict could not be pinned on any single relation, not
    // that there is none.
    CandidateVerdict impositionVerdict = CandidateVerdict::Consistent;
    std::vector<ConstraintId> conflicting;
    bool conflictAttributed = false;

    // A driving imposition was refused as inconsistent, so the reference
    // measurement is on offer. Stage 4 downgraded silently because there was no
    // surface to ask on; the mechanism is unchanged and the choice is now the
    // user's. Names the kind and strength the offer would invoke.
    bool downgradeOffered = false;

    // How far the last imposition moved geometry, in document units. Zero is
    // what imposition promises; the near-parallel snap-shut is the exception,
    // and this is how it shows its motion rather than making it quietly.
    double impositionMotion = 0.0;

    // What the last relation-imposing action created, so a surface can name it
    // and a test can find it. Null when nothing was recorded — which is what
    // measure-once always leaves behind.
    ConstraintId imposed;

    // The outline the last placement closed, in boundary order, when it closed
    // one. An offer and nothing more: filling it is a region record, which
    // makeSolid() attaches. Detecting leaves the document exactly as it was, or
    // ignoring the offer would not be free.
    std::vector<EntityId> closedLoop;

    // An outline that looks closed and is not, and the joints heal-and-fill
    // would shut. Never both this and closedLoop: a run is one or the other,
    // and the two offers differ in exactly the way that matters — one moves
    // nothing, the other moves geometry by the epsilon it names here.
    std::vector<EntityId> healableLoop;
    std::vector<LoopGap> healableGaps;
    double healableWidestGap = 0.0;

    // The region the last make-solid or heal-and-fill attached. Null when none.
    RegionId filled;

    // The composite the last union, intersect or subtract made. Null when none,
    // and cleared by lifting one back out.
    RegionId composed;

    double solveMicroseconds = 0.0;

    // Displayed calmly, never as a progress bar or a warning: under-constraint
    // is the normal state and a free degree of freedom is a thing the user can
    // still push by hand.
    int dof = -1;
    SolveStatus status = SolveStatus::Unsolved;
};

enum class Key : uint8_t { Escape, Delete, Undo, Redo, Enter, Tab };

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

    const Document &document() const { return *doc_; }
    bool canUndo() const { return journal_->canUndo(); }
    bool canRedo() const { return journal_->canRedo(); }

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

    // Feeds one character to the numeric entry, opening one on the tool's first
    // parameter if none is running. Approximate gesture and exact entry are two
    // entrances to the same edit, so this drives the tool already in flight
    // rather than opening anything that replaces it.
    void type(char c);
    void numericBackspace();
    void numericCancel();
    // Moves to the next parameter of the running tool, wrapping.
    void numericAdvance();
    // Resolves the placement so the typed value holds exactly, and commits it.
    // When `impose`, the value also lands as a driving dimension — the one
    // extra key that turns a measurement into a declaration, in the same undo
    // step as the geometry it measures.
    //
    // Enter finishes the gesture. It has to: the reason this entrance exists is
    // that a drag cannot land on a number, so asking for a mouse click
    // afterwards would ask the hand for the very precision the digits were
    // supplying — and the click would arrive at the pointer's position and
    // overwrite the resolved one. Tab moves between fields without committing,
    // which is how a placement with two of them is typed out in full.
    void numericResolve(bool impose);

    // ---------------------------------------------------------------------
    // Imposition
    // ---------------------------------------------------------------------

    // Declares `kind` over the current selection at `strength`.
    //
    // The noun-verb half of the tool, and the one that has to move nothing:
    // a valued relation captures the measurement already true, so the solver
    // has nothing to do and the drawing does not shift under the declaration.
    // Supplying `value` is the other thing — a value edit — and that is allowed
    // to move geometry, because editing a value is one of the two ways
    // PRINCIPLES says geometry is meant to move.
    //
    // assignment: which reading of the selection, for the kinds that read one
    //   operand against the other. Out of range is refused rather than clamped:
    //   a script written against a different selection has asked for something
    //   that is not there, and silently imposing the other reading would
    //   declare the wrong relation.
    //
    // Returns whether anything was applied. A driving imposition that cannot
    // hold returns false and leaves the downgrade on offer rather than
    // committing a reference measurement the user did not ask for — which is
    // the one thing this stage changes about a mechanism that already worked.
    // Redundant is committed and flagged: redundancy is where later edits go to
    // die, not a fault today.
    bool impose(ConstraintKind kind, Strength strength, size_t assignment = 0,
                std::optional<double> value = std::nullopt);

    // Attaches a region to the closed outline on offer.
    //
    // Not a conversion: the record references the cycle of edge IDs, no
    // geometry is copied, no path is synthesized and no constraint is touched.
    // The outline keeps operating, dragging a vertex moves the fill because the
    // fill has no geometry of its own to go stale, and the inverse is deleting
    // one record.
    bool makeSolid();

    // Shuts the gaps in an outline that only looks closed, then fills it.
    //
    // The exception to movement-free imposition, and an explicit one: the
    // epsilon motion is the point of the action rather than a side effect, so
    // it is offered separately from make-solid and reports what it moved.
    bool healAndFill();

    // Flips the selected relations between driving and reference.
    //
    // A reference dimension and a driving dimension are the same object with a
    // toggle. Flipping it is how a measurement is promoted to intent, and how
    // an over-constraint downgrade demotes one — the same operation from either
    // side, which is why there is one action and not two.
    bool toggleDriving();

    // Makes the conflicting set the selection, so it can be walked.
    //
    // A conflict is presented as a selectable set of constraints plus tinted
    // geometry, never a modal dialog. There is no error state that suspends
    // editing; there are only states with more or less diagnostic adornment.
    bool selectConflicting();

    // -----------------------------------------------------------------------
    // Composition
    // -----------------------------------------------------------------------

    // The regions the current selection reaches, in ID order.
    //
    // A region has no handle of its own — it is reached through the geometry
    // bounding it, which is what makes a fill a view of an outline rather than
    // an object beside one. So selecting two outlines is how a user names two
    // operands for a boolean.
    std::vector<RegionId> selectedRegions() const;

    // The layer an action acts on: the one named, else the selection's own.
    LayerId targetLayer(std::optional<double> named = std::nullopt) const;

    // The frontmost layer, or null when the document has none.
    LayerId topLayer() const;

    bool newLayer();
    bool assignLayer(LayerId layer);

    // Hiding leaves the geometry constraining exactly as it did, which is what
    // separates hiding from deleting, and is why the influence indication
    // exists to go with it.
    bool setLayerVisible(LayerId layer, bool visible);

    // Locking means "this does not move", which in a solver world means its
    // parameters join the fixed set. Not a Pin: a pin is a relation the user
    // asked for and can over-constrain, while a lock is presentation state and
    // must never be able to make a system inconsistent.
    bool setLayerLocked(LayerId layer, bool locked);

    bool moveLayer(LayerId layer, int delta);

    // Combines the selected regions without consuming them.
    //
    // The operands stay live records, keep their constraints, and can be
    // constrained to each other — the hole stays concentric with the plate
    // because that is an ordinary coincidence between two outlines that happen
    // to be operands. Destructive path booleans exist nowhere in this model.
    bool composeRegions(CompositeOp op);

    // Dismantles the selected composites, restoring their operands to view. A
    // real inverse, because nothing was consumed to make one.
    bool liftComposite();

    // Alpha overwrite: the selected regions carve visibility out of what their
    // layer accumulated below them instead of painting over it.
    bool togglePunch();

    bool moveRegion(int delta);

    bool groupSelection();
    bool dissolveGroups();

    // Flattens the visible drawing for export. The one destructive path in the
    // tool, and it leads out: nothing is written back, and there is no
    // in-document bake.
    Bake bake() const;

    // What imposing `kind` over the current selection would do, without doing
    // any of it. The hover preview, and the reason the catalogue is learnable by
    // looking rather than by reading.
    //
    // The document is byte-identical afterwards, by construction rather than by
    // discipline: the candidate rides in as a speculative extra on a forked
    // context and nothing writes back.
    std::optional<ImpositionPreview> previewImposition(
        ConstraintKind kind, size_t assignment = 0,
        std::optional<double> value = std::nullopt) const;

    // The relations the current selection admits, ranked — what the context
    // strip lists. A projection of the taxonomy and this document's usage, and
    // nothing else.
    std::vector<RelationOffer> relationOffers() const;

    SurfacePolicy &surfacePolicy() { return surfacePolicy_; }
    const SurfacePolicy &surfacePolicy() const { return surfacePolicy_; }

    // Selection of relations, for the surfaces that walk them.
    void selectConstraint(ConstraintId id, bool additive = false);

    // Sets the geometry selection outright.
    //
    // The programmatic entrance, used by the actions that compute a selection
    // rather than receive one — walking a conflict set is the first — and by
    // headless tests, which have a document and no pointer. Not a recording
    // surface: what a script records is the click, and replaying the click
    // reproduces the selection. A shell that called this instead of feeding a
    // pointer event would produce a session that does not replay.
    void select(std::vector<EntityId> ids);

    // Attaches a recorder, or detaches with nullptr. Every event the session is
    // asked to handle from here on is captured, which is what makes a recording
    // a recording of the session rather than of the shell's interpretation of
    // one: a script recorded through this hook replays through the identical
    // path, and re-recording the replay reproduces the file.
    //
    // The recorder outlives the call, not the session. Caller owns it.
    void setRecorder(ScriptRecorder *recorder) { recorder_ = recorder; }

private:
    // Writes an Action step, for the edits that have no other recording
    // surface behind them.
    //
    // Every other way of changing the document arrives through a method that
    // records itself — a pointer event, a keystroke, a tool change — so an
    // action that dispatched to one of those needed no recording of its own and
    // re-recorded as the change it caused. Imposition and the fill actions have
    // no such effect to be re-recorded as: they are reached by clicking a strip
    // entry, and nothing else about that click is an input the session sees. So
    // they record themselves, by the name and arguments that would reproduce
    // them, and record → replay → record stays the identity the format exists
    // to guarantee.
    //
    // Records the request rather than the outcome, matching confirmOffer: a
    // refused action replays and is refused again, which is the same session.
    void recordAction(std::string_view name,
                      std::vector<std::pair<std::string, double>> arguments = {});

    // Applies whatever a tool asked for, and tells the tool only if it landed.
    // Applies a tool's output through the journal. Returns whether the step
    // landed: applyStep is all-or-nothing, and a caller that records anything
    // about the placement has to know it happened.
    bool runTool(ToolOutput output, std::vector<ConstraintId> inferred = {});
    void refreshToolPresentation();

    // The value a resolved placement pins as a driving dimension.
    struct Imposition {
        size_t target = 0;
        double value = 0.0;
    };

    // Commits the placement in flight at `placement`, declaring `declaring`
    // against whatever the tool created there and, when asked, the dimension
    // that pins a typed value.
    //
    // The one path a placement commits through. A pointer press and a typed
    // Enter are two entrances to the same edit, and the way that stays true is
    // that they are one function with two callers rather than two functions
    // that have to be kept in agreement.
    //
    // Returns false when the tool made nothing — the click that opens a shape
    // rather than closing one — leaving the caller to decide what that means.
    bool commitPlacement(Point placement, const std::vector<SnapCandidate> &declaring,
                         const std::optional<Imposition> &impose);

    // The recording surfaces record; these do the work. handle(Key) already
    // recorded the keystroke, so dispatching to the public methods would write
    // the step twice and replay would do it twice — record → replay → record
    // is the identity the format exists to guarantee.
    void applyNumericResolve(bool impose);
    void applyNumericAdvance();
    void applyNumericCancel();
    // Runs inference for a placement at `cursor`. One call feeds both the ghost
    // and the commit, so a previewed candidate set that differs from the
    // committed one is not a reachable state.
    SnapResult inferAt(Point cursor) const;
    void rememberSnaps(const std::vector<SnapCandidate> &committed);

    // Recomputes the fill offers around `seed`, or clears them when it is null.
    // One place, because closed and healable are mutually exclusive answers to
    // one question and computing them apart is how they come to disagree.
    // before, after: the same component's parameter spans either side of a
    // solve. Accumulates the invisible operands implicated in moving something
    // visible, so one refresh over many components reports all of them.
    void noteHiddenInfluence(const std::vector<SeedSpan> &before,
                             const std::vector<SeedSpan> &after);

    void refreshLoopOffers(EntityId seed);

    // The same, seeded from whatever is selected. A no-op while a creation tool
    // is running: the placement in flight owns the offers, and recomputing them
    // off an empty selection would clear an offer the user is looking at.
    void refreshSelectionOffers();

    // Applies a candidate as a relation record, or as a one-shot solve for
    // Strength::Measure. Shared by every entrance to imposition.
    bool commitCandidate(const ConstraintRecord &candidate, Strength strength,
                         const ImpositionPreview &preview);

    // The strip, while a drag rather than a tool is what is in flight. The
    // fields are the measurements the drag is adjusting, so the numeric machine
    // above them is the one a creation tool uses and not a second one.
    void refreshDragPresentation();

    // Lands the drag on the typed value, and ends it.
    //
    // Enter finishes the gesture here for the same reason it does for a
    // placement: the digits exist because the hand could not hit the number, so
    // asking for a release afterwards would hand the result back to the hand.
    void applyDragResolve(bool impose);

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
    SurfacePolicy surfacePolicy_;
    Viewport viewport_;

    // The relations captured at clicks that created nothing yet, one set per
    // click, oldest first. They cannot be declared when they are captured — the
    // point each binds has no id until the shape that justifies it exists — so
    // they wait and are applied when the tool names what those clicks turned
    // out to create.
    //
    // A queue rather than one set, because a gesture can open more than one
    // click ahead: an arc's start and end are both placed before anything
    // commits, and a single slot would remember the second and lose the first.
    std::vector<std::vector<SnapCandidate>> pendingSnaps_;
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

    // What the fill offers were last computed around: the entity a placement
    // just created, or the first thing selected. Kept so an imposition or a
    // fill can recompute them without guessing what the user was looking at.
    EntityId loopSeed_;

    NumericEntry numeric_;

    ScriptRecorder *recorder_ = nullptr;
    // Null in the home state. Creation tools are shallow, so there is at most
    // one and it never nests.
    std::unique_ptr<Tool> tool_;

    std::optional<DragSession> drag_;
    // The measurements the drag in flight is adjusting, recomputed each frame
    // because a drag changes what they read. The numeric target indexes this.
    std::vector<DragDimension> dragDimensions_;
    // Named storage for the strip's field labels: ToolParameter holds a bare
    // pointer, so the strings it points at have to outlive the frame.
    std::vector<std::string> dragLabels_;
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
