// Drag is a solve.
//
// A drag does not set a point to (x, y); it asks the solver for the nearest
// legal state in which that point sits at (x, y). The dragged point's
// parameters go into the solver's dragged set, it favours solutions keeping
// them near the cursor, and everything else moves as little as the constraints
// allow.
//
// Four feel rules here are commitments, not tuning:
//
//   release commits    the solved state at mouse-up becomes the new seeds.
//                      Nothing springs back.
//   saturation         an unreachable target does not fail. Geometry rides the
//                      feasible boundary while the cursor overshoots, and the
//                      constraints doing the resisting light up. Resistance
//                      with attribution is how users discover constraints by
//                      feel; refusal teaches nothing.
//   locality           the solve is scoped to the dragged component, so distant
//                      geometry cannot move at all.
//   no silent ripple   if the solve moved something outside the viewport, the
//                      edge of the screen says so.
#pragma once

#include <optional>
#include <vector>

#include "core/pose.h"
#include "core/topology.h"
#include "interact/policies.h"
#include "solve/solve.h"

namespace paroculus {

// The transform in force plus the pixel rectangle it maps onto. The two always
// travel together for anything that asks "can the user see this?", and ripple
// detection is exactly that question.
struct Viewport {
    ViewTransform view;
    double width = 0.0;
    double height = 0.0;

    // p: document coordinates. Returns whether it lands on screen.
    bool contains(Point p) const {
        const Eigen::Vector2d s = view.toScreen(p);
        return s.x() >= 0.0 && s.y() >= 0.0 && s.x() <= width && s.y() <= height;
    }
};

struct DragUpdate {
    SolveStatus status = SolveStatus::Unsolved;

    // How far the geometry ended up from the cursor, in pixels. Zero while the
    // drag is tracking; grows once the constraints stop it.
    double gap = 0.0;
    bool saturated = false;

    // The constraints resisting, when saturated. Diagnosed by a speculative
    // hard-pin solve, which costs a second solve and is therefore throttled.
    std::vector<ConstraintId> resisting;

    // Set when the solve moved geometry outside the viewport. Off-screen
    // consequence without indication is how parametric tools lose trust.
    bool rippledOffScreen = false;

    double microseconds = 0.0;
};

// A measurement a drag in flight is adjusting.
//
// "The length under adjustment" is exactly as ambiguous as it sounds the moment
// a vertex belongs to more than one segment, and prose cannot resolve it — the
// surface has to. So a drag offers every measurement it is moving, the strip
// names them, and Tab picks between them. That is the same disambiguation the
// action surface does for role ambiguity, applied to the one place PRINCIPLES
// left it implicit when it said "the length under adjustment becomes exactly
// 45".
//
// Nothing here is a constraint yet. A resolved drag lands the geometry on the
// number and records nothing; imposing it as a driving dimension is the one
// extra key, in the same undo step as the motion it measures.
struct DragDimension {
    ConstraintKind kind = ConstraintKind::PointPointDistance;
    std::array<EntityId, MAX_OPERANDS> operands{};
    size_t count = 0;
    // What it reads at the current pose, in document units. Live: a drag
    // changes it every frame, and the strip shows what it is now rather than
    // what it was when the drag began.
    double value = 0.0;
    // The entity the strip names, so two lengths off one vertex are told apart
    // by what they are lengths of.
    EntityId subject;

    // The record that would hold it at `at`, for the solve and for the
    // dimension an imposing resolve records.
    ConstraintRecord recordAt(double at) const;
};

// The measurements dragging `grabbed` adjusts, in ID order.
//
// For a point: the length of every segment it is an end of. For a circle: its
// radius, since a circle owns one parameter and grabbing it is a resize.
//
// ID-ordered because the strip's field order is what Tab cycles through, and an
// order that changed between frames would move the field under the user
// mid-entry.
std::vector<DragDimension> dragDimensions(const Document &doc, const Pose &pose,
                                          EntityId grabbed);

// One drag, from press to release.
//
// The document is not touched until commit(): every intermediate pose lives in
// the session's own context, which is what makes an abandoned drag free and a
// committed one a single ordinary undo step.
class DragSession {
public:
    // Begins dragging `grabbed`, scoping the solve to the components the drag
    // touches. Returns nullopt when the grabbed entity cannot be dragged — it is
    // not live, or it owns no parameters of its own, as a segment does not.
    //
    // selection: what else the user is holding. Every param-owning member of it
    // that lands in this component joins the solver's dragged set, which is what
    // "multi-selection drags put all selected parameters in the set" means.
    //
    // Held is not targeted. Only the grab is asked to be at the cursor; the rest
    // are marked so the solver favours leaving them where they are, which pushes
    // what has to give into the geometry the user did not select. Anything
    // selected outside this component is not in this solve — locality outranks
    // it, and nothing connects the two.
    static std::optional<DragSession> begin(const Document &doc, const Topology &topology,
                                            EntityId grabbed,
                                            const std::vector<EntityId> &selection,
                                            const HitPolicy &policy);

    // Moves the target to `cursor` and re-solves, warm-started from the last
    // pose. Cheap by design: this runs once per frame.
    //
    // diagnoseResistance: run the extra hard-pin solve that names the resisting
    // constraints. The caller throttles this — it roughly doubles the cost, and
    // the answer only changes when the user pushes further.
    DragUpdate update(const Document &doc, const Viewport &viewport, Point cursor,
                      bool diagnoseResistance);

    // The in-flight pose, for rendering and hit testing mid-gesture.
    const SolveContext &context() const { return context_; }
    EntityId grabbed() const { return grabbed_; }
    // What the solver is told the user is holding, grab first.
    const std::vector<EntityId> &dragged() const { return dragged_; }

    // Re-solves with `dimension` held exactly, instead of with the cursor.
    //
    // The numeric twin of a drag, and the reason it is one solve rather than a
    // second mechanism: the digits supply the precision the hand could not, so
    // the target stops being the pointer and becomes the value. The dragged set
    // is unchanged, which is what keeps the geometry that gives the geometry
    // the user was not holding.
    //
    // Returns whether it held. A value the constraints cannot satisfy leaves
    // the pose exactly as it was, because a drag that cannot reach a number is
    // saturation and not a licence to move somewhere else.
    bool resolve(const Document &doc, const ConstraintRecord &dimension);

    // The commands that make the current pose permanent. Empty when nothing
    // moved, so an abandoned or no-op drag journals nothing.
    std::vector<Command> commit(const Document &doc) const;

private:
    SolveContext context_;
    EntityId grabbed_;
    // Everything whose parameters the solver is told the user is holding: the
    // grab, plus whatever else the selection contributes to this component.
    std::vector<EntityId> dragged_;
    HitPolicy policy_;
    uint64_t generation_ = 0;
};

}  // namespace paroculus
