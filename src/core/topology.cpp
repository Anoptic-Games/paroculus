#include "core/topology.h"

#include <algorithm>

namespace paroculus {

uint32_t Topology::findRoot(std::vector<uint32_t> &parent, uint32_t i) const {
    while(parent[i] != i) {
        parent[i] = parent[parent[i]];  // halve the path as we go
        i = parent[i];
    }
    return i;
}

void Topology::unite(std::vector<uint32_t> &parent, uint32_t a, uint32_t b) const {
    const uint32_t ra = findRoot(parent, a);
    const uint32_t rb = findRoot(parent, b);
    if(ra == rb) return;
    // Union by index rather than rank: the lower index wins, so the partition
    // is a function of the document alone and not of the order edges arrived.
    // Determinism is a document property, and this is one of the places it is
    // cheap to keep and expensive to notice losing.
    if(ra < rb) {
        parent[rb] = ra;
    } else {
        parent[ra] = rb;
    }
}

uint32_t Topology::indexOf(EntityId id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? NO_COMPONENT : it->second;
}

void Topology::rebuild() const {
    order_.clear();
    index_.clear();
    componentOfIndex_.clear();

    const auto &entities = doc_->entities().records();
    order_.reserve(entities.size());
    for(const EntityRecord &e : entities) {
        index_.emplace(e.id, static_cast<uint32_t>(order_.size()));
        order_.push_back(e.id);
    }

    componentParent_.resize(order_.size());
    coincidenceParent_.resize(order_.size());
    for(uint32_t i = 0; i < order_.size(); i++) {
        componentParent_[i] = i;
        coincidenceParent_[i] = i;
    }

    // Ownership edges: a segment solves with its endpoints, always. Without
    // these an unconstrained segment would partition away from the very points
    // that define it.
    for(const EntityRecord &e : entities) {
        const uint32_t self = indexOf(e.id);
        for(size_t i = 0; i < entityInfo(e.kind).pointCount; i++) {
            const uint32_t p = indexOf(e.points[i]);
            if(p != NO_COMPONENT) unite(componentParent_, self, p);
        }
    }

    // Constraint edges: every operand of a constraint solves with every other.
    for(const ConstraintRecord &c : doc_->constraints().records()) {
        const size_t n = boundOperandCount(c);
        const uint32_t first = indexOf(c.operands[0]);
        if(first == NO_COMPONENT) continue;
        for(size_t i = 1; i < n; i++) {
            const uint32_t other = indexOf(c.operands[i]);
            if(other != NO_COMPONENT) unite(componentParent_, first, other);
        }
        // Coincidence alone defines "the same vertex".
        if(c.kind == ConstraintKind::Coincident) {
            const uint32_t second = indexOf(c.operands[1]);
            if(second != NO_COMPONENT) unite(coincidenceParent_, first, second);
        }
    }

    renumber();
    dirty_ = false;
}

// Numbers components in first-appearance order over the ID-ordered entity
// table, so component IDs are a function of the document and not of the order
// edges happened to arrive.
void Topology::renumber() const {
    componentOfIndex_.assign(order_.size(), NO_COMPONENT);
    std::unordered_map<uint32_t, ComponentId> rootToComponent;
    componentCount_ = 0;
    for(uint32_t i = 0; i < order_.size(); i++) {
        const uint32_t root = findRoot(componentParent_, i);
        const auto [it, inserted] =
            rootToComponent.emplace(root, static_cast<ComponentId>(componentCount_));
        if(inserted) componentCount_++;
        componentOfIndex_[i] = it->second;
    }
}

// Additions can always be absorbed in place: a union never needs to split.
void Topology::noteAdded(EntityId id) {
    if(dirty_) return;
    const EntityRecord *e = doc_->entities().find(id);
    if(e == nullptr) return;

    const auto fresh = static_cast<uint32_t>(order_.size());
    index_.emplace(id, fresh);
    order_.push_back(id);
    componentParent_.push_back(fresh);
    coincidenceParent_.push_back(fresh);

    for(size_t i = 0; i < entityInfo(e->kind).pointCount; i++) {
        const uint32_t p = indexOf(e->points[i]);
        if(p != NO_COMPONENT) unite(componentParent_, fresh, p);
    }
    // The union-find is now exact; only the derived numbering is stale, and
    // that is a linear pass rather than a rehash of the whole document.
    renumber();
}

void Topology::noteAdded(ConstraintId id) {
    if(dirty_) return;
    const ConstraintRecord *c = doc_->constraints().find(id);
    if(c == nullptr) return;

    const size_t n = boundOperandCount(*c);
    const uint32_t first = indexOf(c->operands[0]);
    if(first == NO_COMPONENT) {
        dirty_ = true;
        return;
    }
    for(size_t i = 1; i < n; i++) {
        const uint32_t other = indexOf(c->operands[i]);
        if(other != NO_COMPONENT) unite(componentParent_, first, other);
    }
    if(c->kind == ConstraintKind::Coincident) {
        const uint32_t second = indexOf(c->operands[1]);
        if(second != NO_COMPONENT) unite(coincidenceParent_, first, second);
    }
    renumber();
}

ComponentId Topology::componentOf(EntityId id) const {
    ensureFresh();
    const uint32_t i = indexOf(id);
    return i == NO_COMPONENT ? NO_COMPONENT : componentOfIndex_[i];
}

size_t Topology::componentCount() const {
    ensureFresh();
    return componentCount_;
}

std::vector<EntityId> Topology::membersOf(ComponentId component) const {
    ensureFresh();
    std::vector<EntityId> out;
    for(uint32_t i = 0; i < order_.size(); i++) {
        if(componentOfIndex_[i] == component) out.push_back(order_[i]);
    }
    return out;
}

bool Topology::sameComponent(EntityId a, EntityId b) const {
    const ComponentId ca = componentOf(a);
    return ca != NO_COMPONENT && ca == componentOf(b);
}

std::vector<EntityId> Topology::coincidentRun(EntityId point) const {
    ensureFresh();
    std::vector<EntityId> out;
    const uint32_t self = indexOf(point);
    if(self == NO_COMPONENT) return out;

    const uint32_t root = findRoot(coincidenceParent_, self);
    for(uint32_t i = 0; i < order_.size(); i++) {
        if(findRoot(coincidenceParent_, i) == root) out.push_back(order_[i]);
    }
    // order_ follows the ID-ordered entity table, so this is already ID-sorted.
    return out;
}

bool Topology::coincident(EntityId a, EntityId b) const {
    ensureFresh();
    const uint32_t ia = indexOf(a);
    const uint32_t ib = indexOf(b);
    if(ia == NO_COMPONENT || ib == NO_COMPONENT) return false;
    return findRoot(coincidenceParent_, ia) == findRoot(coincidenceParent_, ib);
}

// A closed loop is exactly: every vertex touched by precisely two edges, and
// all edges reachable from one another. Both conditions are needed — the degree
// test alone admits two disjoint triangles, and connectivity alone admits an
// open run.
std::optional<std::vector<EntityId>> findBoundaryCycle(const Document &doc,
                                                       const Topology &topology,
                                                       std::span<const EntityId> edges) {
    if(edges.size() < 2) return std::nullopt;

    // Each edge contributes its two endpoints, collapsed to their coincidence
    // class so that "the same vertex" is topological rather than positional.
    struct Edge {
        EntityId id;
        EntityId a, b;  // representative points of each end
    };
    std::vector<Edge> parsed;
    parsed.reserve(edges.size());

    for(EntityId id : edges) {
        const EntityRecord *e = doc.entities().find(id);
        if(e == nullptr) return std::nullopt;
        if(!entityInfo(e->kind).boundaryCapable) return std::nullopt;
        // v1 boundaries are segments; arcs join them once their macro lands.
        if(e->kind != EntityKind::Segment) return std::nullopt;

        const std::vector<EntityId> runA = topology.coincidentRun(e->points[0]);
        const std::vector<EntityId> runB = topology.coincidentRun(e->points[1]);
        if(runA.empty() || runB.empty()) return std::nullopt;
        parsed.push_back(Edge{id, runA.front(), runB.front()});
    }

    // Degree check over vertex representatives.
    std::unordered_map<EntityId, std::vector<size_t>> incident;
    for(size_t i = 0; i < parsed.size(); i++) {
        incident[parsed[i].a].push_back(i);
        incident[parsed[i].b].push_back(i);
    }
    if(incident.size() != parsed.size()) return std::nullopt;  // vertices != edges
    for(const auto &[vertex, list] : incident) {
        if(list.size() != 2) return std::nullopt;
    }

    // Walk it. Starting from the lowest edge ID keeps the reported order a
    // function of the document rather than of the caller's argument order.
    size_t start = 0;
    for(size_t i = 1; i < parsed.size(); i++) {
        if(parsed[i].id < parsed[start].id) start = i;
    }

    std::vector<EntityId> cycle;
    std::vector<bool> used(parsed.size(), false);
    size_t current = start;
    EntityId vertex = parsed[start].b;
    for(size_t step = 0; step < parsed.size(); step++) {
        cycle.push_back(parsed[current].id);
        used[current] = true;

        const std::vector<size_t> &next = incident[vertex];
        const size_t candidate = next[0] == current ? next[1] : next[0];
        if(step + 1 == parsed.size()) {
            // The last edge must close back onto where we began.
            if(candidate != start) return std::nullopt;
            break;
        }
        if(used[candidate]) return std::nullopt;  // closed early: not one loop
        vertex = parsed[candidate].a == vertex ? parsed[candidate].b : parsed[candidate].a;
        current = candidate;
    }

    if(cycle.size() != parsed.size()) return std::nullopt;
    return cycle;
}

}  // namespace paroculus
