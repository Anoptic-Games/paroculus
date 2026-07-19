#include "core/parameters.h"

#include <algorithm>
#include <vector>

namespace paroculus {

std::optional<double> TableParameterEnv::lookup(ParameterId id) const {
    if(depth_ <= 0) return std::nullopt;
    const ParameterRecord *r = table_.find(id);
    if(r == nullptr) return std::nullopt;
    const TableParameterEnv next(table_, depth_ - 1);
    return r->value.evaluate(&next);
}

std::optional<double> evaluateParameter(const ParameterTable &table, ParameterId id) {
    const ParameterRecord *r = table.find(id);
    if(r == nullptr) return std::nullopt;
    const TableParameterEnv env(table);
    return r->value.evaluate(&env);
}

// Reachability from `value` back to `id`, over the parameter graph as it stands
// plus the proposed assignment. Iterative with a visited set: the graph is
// small, and the set keeps a pre-existing diamond from being walked
// exponentially.
bool wouldCycle(const ParameterTable &table, ParameterId id, const Slot &value) {
    std::vector<ParameterId> pending = value.references();
    std::vector<ParameterId> seen;

    while(!pending.empty()) {
        const ParameterId cur = pending.back();
        pending.pop_back();
        if(cur == id) return true;
        if(std::find(seen.begin(), seen.end(), cur) != seen.end()) continue;
        seen.push_back(cur);

        const ParameterRecord *r = table.find(cur);
        if(r == nullptr) continue;
        for(ParameterId ref : r->value.references()) pending.push_back(ref);
    }
    return false;
}

const ParameterRecord *findParameterByName(const ParameterTable &table, std::string_view name) {
    for(const ParameterRecord &r : table.records()) {
        if(r.name == name) return &r;
    }
    return nullptr;
}

}  // namespace paroculus
