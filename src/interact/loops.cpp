#include "interact/loops.h"

#include "interact/selection.h"

namespace paroculus {

std::optional<std::vector<EntityId>> closedBoundaryContaining(const Document &doc,
                                                              const Topology &topology,
                                                              EntityId seed) {
    if(!seed.valid() || doc.entities().find(seed) == nullptr) return std::nullopt;

    // The run is what the user has been drawing: everything reachable through
    // shared and coincident vertices. Edges are what a boundary is made of, so
    // the points come along for the walk and drop out of the cycle test.
    std::vector<EntityId> edges;
    for(EntityId id : connectedRun(doc, topology, seed)) {
        const EntityRecord *e = doc.entities().find(id);
        if(e == nullptr) continue;
        // Construction geometry is not part of the outline it helped place. An
        // arc's centre would otherwise turn every arc into a run that cannot
        // close.
        if(e->role == Role::Construction) continue;
        if(!entityInfo(e->kind).boundaryCapable) continue;
        edges.push_back(id);
    }
    if(edges.empty()) return std::nullopt;

    return findBoundaryCycle(doc, topology, edges);
}

}  // namespace paroculus
