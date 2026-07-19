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
    (void)topology;
    if(items_.empty()) return false;

    // The ladder is shape, then edges, then points — which is what "descends
    // from a shape to its edges and points" says, and what the level counter
    // has to mean if ascent is to be its inverse. A shape selected by a click
    // is its whole connected run, so the first descent is the one that drops
    // the run's vertices and leaves the edges the user can now pick between.
    bool haveEdges = false;
    bool haveLoose = false;
    for(EntityId id : items_) {
        const EntityRecord *e = doc.entities().find(id);
        if(e == nullptr) continue;
        if(entityInfo(e->kind).pointCount > 0) {
            haveEdges = true;
        } else {
            haveLoose = true;
        }
    }
    if(!haveEdges) return false;  // points have no parts; the ladder ends here

    std::vector<EntityId> parts;
    if(haveLoose) {
        // Shape to edges. The vertices go; what is left is the geometry that
        // still has parts of its own.
        for(EntityId id : items_) {
            const EntityRecord *e = doc.entities().find(id);
            if(e != nullptr && entityInfo(e->kind).pointCount > 0) parts.push_back(id);
        }
    } else {
        // Edges to points.
        for(EntityId id : items_) {
            const EntityRecord *e = doc.entities().find(id);
            if(e == nullptr) continue;
            const size_t points = entityInfo(e->kind).pointCount;
            for(size_t i = 0; i < points; i++) parts.push_back(e->points[i]);
        }
    }
    if(parts.empty()) return false;

    const int previous = depth_;
    set(std::move(parts));
    depth_ = previous + 1;
    return true;
}

bool Selection::ascend(const Document &doc, const Topology &topology) {
    if(depth_ <= 0) return false;

    std::vector<EntityId> whole;
    if(depth_ == 1) {
        // Back to the whole shape, which is the run each item belongs to.
        for(EntityId id : items_) {
            for(EntityId member : connectedRun(doc, topology, id)) whole.push_back(member);
        }
    } else {
        // One rung only: points back to the edges built on them. Going straight
        // to the run here would make ascent skip the level descent stopped at,
        // so two descents and one ascent would not leave the user in the middle.
        for(EntityId id : items_) {
            bool used = false;
            for(const EntityRecord &e : doc.entities().records()) {
                const size_t points = entityInfo(e.kind).pointCount;
                for(size_t i = 0; i < points; i++) {
                    if(e.points[i] != id) continue;
                    whole.push_back(e.id);
                    used = true;
                    break;
                }
            }
            // A vertex nothing is built on has nowhere to rise to, so it stays
            // rather than vanishing out of the selection.
            if(!used) whole.push_back(id);
        }
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
