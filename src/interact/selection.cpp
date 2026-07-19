#include "interact/selection.h"

#include <algorithm>

namespace paroculus {

std::string Signature::describe() const {
    std::string out = "{";
    for(size_t i = 0; i < kinds.size(); i++) {
        if(i) out += ", ";
        out += entityInfo(kinds[i]).name;
    }
    out += "}";
    return out;
}

void Selection::normalise() {
    std::sort(items_.begin(), items_.end());
    items_.erase(std::unique(items_.begin(), items_.end()), items_.end());
}

void Selection::clear() {
    items_.clear();
    depth_ = 0;
}

void Selection::set(EntityId id) {
    items_.clear();
    if(id.valid()) items_.push_back(id);
    depth_ = 0;
}

void Selection::set(std::vector<EntityId> ids) {
    items_ = std::move(ids);
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [](EntityId id) { return !id.valid(); }),
                 items_.end());
    normalise();
    depth_ = 0;
}

void Selection::add(EntityId id) {
    if(!id.valid() || contains(id)) return;
    items_.push_back(id);
    normalise();
}

void Selection::remove(EntityId id) {
    items_.erase(std::remove(items_.begin(), items_.end(), id), items_.end());
}

void Selection::toggle(EntityId id) {
    if(!id.valid()) return;
    if(contains(id)) {
        remove(id);
    } else {
        add(id);
    }
}

bool Selection::contains(EntityId id) const {
    return std::binary_search(items_.begin(), items_.end(), id);
}

// Expansion is over shared geometry, not over constraints. Two segments held
// parallel are related but are not one shape, and selecting one should not drag
// in the other — the constraint graph is what the *solver* groups by, and the
// coincidence graph is what the *user* sees as joined.
std::vector<EntityId> connectedRun(const Document &doc, const Topology &topology,
                                   EntityId seed) {
    std::vector<EntityId> out;
    std::vector<EntityId> pending{seed};

    auto push = [&](EntityId id) {
        if(!id.valid()) return;
        if(std::find(out.begin(), out.end(), id) != out.end()) return;
        if(std::find(pending.begin(), pending.end(), id) != pending.end()) return;
        pending.push_back(id);
    };

    while(!pending.empty()) {
        const EntityId current = pending.back();
        pending.pop_back();
        const EntityRecord *e = doc.entities().find(current);
        if(e == nullptr) continue;
        if(std::find(out.begin(), out.end(), current) != out.end()) continue;
        out.push_back(current);

        // Down: the points that define this entity.
        for(size_t i = 0; i < entityInfo(e->kind).pointCount; i++) push(e->points[i]);

        // Sideways: every point sharing this vertex.
        if(e->kind == EntityKind::Point) {
            for(EntityId joined : topology.coincidentRun(current)) push(joined);
        }

        // Up: entities defined by any point we have reached.
        for(const EntityRecord &other : doc.entities().records()) {
            for(size_t i = 0; i < entityInfo(other.kind).pointCount; i++) {
                if(other.points[i] != current) continue;
                push(other.id);
            }
        }
    }

    std::sort(out.begin(), out.end());
    return out;
}

// Descent replaces each selected shape with its parts. The parts of a segment
// are its endpoints; the parts of a run are the segments in it.
bool Selection::descend(const Document &doc, const Topology &topology) {
    if(items_.empty()) return false;

    std::vector<EntityId> parts;
    bool descended = false;

    for(EntityId id : items_) {
        const EntityRecord *e = doc.entities().find(id);
        if(e == nullptr) continue;
        const size_t points = entityInfo(e->kind).pointCount;
        if(points == 0) {
            // A point has no parts; it stays, so a mixed-depth selection keeps
            // the vertex the user already picked.
            parts.push_back(id);
            continue;
        }
        for(size_t i = 0; i < points; i++) parts.push_back(e->points[i]);
        descended = true;
    }

    if(!descended) return false;
    const int previous = depth_;
    set(std::move(parts));
    depth_ = previous + 1;
    (void)topology;
    return true;
}

bool Selection::ascend(const Document &doc, const Topology &topology) {
    if(depth_ <= 0) return false;

    std::vector<EntityId> whole;
    for(EntityId id : items_) {
        for(EntityId member : connectedRun(doc, topology, id)) whole.push_back(member);
    }

    const int previous = depth_;
    set(std::move(whole));
    depth_ = previous - 1;
    return true;
}

Signature Selection::signature(const Document &doc) const {
    Signature signature;
    signature.kinds.reserve(items_.size());
    for(EntityId id : items_) {
        const EntityRecord *e = doc.entities().find(id);
        if(e != nullptr) signature.kinds.push_back(e->kind);
    }
    // Sorted: the surface asks what can apply to a set, not to a click order.
    std::sort(signature.kinds.begin(), signature.kinds.end());
    return signature;
}

}  // namespace paroculus
