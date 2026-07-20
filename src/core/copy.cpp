#include "core/copy.h"

#include <algorithm>

#include "core/composition.h"
#include "core/transform.h"

namespace paroculus {
namespace {

// Whether every entity a container names is inside the copied set. Asked of
// regions and tags alike, because "can this come whole" is the same question for
// both and answering it twice is how the two come to disagree.
bool allInside(std::span<const EntityId> named,
               const std::unordered_map<EntityId, EntityId> &map) {
    if(named.empty()) return false;
    for(EntityId id : named) {
        if(map.find(id) == map.end()) return false;
    }
    return true;
}

}  // namespace

std::vector<EntityId> CopyStep::copiedEntities() const {
    std::vector<EntityId> out;
    out.reserve(entities.size());
    for(const auto &[from, to] : entities) out.push_back(to);
    // ID order, not map order: hash-map iteration order is exactly the kind of
    // thing determinism forbids behaviour from being keyed on, and this is what
    // a caller selects afterwards.
    std::sort(out.begin(), out.end());
    return out;
}

CopyStep copyStep(const Document &doc, std::span<const EntityId> selection, double dx,
                  double dy) {
    return copyStep(doc, selection,
                    CopyPlacement{[dx, dy](Point p) { return Point{p.x + dx, p.y + dy}; },
                                  false});
}

CopyStep copyStep(const Document &doc, std::span<const EntityId> selection,
                  const CopyPlacement &placement) {
    CopyStep step;
    if(!placement.place) return step;
    const std::vector<EntityId> source = transformClosure(doc, selection);
    if(source.empty()) return step;

    // The whole bijection is fixed before a single command is emitted, because a
    // record may name another record in the same step and a name has to resolve
    // to the image rather than to the original. Two passes, and the first one
    // touches nothing.
    uint32_t nextEntity = doc.entities().allocator().next();
    for(EntityId id : source) {
        if(doc.entities().find(id) == nullptr) continue;
        step.entities.emplace(id, EntityId(nextEntity++));
    }

    // Points before what refers to them. Source is in ID order and a defining
    // point is always older than what defines itself by it — entities claim IDs
    // in creation order and a segment cannot name an endpoint that did not exist
    // — so ID order is already dependency order and no sort is needed. Emitting
    // in it also makes the copy deterministic, which is the property that lets
    // the isomorphism test compare two runs.
    for(EntityId id : source) {
        const EntityRecord *r = doc.entities().find(id);
        if(r == nullptr) continue;
        EntityRecord copy = *r;
        copy.id = step.entities.at(id);
        const size_t points = entityInfo(r->kind).pointCount;
        for(size_t i = 0; i < points; i++) {
            const auto it = step.entities.find(r->points[i]);
            // Cannot miss: the closure pulled every defining point in. Guarded
            // anyway, because a record naming an entity the step does not create
            // would be refused by validation and take the whole step with it.
            if(it == step.entities.end()) return CopyStep{};
            copy.points[i] = it->second;
        }
        // An arc's sweep runs counter-clockwise from start to end, so a
        // reflected arc keeps its centre and trades its endpoints. Without the
        // swap the copy is the complementary arc — same three points, the long
        // way round — which satisfies every constraint the original did and is
        // not the shape the user is looking at.
        if(copy.kind == EntityKind::Arc && placement.reversesOrientation) {
            std::swap(copy.points[1], copy.points[2]);
        }
        if(copy.kind == EntityKind::Point) {
            const Point p = placement.place(Point{copy.seeds[0], copy.seeds[1]});
            copy.seeds[0] = p.x;
            copy.seeds[1] = p.y;
        }
        step.commands.push_back(AddRecord<EntityRecord>{copy});
    }

    uint32_t nextConstraint = doc.constraints().allocator().next();
    for(const ConstraintRecord &c : doc.constraints().records()) {
        const size_t bound = boundOperandCount(c);
        if(bound == 0) continue;
        size_t inside = 0;
        for(size_t i = 0; i < bound; i++) {
            if(step.entities.count(c.operands[i]) != 0) inside++;
        }
        if(inside == 0) continue;
        if(inside < bound) {
            // One operand outside: this relation says where the original sits
            // relative to the rest of the drawing, and the copy does not sit
            // there. Dropped, counted, and the count is what the surface reports
            // — re-binding offers are the transient strip's business, not this
            // function's.
            step.droppedConstraints++;
            continue;
        }
        ConstraintRecord copy = c;
        copy.id = ConstraintId(nextConstraint++);
        for(size_t i = 0; i < bound; i++) copy.operands[i] = step.entities.at(c.operands[i]);
        step.constraints.emplace(c.id, copy.id);
        step.commands.push_back(AddRecord<ConstraintRecord>{copy});
    }

    uint32_t nextRegion = doc.regions().allocator().next();
    for(const RegionRecord &r : doc.regions().records()) {
        // Outlines only in this pass; a composite names regions rather than
        // entities and is decided by whether its operands came, which is not
        // known until they have.
        if(r.op != CompositeOp::Outline) continue;
        if(!allInside(r.boundary, step.entities)) {
            // A fill whose outline came half across bounds nothing. Dropped
            // whole rather than copied thin, because a record born broken is a
            // diagnostic the user did not cause.
            const bool touched = std::any_of(r.boundary.begin(), r.boundary.end(),
                                             [&](EntityId e) {
                                                 return step.entities.count(e) != 0;
                                             });
            if(touched) step.droppedRegions++;
            continue;
        }
        RegionRecord copy = r;
        copy.id = RegionId(nextRegion++);
        for(EntityId &e : copy.boundary) e = step.entities.at(e);
        step.regions.emplace(r.id, copy.id);
        step.commands.push_back(AddRecord<RegionRecord>{copy});
    }

    // Composites, in ID order, so one naming another resolves. A composite's
    // operands are older than it is for the same reason a segment's endpoints
    // are, and cycles are refused at creation, so one pass suffices.
    for(const RegionRecord &r : doc.regions().records()) {
        if(r.op == CompositeOp::Outline) continue;
        bool whole = !r.operands.empty();
        bool touched = false;
        for(RegionId id : r.operands) {
            if(step.regions.count(id) != 0) {
                touched = true;
            } else {
                whole = false;
            }
        }
        if(!whole) {
            if(touched) step.droppedRegions++;
            continue;
        }
        RegionRecord copy = r;
        copy.id = RegionId(nextRegion++);
        for(RegionId &id : copy.operands) id = step.regions.at(id);
        step.regions.emplace(r.id, copy.id);
        step.commands.push_back(AddRecord<RegionRecord>{copy});
    }

    uint32_t nextTag = doc.tags().allocator().next();
    for(const TagRecord &t : doc.tags().records()) {
        // A tag that is already broken is not copied at all.
        //
        // Its constraint list may have shrunk to empty, and "every constraint
        // came" is vacuously true of nothing — so the tag would be duplicated
        // onto brand-new geometry and born broken, with render drawing the
        // degradation diagnostic over something the user just created. A
        // degraded record is what an *edit* leaves behind; nothing has been
        // edited here, and copy.h says a tag comes whole or not at all.
        const bool whole = tagState(doc, t) == TagState::Whole;
        const bool entitiesIn = allInside(t.entities, step.entities);
        bool constraintsIn = !t.constraints.empty();
        for(ConstraintId c : t.constraints) {
            if(step.constraints.count(c) == 0) constraintsIn = false;
        }
        if(!whole || !entitiesIn || !constraintsIn) {
            const bool touched = std::any_of(t.entities.begin(), t.entities.end(),
                                             [&](EntityId e) {
                                                 return step.entities.count(e) != 0;
                                             });
            if(touched) step.droppedTags++;
            continue;
        }
        TagRecord copy = t;
        copy.id = TagId(nextTag++);
        for(EntityId &e : copy.entities) e = step.entities.at(e);
        for(ConstraintId &c : copy.constraints) c = step.constraints.at(c);
        step.tags.emplace(t.id, copy.id);
        step.commands.push_back(AddRecord<TagRecord>{copy});
    }

    return step;
}

}  // namespace paroculus
