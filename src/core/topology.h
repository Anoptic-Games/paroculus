// One graph, four consumers.
//
// The coincidence graph underlies regions, heal-and-fill, selection expansion
// and the component partition. Keeping it as one incrementally maintained
// structure rather than four recomputations is what makes drag locality cheap
// enough to be a per-frame property.
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

    // Incremental maintenance. Additions union in place; removals mark for
    // rebuild, because a union-find cannot split. That asymmetry is deliberate:
    // additions are the per-gesture common case and must stay cheap, while
    // removals are rare enough to pay for a rebuild at the next query.
    void noteAdded(EntityId id);
    void noteAdded(ConstraintId id);
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
// of loops, or anything touching a vertex more than twice. That refusal is what
// make-solid consults, and it is why a visually-closed-but-topologically-open
// outline gets the heal-and-fill offer instead of a fill.
//
// Areas enclosed by crossing segments rather than shared endpoints are a known
// later case: they need explicit intersection points before a cycle exists.
std::optional<std::vector<EntityId>> findBoundaryCycle(const Document &doc,
                                                       const Topology &topology,
                                                       std::span<const EntityId> edges);

}  // namespace paroculus
