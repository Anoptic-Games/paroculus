#include "solve/context.h"

#include <algorithm>

namespace paroculus {

SolveContext SolveContext::build(const Document &doc, std::vector<EntityId> members) {
    // Sorted, so translation order — and therefore the solver's parameter
    // numbering, and therefore its arithmetic — is a function of the document
    // and not of how the partition happened to enumerate.
    std::sort(members.begin(), members.end());

    SolveContext context;
    context.members_ = std::move(members);
    for(EntityId id : context.members_) {
        const EntityRecord *e = doc.entities().find(id);
        if(e == nullptr) continue;
        if(entityInfo(e->kind).ownParamCount == 0) continue;
        SeedSpan span;
        span.entity = id;
        span.seeds = e->seeds;
        context.params_.push_back(span);
    }
    return context;
}

SolveContext SolveContext::forComponent(const Document &doc, const Topology &topology,
                                        EntityId anchor) {
    const ComponentId component = topology.componentOf(anchor);
    if(component == NO_COMPONENT) return SolveContext();
    return build(doc, topology.membersOf(component));
}

SolveContext SolveContext::forComponents(const Document &doc, const Topology &topology,
                                         const std::vector<EntityId> &anchors) {
    std::vector<ComponentId> seen;
    std::vector<EntityId> members;
    for(EntityId anchor : anchors) {
        const ComponentId component = topology.componentOf(anchor);
        if(component == NO_COMPONENT) continue;
        if(std::find(seen.begin(), seen.end(), component) != seen.end()) continue;
        seen.push_back(component);
        const std::vector<EntityId> part = topology.membersOf(component);
        members.insert(members.end(), part.begin(), part.end());
    }
    return build(doc, std::move(members));
}

SolveContext SolveContext::forWholeDocument(const Document &doc) {
    std::vector<EntityId> members;
    members.reserve(doc.entities().size());
    for(const EntityRecord &e : doc.entities().records()) members.push_back(e.id);
    return build(doc, std::move(members));
}

bool SolveContext::contains(EntityId id) const {
    return std::binary_search(members_.begin(), members_.end(), id);
}

std::optional<Point> SolveContext::point(EntityId id) const {
    for(const SeedSpan &s : params_) {
        if(s.entity == id) return Point{s.seeds[0], s.seeds[1]};
    }
    return std::nullopt;
}

std::optional<double> SolveContext::radius(EntityId id) const {
    for(const SeedSpan &s : params_) {
        if(s.entity == id) return s.seeds[0];
    }
    return std::nullopt;
}

std::vector<Command> SolveContext::commitCommands(const Document &doc) const {
    std::vector<Command> out;
    for(const SeedSpan &s : params_) {
        const EntityRecord *e = doc.entities().find(s.entity);
        if(e == nullptr) continue;
        // Bit-exact comparison on purpose: a drag that moved nothing must
        // journal nothing, and "moved" here means the solver wrote a different
        // double, not a different-looking one.
        if(e->seeds == s.seeds) continue;
        EntityRecord updated = *e;
        updated.seeds = s.seeds;
        out.push_back(SetRecord<EntityRecord>{updated});
    }
    return out;
}

}  // namespace paroculus
