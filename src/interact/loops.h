// Closed outlines.
//
// A drawn run that comes back to where it started is the flagship case: four
// segments, endpoint snapping having made the joints coincident as they were
// drawn, and a loop that closes. Noticing that is worth doing at the moment it
// happens, because that is when the user is thinking about it.
//
// Noticing is all this does. Making the loop a solid is a region record
// referencing the cycle, and that action lands in stage 5 — so what stage 4
// emits is an offer, not a fill. Detecting and acting are kept apart on purpose:
// an offer the user ignores must leave the document exactly as it was.
//
// Closure is topological, never visual. An outline whose endpoints are near but
// not coincident is open, and stays open. The gap between looks-closed and
// is-closed is bridged by imposing the missing coincidence — heal-and-fill,
// which is also stage 5 — never by a renderer guessing that two nearby points
// were meant to be one.
#pragma once

#include <optional>
#include <vector>

#include "core/document.h"
#include "core/pose.h"
#include "core/topology.h"

namespace paroculus {

// The closed boundary the run containing `seed` forms, in order, or nullopt
// when that run is not a single closed loop.
//
// Refuses an open run, a figure-eight, a disjoint pair of loops, and anything
// touching a vertex more than twice — which is findBoundaryCycle's contract and
// the reason this is a thin query over it rather than a second implementation.
std::optional<std::vector<EntityId>> closedBoundaryContaining(const Document &doc,
                                                              const Topology &topology,
                                                              EntityId seed);

// A joint that looks shut and is not.
struct LoopGap {
    EntityId a;
    EntityId b;
    // How far apart, in document units. This is the epsilon the user already
    // could not see, and the distance the geometry will move when it is shut —
    // so it is what the surface shows rather than a fact it keeps to itself.
    double distance = 0.0;

    friend bool operator==(const LoopGap &x, const LoopGap &y) {
        return x.a == y.a && x.b == y.b && x.distance == y.distance;
    }
};

// An outline that is visually closed and topologically open, plus what it would
// take to close it.
struct HealableLoop {
    // In order, as the boundary would read once healed.
    std::vector<EntityId> boundary;
    // The coincidences heal-and-fill imposes, ID-ordered. Never empty: a loop
    // needing no gaps shut is already closed and is closedBoundaryContaining's
    // answer, not this one's.
    std::vector<LoopGap> gaps;
    // The furthest any joint has to travel. The motion bound the action
    // promises and the number the surface reports.
    double widestGap = 0.0;
};

// The loop the run containing `seed` would form if its near-misses were shut.
//
// Closure is topological, never visual, and this function does not change that:
// it does not decide that two nearby points are one, it reports what imposing
// the missing coincidences would produce, and imposing them is a separate,
// offered, undoable act. The gap between looks-closed and is-closed is bridged
// by explicit constraint imposition or it is not bridged.
//
// tolerance: how near two endpoints must be to count as a near miss, in
//   document units. A screen-calibrated pixel radius converted by the caller,
//   because how close is close enough is a property of the hand and the zoom.
//
// Returns nullopt when the run is already closed — closedBoundaryContaining is
// the query for that and make-solid is the action — when no arrangement of near
// misses closes it, or when what it would close is not a simple cycle. Enclosed
// areas formed by crossing segments rather than by shared endpoints are the
// deferred case: they need explicit intersection points before a cycle exists,
// and reporting them as healable would promise a fill the model cannot attach.
std::optional<HealableLoop> healableLoopContaining(const Document &doc,
                                                   const Topology &topology, const Pose &pose,
                                                   EntityId seed, double tolerance);

// The commands that shut a healable loop's gaps, as one undo step.
//
// Coincidences and nothing else: heal-and-fill imposes the relation the user
// meant and the solver moves the geometry by the epsilon they already could not
// see. The region is attached separately, so healing without filling is a state
// the model can hold and an undo can land on.
std::vector<Command> healingStep(const Document &doc, const HealableLoop &loop);

}  // namespace paroculus
