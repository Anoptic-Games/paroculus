// The selection model.
//
// Selection is the home state and Esc always lands there. There is no edit-mode
// wall: object versus component is selection *depth*, not mode. Double-click
// descends from a shape to its edges and points along the coincidence graph,
// Esc ascends, and mixed-depth selections — this rectangle plus that one vertex
// — are legal and produce honest signatures.
//
// Constraints are selectable through their glyphs by the same machinery, which
// is what makes a conflict set walkable. Stage 3 carries the entity side; the
// constraint side arrives with the glyphs that surface it in stage 5.
#pragma once

#include <string>
#include <vector>

#include "core/topology.h"

namespace paroculus {

// The typed multiset of the current selection: the key for contextual UI.
//
// Canonical form is the sorted kinds. Sorted because the surface asks "what can
// apply to {segment, segment}" — a question about what is selected, not about
// the order it was clicked in. Operand roles are assigned afterwards, in the
// surface, where a length-ratio can ask which way round and preview the answer.
struct Signature {
    std::vector<EntityKind> kinds;

    bool empty() const { return kinds.empty(); }
    size_t size() const { return kinds.size(); }

    // Stable, human-readable, and used in tests and the action registry:
    // "{point, segment}".
    std::string describe() const;

    friend bool operator==(const Signature &a, const Signature &b) {
        return a.kinds == b.kinds;
    }
    friend bool operator!=(const Signature &a, const Signature &b) { return !(a == b); }
};

class Selection {
public:
    // All mutators keep the contents ID-ordered and free of duplicates, so two
    // selections built by different routes compare and serialize alike.
    void clear();
    void set(EntityId id);
    void set(std::vector<EntityId> ids);
    void add(EntityId id);
    void remove(EntityId id);
    // Additive click: in if out, out if in. What Ctrl-click does.
    void toggle(EntityId id);

    bool contains(EntityId id) const;
    bool empty() const { return items_.empty(); }
    size_t size() const { return items_.size(); }
    const std::vector<EntityId> &items() const { return items_; }

    // How deep the last descent went. Zero is the home state.
    int depth() const { return depth_; }

    // Descends one level: from whole connected shapes to the individual
    // entities that make them up, following the coincidence graph so a run of
    // joined segments descends as one thing rather than shattering.
    // Returns false when there is nothing further to descend into.
    bool descend(const Document &doc, const Topology &topology);

    // Ascends one level, back toward whole shapes. Esc.
    // Returns false when already at the home state, which is the caller's cue
    // that Esc should clear the selection instead.
    bool ascend(const Document &doc, const Topology &topology);

    Signature signature(const Document &doc) const;

private:
    void normalise();

    std::vector<EntityId> items_;
    int depth_ = 0;
};

// Everything reachable from `seed` through shared geometry: the entities that
// define it, the entities defined by it, and anything joined to those by a
// coincidence. This is what a single click selects at depth zero, and what
// ascending returns to.
std::vector<EntityId> connectedRun(const Document &doc, const Topology &topology, EntityId seed);

}  // namespace paroculus
