#include "core/composition.h"

#include <algorithm>
#include <unordered_map>

namespace paroculus {

namespace {

// The two ends of a boundary edge, or nullopt if it has none. Only
// boundary-capable kinds reach here — validation refuses the rest — and every
// one of them is defined by two points.
std::optional<std::pair<EntityId, EntityId>> endsOf(const Document &doc, EntityId edge) {
    const EntityRecord *e = doc.entities().find(edge);
    if(e == nullptr || !entityInfo(e->kind).boundaryCapable) return std::nullopt;
    if(!e->points[0].valid() || !e->points[1].valid()) return std::nullopt;
    return std::pair<EntityId, EntityId>{e->points[0], e->points[1]};
}

// Coincidence classes over every point any Coincident constraint names.
//
// The whole graph and not the boundary's own endpoints, because restricting a
// transitive closure is not the closure of a restriction. Two boundary
// endpoints joined through a point outside the ring — a T-junction, which snap
// chaining produces whenever a click lands on a spur's endpoint rather than on
// a corner — are one joint to the coincidence partition and would be two to a
// union-find that only ever saw the ring. That is the disagreement between
// findBoundaryCycle and this walk that made a loop make-solid accepts render
// immediately as broken.
//
// A local union-find rather than the document's Topology, because this is a
// smaller question — Coincident alone, no ownership edges — and asking it this
// way keeps the ring walk usable from render, which may not include topology.h.
// It answers the same as Topology::coincident over the points it is asked
// about, and the coincidence corpus pins that.
class JointClasses {
public:
    explicit JointClasses(const Document &doc) {
        for(const ConstraintRecord &c : doc.constraints().records()) {
            if(c.kind != ConstraintKind::Coincident) continue;
            unite(intern(c.operands[0]), intern(c.operands[1]));
        }
    }

    // Whether two endpoints are the same joint. A point is trivially itself,
    // which is what makes a shared point and a coincidence-joined pair the same
    // thing to the walk — as they are to the user. A point no coincidence names
    // is in no class but its own.
    bool same(EntityId a, EntityId b) {
        if(a == b) return true;
        const std::optional<size_t> x = indexOf(a);
        const std::optional<size_t> y = indexOf(b);
        if(!x || !y) return false;
        return find(*x) == find(*y);
    }

private:
    std::optional<size_t> indexOf(EntityId id) const {
        const auto it = index_.find(id);
        if(it == index_.end()) return std::nullopt;
        return it->second;
    }

    // The dense slot for a point, creating one on first sight. Only lookups
    // read the map, so nothing here depends on its iteration order; which pairs
    // are united follows the constraint table's ID order.
    size_t intern(EntityId id) {
        const auto [it, fresh] = index_.emplace(id, parent_.size());
        if(fresh) parent_.push_back(it->second);
        return it->second;
    }

    size_t find(size_t i) {
        while(parent_[i] != i) {
            parent_[i] = parent_[parent_[i]];
            i = parent_[i];
        }
        return i;
    }

    void unite(size_t a, size_t b) { parent_[find(a)] = find(b); }

    std::unordered_map<EntityId, size_t> index_;
    std::vector<size_t> parent_;
};

constexpr int32_t BASE_LAYER_ORDER = 0;

// What a tag needs to still mean what its kind means. Minima rather than the
// counts a tool happened to emit: a rectangle is four edges and the eight
// relations that square them, a distribution is a rhythm over at least three
// things, a mirror is at least a pair. Below these the tag is offering
// affordances for a shape that is no longer there.
struct TagMinimum {
    TagKind kind;
    size_t entities;
    size_t constraints;
};

constexpr TagMinimum TAG_MINIMA[] = {
    {TagKind::Rectangle, 4, 8},
    {TagKind::Distribution, 3, 2},
    {TagKind::Mirror, 2, 1},
};

}  // namespace

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

bool layerVisible(const Document &doc, LayerId layer) {
    if(!layer.valid()) return true;
    const LayerRecord *l = doc.layers().find(layer);
    return l == nullptr || l->visible;
}

bool layerLocked(const Document &doc, LayerId layer) {
    if(!layer.valid()) return false;
    const LayerRecord *l = doc.layers().find(layer);
    return l != nullptr && l->locked;
}

bool isVisible(const Document &doc, EntityId id) {
    const EntityRecord *e = doc.entities().find(id);
    return e == nullptr || layerVisible(doc, e->layer);
}

bool isLocked(const Document &doc, EntityId id) {
    const EntityRecord *e = doc.entities().find(id);
    return e != nullptr && layerLocked(doc, e->layer);
}

bool isFrozen(const Document &doc, EntityId id) {
    const EntityRecord *e = doc.entities().find(id);
    if(e == nullptr) return false;
    const EntityKindInfo &info = entityInfo(e->kind);
    if(info.ownParamCount > 0 && !layerLocked(doc, e->layer)) return false;
    // Recursion is one level deep by construction: a defining point is a point,
    // and a point defines nothing.
    for(size_t i = 0; i < info.pointCount; i++) {
        if(!isFrozen(doc, e->points[i])) return false;
    }
    return true;
}

std::vector<LayerId> layerOrder(const Document &doc) {
    std::vector<const LayerRecord *> sorted;
    sorted.reserve(doc.layers().size());
    for(const LayerRecord &l : doc.layers().records()) sorted.push_back(&l);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const LayerRecord *a, const LayerRecord *b) {
                         if(a->order != b->order) return a->order < b->order;
                         return a->id < b->id;
                     });

    // The base layer sits at order zero among the named ones rather than below
    // all of them, so a layer authored below the geometry that predates it can
    // actually get there — which is what a signed order is for.
    const auto below = std::find_if(sorted.begin(), sorted.end(), [](const LayerRecord *l) {
        return l->order >= BASE_LAYER_ORDER;
    });
    std::vector<LayerId> out;
    out.reserve(sorted.size() + 1);
    for(auto it = sorted.begin(); it != below; ++it) out.push_back((*it)->id);
    out.push_back(LayerId());
    for(auto it = below; it != sorted.end(); ++it) out.push_back((*it)->id);
    return out;
}

