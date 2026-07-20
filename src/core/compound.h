// Compound relations are macro expansions.
//
// Distribute-evenly and mirror are not constraint kinds. They emit ordinary
// primitives — the ones already in the catalogue, over the geometry already
// there — plus a tag that lets the set be edited as one thing for as long as the
// primitives hold. The solver never sees anything but primitives, which is why
// adding a compound costs the solve layer nothing and why a compound cannot
// introduce a semantics the semantics suite does not already cover.
//
// The tag owns nothing. Break one defining relation and the tag dissolves,
// leaving every primitive and every surviving constraint exactly where it was —
// there is no convert-to-path cliff because there is nothing to convert. That is
// the same bargain the rectangle tool makes, and it is deliberately the same
// mechanism rather than a parallel one.
//
// Expansion is deterministic and total: the same selection produces the same
// primitive set every time, in the same order, so a compound's expansion can be
// compared against a hand-built set record for record.
#pragma once

#include <span>
#include <vector>

#include "core/document.h"

namespace paroculus {

enum class CompoundError : uint8_t {
    None,
    TooFew,       // fewer operands than the compound needs to mean anything
    NoAxis,       // mirror with no segment to reflect about
    WrongKinds,   // something in the selection the compound cannot act on
    AxisInside,   // the mirror axis is part of what would be mirrored
    Degenerate,   // an axis of zero length, which names no line
};

const char *compoundErrorName(CompoundError e);

struct CompoundStep {
    std::vector<Command> commands;
    CompoundError error = CompoundError::None;

    // The primitives the expansion added, so a caller can select them, and so a
    // test can compare the expansion against a hand-built set.
    std::vector<EntityId> entities;
    std::vector<ConstraintId> constraints;
    TagId tag;

    bool ok() const { return error == CompoundError::None; }
};

// Distributes the selected points evenly along the line through the first and
// the last, in ID order.
//
// The expansion, for n points p0..p(n-1):
//
//   one construction segment spanning p0 to p(n-1)
//   n-1 construction segments, one across each consecutive gap
//   point-on-line, holding each interior point to the span
//   equal-length, between each consecutive pair of gaps
//
// Even spacing is equal gaps plus collinearity, and both halves are needed: gaps
// alone let the run zig-zag with equal strides, and collinearity alone puts the
// points on a line at any spacing. Neither on its own is what the word means.
//
// The gap segments are construction geometry rather than a hidden mechanism.
// They constrain normally, they never attract a snap, they stay out of regions
// and export, and they are visible and selectable — a user who wants to know
// what a distribution is holding can look at it, and one who wants to be rid of
// it can delete a segment.
//
// The points are ordered along the run they form, not by ID and not by the order
// they were clicked in: a distribution is a statement about the arrangement on
// screen, so the expansion has to read that arrangement. Ties break by ID, so
// the same selection expands the same way every time.
CompoundStep distributeStep(const Document &doc, std::span<const EntityId> selection);

// Mirrors the selected geometry about the selected segment.
//
// The expansion is a copy placed by the reflection, plus one symmetric-about-line
// relation per copied point holding it to its original. So the mirror is live:
// dragging an original drags its image, and deleting the relations leaves two
// independent pieces of ordinary geometry rather than a broken construct.
//
// The axis must be selected and must not be part of what is mirrored — a segment
// reflected about itself is itself, and the symmetric relations that would say so
// are relations between a point and its own image.
CompoundStep mirrorStep(const Document &doc, std::span<const EntityId> selection,
                        EntityId axis);

// The segment a mirror would reflect about.
//
// The single construction segment in the selection, since an axis is
// construction geometry — that is what lets a bar be mirrored about a guide when
// both are segments and nothing else could tell them apart. Failing that, the
// single ordinary segment, which is how points are mirrored about an edge. Null
// when the selection offers neither, or offers two of the same kind.
//
// Here rather than at the call site because the action surface asks it to decide
// whether mirror applies and the expansion asks it to do the work, and an
// applicability predicate that disagreed with the operation is an action a
// surface offers and the model refuses.
EntityId mirrorAxisIn(const Document &doc, std::span<const EntityId> selection);

}  // namespace paroculus
