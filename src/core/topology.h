// One graph, four consumers.
//
// The coincidence graph underlies regions, heal-and-fill, selection expansion
// and the component partition. Keeping it as one structure rather than four
// recomputations is what makes drag locality cheap enough to be a per-frame
// property.
//
// Rebuilt rather than maintained in place. PRINCIPLES has the partition
// maintained incrementally as the graph edits, and that is still where this
// goes; it is not there yet because incremental maintenance means every site
// that mutates the document has to declare what it did to the graph, and the
// failure mode of forgetting is a silently wrong partition — wrong solve
// scoping, wrong selection runs, no assertion tripped. Rebuild-always is
// correct by construction, the profile does not yet object, and the union-find
// underneath is unchanged, so reinstating the incremental path is a small
// change made with measurements in hand rather than a guess.
//
// Two partitions live here and they are not the same:
//
//   components  — connectivity over the whole constraint graph, plus the
//                 ownership edges that bind a segment to its endpoints. This is
//                 the unit a solve is scoped to, and the reason a drag cannot
//                 move geometry it is not connected to.
//   coincidence — connectivity over Coincident constraints alone. This is what
//                 "the same vertex" means topologically, and it is what decides
//                 whether an outline is closed.
//
// Closure is topological, not visual: a loop is a cycle in the coincidence
// graph. Endpoints that merely look close are an open outline, which is why
// heal-and-fill has to impose the missing coincidences rather than paper over
// the gap with a fill that has geometry of its own.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "core/document.h"

namespace paroculus {

// Dense, assigned at rebuild, stable only until the next rebuild. Persistent
// identity belongs to IDs; this is an index.
using ComponentId = uint32_t;
inline constexpr ComponentId NO_COMPONENT = UINT32_MAX;

class Topology {
public:
    // The document must outlive the Topology and is the single source of truth;
    // this is a derived index over it and never mutates it.
    explicit Topology(const Document &doc) : doc_(&doc) {}

    // Marks the partition stale. The next query rebuilds it, so a caller only
    // has to say that something changed and never what.
    void noteRemoved() { dirty_ = true; }
    void markDirty() { dirty_ = true; }

    // Queries. Each rebuilds first if a removal has been noted since the last
    // one, so a caller can never observe a stale partition.
    ComponentId componentOf(EntityId id) const;
    size_t componentCount() const;
    std::vector<EntityId> membersOf(ComponentId component) const;

    // True when two entities would solve together. The drag-locality invariant
    // is stated against this: parameters outside the dragged entity's component
    // must come back bit-unchanged.
    bool sameComponent(EntityId a, EntityId b) const;

    // Every point sharing a vertex with this one, including itself, in ID
    // order. Selection depth descent walks these runs.
    std::vector<EntityId> coincidentRun(EntityId point) const;
    bool coincident(EntityId a, EntityId b) const;

private:
    void rebuild() const;
    void renumber() const;
    void ensureFresh() const {
        if(dirty_) rebuild();
    }

    // Returns the dense index for an entity, or NO_COMPONENT if unknown.
    uint32_t indexOf(EntityId id) const;
    uint32_t findRoot(std::vector<uint32_t> &parent, uint32_t i) const;
    void unite(std::vector<uint32_t> &parent, uint32_t a, uint32_t b) const;

    const Document *doc_;

    // Rebuilt wholesale; mutable because queries are logically const and the
    // laziness is an implementation detail.
    mutable bool dirty_ = true;
    mutable std::vector<EntityId> order_;                  // dense index -> id
    mutable std::unordered_map<EntityId, uint32_t> index_;  // id -> dense index
    mutable std::vector<uint32_t> componentParent_;
    mutable std::vector<uint32_t> coincidenceParent_;
    mutable std::vector<ComponentId> componentOfIndex_;
    mutable size_t componentCount_ = 0;
};

// Whether a set of boundary-capable edges forms one closed cycle through
// coincident endpoints, and in what order.
//
// Returns the ordered boundary on success. Returns nullopt when the edges do
// not form a single closed loop — an open run, a figure-eight, a disjoint pair
// of loops, anything touching a vertex more than twice, or fewer than three
// edges. That refusal is what make-solid consults, and it is why a
// visually-closed-but-topologically-open outline gets the heal-and-fill offer
// instead of a fill.
//
// How many edges are enough is enclosesArea's question: three while every edge
// is straight, because two segments over one pair of endpoints walk closed while
// enclosing nothing, and two once a curve is involved. Self-closing edges — a
// circle — take no part here at all: they have no joints to match, so they bound
// alone and closedBoundaryContaining answers for them from the seed.
//
// Areas enclosed by crossing segments rather than shared endpoints are a known
// later case: they need explicit intersection points before a cycle exists.
std::optional<std::vector<EntityId>> findBoundaryCycle(const Document &doc,
                                                       const Topology &topology,
                                                       std::span<const EntityId> edges);

}  // namespace paroculus
