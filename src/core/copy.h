// Copy takes internal structure.
//
// A copy is a kind-preserving bijection: every record in the copied set appears
// once in the result, with the same kind and the same shape, on fresh IDs that
// collide with nothing. What travels is what is internal — a relation with both
// operands inside the set comes along, because it says something about the set
// and stays true of the copy. What does not travel is what straddles the
// boundary — a relation with one operand outside says something about the copied
// geometry's place in the drawing, and a copy is somewhere else.
//
// Boundary relations are dropped and the drop is counted, never silently
// discarded and never blocking. That count is the whole of what the surface has
// to say, and it is the same discipline deletion follows: report what went, do
// not ask permission for it.
//
// Duplicate-with-offset is this plus a translation, deliberately rather than as
// a separate system: an array or a pattern is duplicate-with-offset applied more
// than once, so the feature that has not been built yet already has its
// mechanism.
#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

#include "core/document.h"
#include "core/geom.h"

namespace paroculus {

// Where the images land.
//
// A map rather than an offset because mirror is a copy too, and a mirror is the
// same bijection over the same records placed by a reflection instead of a
// translation. Two functions that agreed about everything except one line is how
// the copy semantics would come to have two versions.
struct CopyPlacement {
    std::function<Point(Point)> place;

    // Whether `place` reverses orientation. An arc records its sweep
    // counter-clockwise from start to end, so reflecting its three points
    // without swapping the endpoints yields the complementary arc — the one
    // going the long way round — rather than the mirror image on screen. The
    // flag is the caller's, because only the caller knows the determinant of the
    // map it supplied.
    bool reversesOrientation = false;
};

// What a copy produced, and what it could not bring.
//
// The maps are the bijection made inspectable, which is what lets the copy
// isomorphism property be a test rather than an eyeball: every original has
// exactly one image, images are disjoint from originals, and kinds correspond.
struct CopyStep {
    std::vector<Command> commands;

    std::unordered_map<EntityId, EntityId> entities;
    std::unordered_map<ConstraintId, ConstraintId> constraints;
    std::unordered_map<RegionId, RegionId> regions;
    std::unordered_map<TagId, TagId> tags;

    // Relations that named one operand inside the copied set and one outside.
    // They stayed with the original and the copy is free of them.
    size_t droppedConstraints = 0;
    // Regions and tags the copy could not carry whole, for the same reason.
    size_t droppedRegions = 0;
    size_t droppedTags = 0;

    // The images, in ID order. What a caller selects afterwards, so a duplicate
    // leaves the new geometry selected and a second duplicate offsets from it.
    std::vector<EntityId> copiedEntities() const;

    bool empty() const { return commands.empty(); }
};

// Copies `selection`, closed downward over defining points, offset by (dx, dy).
//
// Closed downward for the same reason a transform is: a segment is entirely its
// endpoints, so copying one without them would produce a record naming geometry
// it does not own. Closed downward and not upward: copying a point does not copy
// every segment that happens to end there.
//
// Regions and tags come along when everything they name is inside — a fill whose
// outline is half-copied has no area to bound, and a tag whose rectangle is half
// there is a tag that would be born broken. Both are dropped rather than carried
// degraded, because a degraded record is what an *edit* leaves behind, and
// nothing has been edited here.
//
// Groups are not copied. A group is a drag-together default the user set up over
// particular geometry; the copy is different geometry and the user has not said
// anything about it.
//
// A frame-referenced relation — symmetric-horizontal and vertical, which mean
// the world origin through no operand — is dropped and counted on every copy,
// because the world frame is outside every copied set: an offset copy that kept
// it would slide back toward the world axis. An orientation-reversing placement
// (a mirror) drops two more things a translation does not. A null-reference
// horizontal or vertical means the document frame, which reflection does not
// preserve, so it drops and counts like a straddler; and a tangency's arc had
// its endpoints swapped, so its alternative flips to keep naming the same
// physical end. Both keep the copy honest about what a reflection changed. A
// future mirror could instead retarget those axis relations to a reflected
// frame, exactly as rotate does — dropping is the answer that rewrites nothing.
CopyStep copyStep(const Document &doc, std::span<const EntityId> selection,
                  const CopyPlacement &placement);

// Duplicate-with-offset: the common case, spelled without a lambda.
CopyStep copyStep(const Document &doc, std::span<const EntityId> selection, double dx,
                  double dy);

}  // namespace paroculus
