// Inference: snaps are proposed constraints.
//
// The bridge between freehand drawing and the parametric layer, and the single
// place this project most easily could have gone wrong. A snap here is not a
// coordinate correction that happens to look tidy — it is a constraint
// candidate that placement commits. Snap-to-endpoint commits coincidence;
// snap-to-horizontal commits horizontal. Building this as a geometric corrector
// and bolting inference on afterwards would produce exactly the freehand-then-
// convert tool this project exists to refuse.
//
// So the engine is a projection of the taxonomy, not a second list. Every kind
// it can emit is a row in SNAP_KINDS, and that row already names the constraint
// it commits and the tier it commits at. Adding a snap kind is adding a row.
//
// Two disciplines the rest of the design leans on:
//
//   preview shows truth   the same call produces the ghost and the commit, so
//                         a previewed candidate set that differs from the
//                         committed one is not a possible bug.
//   grid never declares   grid is placement-only. A document where every point
//                         is grid-pinned is rigidity by helpfulness.
#pragma once

#include <optional>
#include <vector>

#include "core/document.h"
#include "core/pose.h"
#include "interact/hit.h"
#include "interact/placement.h"
#include "interact/policies.h"

namespace paroculus {

// What the placement is creating, so a candidate can name it before it has an
// id. The tool declares which of the entities it claimed play these roles.
enum class SnapSubject : uint8_t { PlacedPoint, PlacedSegment };

// One relation the placement would declare.
struct SnapCandidate {
    SnapKind kind = SnapKind::Grid;
    SnapSubject subject = SnapSubject::PlacedPoint;
    // The existing entity the relation binds to. Null for kinds that constrain
    // the placement alone — horizontal and vertical are properties of the new
    // segment and reference nothing else.
    EntityId target;
    // Where this candidate would put the point. Kept per candidate rather than
    // only on the result, because ranking has to compare corrections and
    // because a rejected candidate must not silently move anything.
    Point placement;
    double correction = 0.0;  // pixels the placement moved
    double score = 0.0;

    // The user confirmed this offer for the placement in flight. An offered
    // kind that has been confirmed commits exactly like an auto-committing one
    // — confirmation is the user supplying the confidence the tier withheld.
    bool confirmed = false;

    SnapTier tier() const { return snapInfo(kind).tier; }
    bool autoCommits() const { return confirmed || tier() == SnapTier::AutoCommit; }
};

// The pointer state a snap is computed against.
struct SnapRequest {
    Point cursor;  // raw, in document units

    // The in-flight segment's anchor, when there is one. Direction-valued kinds
    // — horizontal, vertical, parallel, perpendicular — are properties of a
    // segment, so without an anchor there is no segment for them to describe
    // and they are not generated at all.
    bool haveAnchor = false;
    Point anchor;
    EntityId anchorEntity;

    // Kinds committed recently in this document, most recent first. The whole
    // of "ranking is contextual and document-local".
    std::vector<SnapKind> recent;

    // Offers the user has confirmed for the placement in flight, as
    // (kind, target). A confirmation holds only while the relation is still
    // available: swinging far enough that the candidate is no longer generated
    // lapses it, because a parallel to a segment you have rotated away from is
    // not a relation anyone can declare.
    std::vector<std::pair<SnapKind, EntityId>> confirmed;
};

struct SnapResult {
    // Where the point actually goes: the best candidate's placement, or the
    // raw cursor when nothing captured it.
    Point placement;
    // Ranked, best first. Auto-committing kinds are not filtered out here —
    // the caller commits those and offers the rest, and a test can see both.
    std::vector<SnapCandidate> candidates;

    // The candidates that would be declared by a placement now.
    std::vector<SnapCandidate> autoCommitted() const;
    // The candidates that would be offered for one-key confirmation.
    std::vector<SnapCandidate> offered() const;
};

// Generates and ranks the candidates for a placement at `request.cursor`.
//
// pose: the geometry currently on screen. index: over that same pose.
// view: the transform in force, used to convert the pixel tolerances — snap
//   radii are pixel quantities because they describe aim, and a snap that got
//   easier as you zoomed out would be a snap that ignored what you meant.
SnapResult snap(const Document &doc, const Pose &pose, const SpatialIndex &index,
                const ViewTransform &view, const SnapRequest &request,
                const SnapPolicy &policy);

// Builds the constraint a candidate declares, once the placement has ids.
//
// placed: what the tool created, by role. Returns nullopt for the
// placement-only kinds, which declare nothing by design, and for a candidate
// whose subject the placement did not create in any form it can be said about.
//
// A candidate says two things coincide at a position. Which constraint says it
// depends on what the placement put there: a point gives coincidence, and a
// curve passing through with no point of its own gives point-on-curve. That is
// one geometric claim with two spellings, not two rules — see the comment on
// the implementation before adding a third.
std::optional<ConstraintRecord> constraintFor(const SnapCandidate &candidate,
                                              const PlacementSubjects &placed);

// Whether a placement filling exactly these roles would declare this candidate.
//
// The ghost's question, asked a click before the ids exist. It is answered by
// running constraintFor itself against stand-in ids, so a previewed relation
// and a committed one cannot disagree — that is what "preview shows truth"
// means mechanically, and it is why this is not a second predicate.
std::optional<ConstraintKind> declaredKind(const SnapCandidate &candidate, PlacementRoles roles);

}  // namespace paroculus
