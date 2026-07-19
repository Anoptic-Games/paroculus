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

}  // namespace paroculus