// ---------------------------------------------------------------------------
// Region algebra
// ---------------------------------------------------------------------------

bool isTopLevelRegion(const Document &doc, RegionId id) {
    return compositesOver(doc, id).empty();
}

std::vector<RegionId> regionOrder(const Document &doc, LayerId layer) {
    std::vector<const RegionRecord *> sorted;
    for(const RegionRecord &r : doc.regions().records()) {
        if(r.layer != layer) continue;
        if(!isTopLevelRegion(doc, r.id)) continue;
        sorted.push_back(&r);
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const RegionRecord *a, const RegionRecord *b) {
                         if(a->z != b->z) return a->z < b->z;
                         return a->id < b->id;
                     });

    std::vector<RegionId> out;
    out.reserve(sorted.size());
    for(const RegionRecord *r : sorted) out.push_back(r->id);
    return out;
}

std::optional<std::vector<EntityId>> boundaryRing(const Document &doc,
                                                  const RegionRecord &region) {
    if(region.op != CompositeOp::Outline) return std::nullopt;
    if(region.boundary.size() < 3) return std::nullopt;

    std::vector<std::pair<EntityId, EntityId>> ends;
    ends.reserve(region.boundary.size());
    for(EntityId edge : region.boundary) {
        const std::optional<std::pair<EntityId, EntityId>> e = endsOf(doc, edge);
        if(!e) return std::nullopt;
        ends.push_back(*e);
    }

    JointClasses joints(doc);

    // Orient the first edge against the last, so the ring closes rather than
    // starting off in whichever direction the record happened to store.
    const std::pair<EntityId, EntityId> &last = ends.back();
    const bool forward =
        joints.same(ends.front().first, last.first) || joints.same(ends.front().first, last.second);

    std::vector<EntityId> ring;
    ring.reserve(ends.size());
    ring.push_back(forward ? ends.front().first : ends.front().second);
    EntityId cursor = forward ? ends.front().second : ends.front().first;

    for(size_t i = 1; i < ends.size(); i++) {
        ring.push_back(cursor);
        if(joints.same(ends[i].first, cursor)) {
            cursor = ends[i].second;
        } else if(joints.same(ends[i].second, cursor)) {
            cursor = ends[i].first;
        } else {
            return std::nullopt;  // the ring does not meet; nothing honest to fill
        }
    }
    // And the walk has to arrive back where it started, or this is an open run
    // whose ends merely happened to line up edge by edge.
    if(!joints.same(cursor, ring.front())) return std::nullopt;
    return ring;
}

// ---------------------------------------------------------------------------
// Degradation
// ---------------------------------------------------------------------------

RegionState regionState(const Document &doc, const RegionRecord &region) {
    if(region.op == CompositeOp::Outline) {
        return boundaryRing(doc, region) ? RegionState::Whole : RegionState::Broken;
    }
    // A composite is exactly as whole as what it combines. One operand is not a
    // combination of anything, and a broken operand has no area to contribute,
    // so a composite over one is broken however sound its own record looks.
    if(region.operands.size() < 2) return RegionState::Broken;
    for(RegionId o : region.operands) {
        if(regionState(doc, o) != RegionState::Whole) return RegionState::Broken;
    }
    return RegionState::Whole;
}

RegionState regionState(const Document &doc, RegionId id) {
    const RegionRecord *r = doc.regions().find(id);
    if(r == nullptr) return RegionState::Broken;
    return regionState(doc, *r);
}

std::vector<RegionId> brokenRegions(const Document &doc) {
    std::vector<RegionId> out;
    for(const RegionRecord &r : doc.regions().records()) {
        if(regionState(doc, r) != RegionState::Whole) out.push_back(r.id);
    }
    return out;
}

TagState tagState(const Document &doc, const TagRecord &tag) {
    for(EntityId e : tag.entities) {
        if(doc.entities().find(e) == nullptr) return TagState::Broken;
    }
    for(ConstraintId c : tag.constraints) {
        if(doc.constraints().find(c) == nullptr) return TagState::Broken;
    }
    for(const TagMinimum &m : TAG_MINIMA) {
        if(m.kind != tag.kind) continue;
        if(tag.entities.size() < m.entities || tag.constraints.size() < m.constraints) {
            return TagState::Broken;
        }
    }
    return TagState::Whole;
}

std::vector<TagId> brokenTags(const Document &doc) {
    std::vector<TagId> out;
    for(const TagRecord &t : doc.tags().records()) {
        if(tagState(doc, t) != TagState::Whole) out.push_back(t.id);
    }
    return out;
}

}  // namespace paroculus
