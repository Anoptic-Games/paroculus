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
#include <span>
#include <vector>

#include "core/document.h"
#include "core/geom.h"
#include "core/pose.h"

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

// One edge of a boundary, in walk order, with the joints it is traversed
// between.
//
// The traversal and not just the corner, because an arc has to be drawn along
// its sweep and which way round that runs is a property of the walk rather than
// of the record: the same arc bounds two different regions depending on which
// joint the ring enters it by. A ring of bare points was enough while every edge
// was straight and lost exactly that.
struct BoundaryStep {
    EntityId edge;
    // The joint this edge is entered by and the one it leaves at. Both null for
    // a self-closing edge, which meets no neighbour.
    EntityId from;
    EntityId to;
    // Whether the ring traverses this edge against its record's own direction.
    //
    // Recorded by the walk rather than re-derived, because the walk is the only
    // place that has already done the coincidence matching. Working it out again
    // in render and again in the bake is how the two come to disagree about
    // which way an arc bulges — and the disagreement shows up as an exported
    // file that does not match the screen.
    bool reversed = false;
};

// The angular run a boundary step traverses, for a curved edge, at this pose.
//
// Signed sweep: negative when the ring enters the arc at the end its record
// calls last. A closed curve reports a full turn from angle zero, which is the
// only run it has.
//
// Nullopt for a straight edge, which is the caller's cue to draw a line to the
// joint the step leaves at. Here rather than in render because the bake needs
// exactly the same answer at a different sampling density, and a curve baked one
// way and drawn another is the same two-answers bug the ring walk exists to
// prevent.
struct CurveRun {
    Point centre;
    double radius = 0.0;
    double startAngle = 0.0;
    double sweep = 0.0;
};

std::optional<CurveRun> curveRunOf(const Pose &pose, const BoundaryStep &step);

// The boundary of an outline region as an ordered, closed ring of edges — or
// nullopt when the stored boundary no longer walks closed.
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
// Three edges minimum while every one of them is straight, two once a curve is
// involved, and one for a closed curve that bounds alone. enclosesArea is where
// that rule lives; this walk and topology's cycle test both read it, because a
// region the cycle test offers and the ring walk calls broken is the two-answers
// bug in its purest form.
std::optional<std::vector<BoundaryStep>> boundaryRing(const Document &doc,
                                                      const RegionRecord &region);

// Whether a selection names a region.
//
// A region has no handle of its own — it is reached through the geometry that
// bounds it, which is the same thing that makes a fill a view of an outline
// rather than an object beside one. So an outline is named when every edge it
// names is selected, and a composite when every operand is, recursively.
//
// Here rather than in interact because render asks it too, to decide whether a
// fill draws as selected, and every region action asks it to decide what to act
// on. Two implementations disagreed: the fill tinted when any one edge was
// selected while punch, raise, lower and subtract all wanted every edge, so a
// user could see a fill highlighted and watch every action on it refuse. Same
// discipline as the ring walk, and the same reason.
bool regionSelected(const Document &doc, const RegionRecord &region,
                    std::span<const EntityId> selection);

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
