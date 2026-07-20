// Composition: what is drawn, in what order, and whether it is still whole.
//
// Layers and groups are organization, not semantics — render order, visibility,
// lock state, drag-together defaults — and constraints cross them freely. Two
// couplings are real and both are asked here rather than at each consumer: a
// locked layer's geometry enters solves pinned, and a hidden layer's geometry
// still constrains.
//
// This lives in core because render draws the composition, hit testing picks
// through it, and solve pins from it, and none of those layers may include
// another. The same reason glyph placement is here: a rule computed twice is a
// rule that comes apart.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/document.h"

namespace paroculus {

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

// A null LayerId is the implicit base layer: visible, unlocked, and ordered
// below everything a user has made. Every document has one without anybody
// creating it, which is what lets geometry exist before layers do.
bool layerVisible(const Document &doc, LayerId layer);
bool layerLocked(const Document &doc, LayerId layer);

bool isVisible(const Document &doc, EntityId id);

// Whether this entity's own parameters are locked. Asked of the entity, not of
// what defines it: a segment owns nothing, so this is false for one however
// frozen its endpoints are.
bool isLocked(const Document &doc, EntityId id);

// Whether every parameter this entity's pose depends on is locked — its own,
// and those of the points defining it.
//
// The question a solve asks, and it is not the same as isLocked. A relation
// between two frozen segments has no unknowns to solve for, so including it
// would hand the solver equations and nothing to satisfy them with, and the
// verdict would be Inconsistent. A lock is presentation state and must never be
// able to make a system inconsistent; it removes unknowns, and a constraint left
// with none has to go with them.
bool isFrozen(const Document &doc, EntityId id);

// Every layer the document draws through, back to front. The implicit base
// layer leads with a null ID; the rest follow by (order, id), so the ordering is
// total and two layers sharing an order still stack in creation order rather
// than in whatever order a sort happened to leave them.
std::vector<LayerId> layerOrder(const Document &doc);

// ---------------------------------------------------------------------------
// Region algebra
// ---------------------------------------------------------------------------

// A region drawn in its own right: one no composite has taken as an operand.
// An operand is drawn as part of its composite and not beside it, which is what
// makes a boolean look like a boolean without consuming anything.
bool isTopLevelRegion(const Document &doc, RegionId id);

// The top-level regions of one layer, back to front, by (z, id).
std::vector<RegionId> regionOrder(const Document &doc, LayerId layer);

// The boundary of an outline region as an ordered, closed ring of point IDs —
// one per edge, each the end that edge is entered by — or nullopt when the
// stored boundary no longer walks closed.
//
// Here rather than in render because the renderer and the degradation query must
// agree on what closed means, exactly. A fill drawn from a ring the state query
// calls broken — or withheld from one it calls whole — is the two-answers bug
// this shares with glyph placement.
//
// Closure is topological: two endpoints are the same joint when a coincidence
// says so, never when they merely sit at the same coordinates. So this asks the
// document and takes no pose, and a region stays whole through a solve that has
// not converged rather than flickering broken while the geometry catches up.
//
// Three edges minimum: two segments over one pair of vertices pass the degree
// test and walk closed, and the 2-gon they report encloses nothing. The bound
// lifts when arcs become boundary-capable, since two curved edges do enclose a
// lens.
std::optional<std::vector<EntityId>> boundaryRing(const Document &doc,
                                                  const RegionRecord &region);

// ---------------------------------------------------------------------------
// Degradation
// ---------------------------------------------------------------------------

// Broken means the record is still here and still says what it meant, but no
// longer has the parts to mean it. Deleting an edge of a filled loop lands the
// region here: it renders as a diagnostic rather than dissolving, the deletion
// is not blocked, and undo restores it in one step because the shrink was a
// whole-record set.
enum class RegionState : uint8_t { Whole, Broken };

RegionState regionState(const Document &doc, RegionId id);
RegionState regionState(const Document &doc, const RegionRecord &region);

// Every broken region in the document, in ID order.
std::vector<RegionId> brokenRegions(const Document &doc);

enum class TagState : uint8_t { Whole, Broken };

// A tag is broken when it no longer names enough to mean what its kind means: a
// rectangle missing an edge, a distribution over two things. It owns nothing
// either way, so a broken tag costs the user only the affordances it was
// offering — every primitive and every surviving constraint is untouched.
TagState tagState(const Document &doc, const TagRecord &tag);

std::vector<TagId> brokenTags(const Document &doc);

}  // namespace paroculus
