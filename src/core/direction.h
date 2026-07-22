// Declared-direction classes over the document's segments.
//
// A direction class is a set of segments the document has declared to run the
// same way — the closure of the direction relations, not a measurement of the
// angles on screen. Two segments that merely look parallel count as two classes;
// that discrepancy is the feature, because it surfaces undeclared structure,
// which is exactly the thesis's diagnostic. The count reads the declarations.
//
// What closes a class, and only these, per the spec's segment-only rule:
//   - parallel: unions the two segments;
//   - horizontal with no reference: all such join one class (the document's
//     horizontal), because an unreferenced horizontal means the document frame;
//   - vertical with no reference: likewise, one class for the document vertical,
//     distinct from the horizontal one — perpendicular is a different direction;
//   - horizontal with a named reference: the segment is parallel to the
//     reference, so it joins the reference's class;
//   - vertical with a named reference: the segment is perpendicular to the
//     reference, so it joins the class of things perpendicular to that
//     reference — its own class, shared by every vertical about the same
//     reference, and deliberately not the reference's own.
//
// Perpendicular between two ordinary segments is not a direction identity and
// takes no part, exactly as it does not in the spec's list.
//
// This lives in core beside topology: it is a union-find over the constraint
// graph, deterministic in output order, and read by interact (the HUD count)
// and render (the extension overlay's class highlight), neither of which may
// include the other.
#pragma once

#include <unordered_map>
#include <vector>

#include "core/document.h"

namespace paroculus {

struct DirectionClasses {
    // Every segment's class index, dense and 0-based. Not iterated for output —
    // `members` carries the deterministic order — so a hash map is fine here.
    std::unordered_map<EntityId, int> classOf;
    // Members per class, each list in ascending id order, the classes themselves
    // ordered by their smallest member. This is the deterministic projection.
    std::vector<std::vector<EntityId>> members;

    size_t count() const { return members.size(); }
    // The class-mates of one segment, in id order, or empty when it is not a
    // segment the document holds. Includes the segment itself.
    std::vector<EntityId> classMembers(EntityId segment) const;
};

// The direction classes over every normal-or-construction segment in `doc`.
// Deterministic: constraints are walked in id order, segments in id order.
DirectionClasses directionClasses(const Document &doc);

}  // namespace paroculus
