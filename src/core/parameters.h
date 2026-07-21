// Named-parameter semantics over the generic record table.
//
// A parameter's value is itself a Slot, so parameters may build on each other
// (margin = gutter * 2). That is the one place a cycle can enter the value
// graph, and assigning a parameter is therefore the one operation that has to
// check for it. Measurements are excluded from the expression language
// entirely, so this check plus that exclusion make the graph acyclic by
// construction rather than by convention.
//
// Storage lives in RecordTable like every other record kind; only the rules
// live here.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string_view>

#include "core/records.h"
#include "core/slots.h"
#include "core/table.h"

namespace paroculus {

using ParameterTable = RecordTable<ParameterRecord>;

// Resolves parameter references against a table, with a depth bound and a
// per-evaluation memo.
//
// The bound is not redundant with the cycle check: assignment guards the front
// door, and this contains a loop that arrived some other way, such as a
// hand-edited file. Evaluation must terminate on any input.
//
// The memo is not an optimization to reach for later. Slot::evaluate memoizes
// within one slot, but a slot reading the same parameter through two nodes
// re-resolves it once per node, so a chain of doublings costs 2^n: a document
// with a 60-deep chain never evaluates, on the solve path, the frame, the bake
// and parameter deletion alike. The memo resolves each parameter at most once
// per top-level evaluate, making the cost linear in the reference graph. It is
// shared down the recursion — the nested env resolving a reference sees what the
// root already resolved — and it caches nullopt too, since re-walking a failing
// branch is the same blowup.
class TableParameterEnv : public ParameterEnv {
public:
    explicit TableParameterEnv(const ParameterTable &table, int depth = 64)
        : table_(table), depth_(depth) {}

    // Returns nullopt for an unknown ID, an unevaluable expression, or a
    // reference chain deeper than the bound.
    std::optional<double> lookup(ParameterId id) const override;

private:
    // A resolved parameter to its value, for the life of one top-level evaluate.
    // A present entry means resolved; its nullopt means resolved-to-unevaluable.
    // Ordered by ParameterId and only ever point-queried, so no iteration order
    // reaches a result.
    using Memo = std::map<ParameterId, std::optional<double>>;

    // Recursion constructor: one level deeper, sharing the root's memo.
    TableParameterEnv(const ParameterTable &table, int depth, std::shared_ptr<Memo> memo)
        : table_(table), depth_(depth), memo_(std::move(memo)) {}

    const ParameterTable &table_;
    int depth_;
    // Lazily allocated on the first reference, so a constant slot allocates
    // nothing; mutable because resolution is logically const.
    mutable std::shared_ptr<Memo> memo_;
};

// Convenience over TableParameterEnv for a single lookup.
std::optional<double> evaluateParameter(const ParameterTable &table, ParameterId id);

// True when assigning `value` to `id` would make `id` reachable from itself.
// Exposed so the action layer can offer the diagnosis before the user commits,
// rather than reporting a refusal after the fact.
bool wouldCycle(const ParameterTable &table, ParameterId id, const Slot &value);

// Returns the first parameter with this name, or nullptr. Names are a
// presentation affordance; IDs are identity, so duplicates are a document-level
// concern rather than a table invariant.
const ParameterRecord *findParameterByName(const ParameterTable &table, std::string_view name);

}  // namespace paroculus
