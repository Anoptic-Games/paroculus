#include "core/parameters.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace paroculus {

// Depth guards a hand-edited cycle, so it is tested before the memo: a cycle
// never completes and so never caches, and terminates by exhausting the bound.
// The memo then keys resolved values by id, so a parameter reached through two
// nodes — or two paths of a diamond — resolves once. A well-formed graph's
// longest chain sits far under the bound, so every reference has budget to
// resolve fully wherever it is reached and the id key is exact.
std::optional<double> TableParameterEnv::lookup(ParameterId id) const {
    if(depth_ <= 0) return std::nullopt;

    if(memo_ == nullptr) memo_ = std::make_shared<Memo>();
    const auto it = memo_->find(id);
    if(it != memo_->end()) return it->second;

    const ParameterRecord *r = table_.find(id);
    if(r == nullptr) {
        (*memo_)[id] = std::nullopt;
        return std::nullopt;
    }
    const TableParameterEnv next(table_, depth_ - 1, memo_);
    const std::optional<double> v = r->value.evaluate(&next);
    (*memo_)[id] = v;
    return v;
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
