#include "solve/diagnose.h"

#include "solve/solve.h"

namespace paroculus {
namespace {

// Whether two operand slots of this kind may be swapped without changing what
// the constraint means.
//
// Derived from columns the taxonomy already carries rather than a new one: a
// two-operand kind whose slots accept the same thing and which carries no value
// is symmetric — coincident, parallel, perpendicular, equal-length,
// equal-radius, both symmetric-axis kinds. Anything valued (length-ratio,
// length-difference, angle) or mixed-kind (point-on-line, midpoint) is
// positional, and swapping it would mean something different.
bool operandsAreInterchangeable(const ConstraintKindInfo &info) {
    return info.operandCount == 2 && info.valueArity == 0 &&
           info.operands[0] == info.operands[1];
}

// Whether the document already declares exactly this relation.
//
// This is the policy the plan hands to creation time, and it exists because the
// solver's own signals cannot be trusted for it: SolveSpace handles coincidence,
// horizontal, vertical and equal-radius by substituting parameters out rather
// than by adding a Jacobian row, so a duplicate reduces its reported degree-of-
// freedom count as though it had added information, and the solve returns plain
// OKAY. Relying on dof or on REDUNDANT_OKAY alone would therefore miss exactly
// the duplicates a user is most likely to create by hand.
bool alreadyDeclared(const Document &doc, const ConstraintRecord &candidate) {
    const ConstraintKindInfo &info = constraintInfo(candidate.kind);

    for(const ConstraintRecord &existing : doc.constraints().records()) {
        if(existing.kind != candidate.kind) continue;
        if(!existing.driving) continue;  // a measurement duplicates nothing
        // Tangency at one end is not a duplicate of tangency at the other.
        if(existing.alternative != candidate.alternative) continue;
        if(info.valueArity == 1 && !(existing.value == candidate.value)) continue;

        // Bound, so a horizontal against a reference axis does not read as a
        // duplicate of a plain one: they are different declarations.
        if(boundOperandCount(existing) != boundOperandCount(candidate)) continue;
        bool positional = true;
        for(size_t i = 0; i < boundOperandCount(candidate); i++) {
            if(existing.operands[i] != candidate.operands[i]) {
                positional = false;
                break;
            }
        }
        if(positional) return true;

        if(operandsAreInterchangeable(info) && existing.operands[0] == candidate.operands[1] &&
           existing.operands[1] == candidate.operands[0]) {
            return true;
        }
    }
    return false;
}

// Every driving relation whose operands all sit inside `context`. These are the
// ones a solve of that context actually carries, and so the only ones that can
// be disagreeing with the candidate.
//
// ID-ordered, because it is walked in order and the resulting conflict set is
// something the user selects through — a set that reshuffled between two
// identical asks would be a set nobody could point at.
std::vector<ConstraintId> relationsIn(const Document &doc, const SolveContext &context) {
    std::vector<ConstraintId> out;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        if(!c.driving) continue;  // a measurement drives nothing and blames nothing
        const size_t n = boundOperandCount(c);
        bool inside = true;
        for(size_t i = 0; i < n && inside; i++) {
            inside = context.contains(c.operands[i]);
        }
        if(inside) out.push_back(c.id);
    }
    return out;
}

// Which existing relations the candidate disagrees with.
//
// The counterfactual: suppress one relation, add the candidate, and see whether
// the system becomes solvable. Every relation that buys solvability is one the
// candidate contradicts. Asking the solver directly does not work — it reports
// the set to remove to make the system solvable, and the candidate is always in
// it, which names the question rather than the answer.
//
// A conflict needing two suppressions at once reports nothing and leaves
// `attributed` false. That is honest: naming one of the pair would send the
// user to delete a relation that is not sufficient, and enumerating pairs is
// quadratic in a walk that is already linear in solves.
std::vector<ConstraintId> walkConflicts(const Document &doc, const SolveContext &baseline,
                                        const ConstraintRecord &driving,
                                        const std::vector<ConstraintId> &relations,
                                        bool &attributed) {
    std::vector<ConstraintId> out;
    for(ConstraintId id : relations) {
        SolveContext trial = baseline;
        SolveOptions options;
        options.diagnoseFailures = false;
        options.extra.push_back(driving);
        options.suppressed.push_back(id);
        if(solve(doc, trial, options).ok()) out.push_back(id);
    }
    attributed = true;
    return out;
}

}  // namespace

CandidateCheck checkCandidate(const Document &doc, const Topology &topology,
                              const ConstraintRecord &candidate,
                              const DiagnoseOptions &diagnose) {
    CandidateCheck check;

    // The taxonomy answers first. An action the model would refuse gets one
    // verdict here rather than a second, differently-shaped failure at commit.
    if(doc.validate(candidate) != CommandError::None) {
        check.verdict = CandidateVerdict::Malformed;
        return check;
    }

    // Every component the candidate touches, the reference axis included:
    // committing it would merge them.
    const size_t operandCount = boundOperandCount(candidate);
    std::vector<EntityId> anchors(candidate.operands.begin(),
                                  candidate.operands.begin() + operandCount);

    // Both components, because committing would merge them.
    SolveContext before = SolveContext::forComponents(doc, topology, anchors);
    if(before.empty()) {
        check.verdict = CandidateVerdict::Malformed;
        return check;
    }

    // Establish the baseline from the same seeds the real solve would use, so
    // the DOF comparison is against the pose the user is actually looking at.
    SolveOptions baselineOptions;
    baselineOptions.diagnoseFailures = false;
    const SolveOutcome baseline = solve(doc, before, baselineOptions);
    check.dofBefore = baseline.dof;

    SolveContext speculative = before;
    SolveOptions options;
    // The candidate is tested as a driving constraint whatever the record says:
    // a reference measurement constrains nothing and would always pass.
    ConstraintRecord driving = candidate;
    driving.driving = true;
    options.extra.push_back(driving);

    const SolveOutcome outcome = solve(doc, speculative, options);
    check.dofAfter = outcome.dof;

    switch(outcome.status) {
        case SolveStatus::Okay:
            check.verdict = CandidateVerdict::Consistent;
            break;
        case SolveStatus::RedundantOkay:
            check.verdict = CandidateVerdict::Redundant;
            break;
        case SolveStatus::Inconsistent: {
            check.verdict = CandidateVerdict::Inconsistent;
            // What it conflicts with, never itself. The candidate rides in as
            // an extra and is reported among the failures like any other, and
            // its id is usually null because nothing has allocated one yet — so
            // highlighting the solver's set unfiltered would point at a
            // constraint that does not exist.
            check.conflicting.clear();
            for(ConstraintId id : outcome.failed) {
                if(id == candidate.id) continue;
                check.conflicting.push_back(id);
            }
            // The solver having named something is not the same as its having
            // attributed the conflict, and in practice it names nothing. The
            // walk is what turns the verdict into a set the user can select.
            if(diagnose.conflictWalk) {
                const std::vector<ConstraintId> relations = relationsIn(doc, before);
                if(relations.size() <= diagnose.conflictWalkLimit) {
                    check.conflicting =
                        walkConflicts(doc, before, driving, relations, check.attributed);
                }
            }
            break;
        }
        default:
            check.verdict = CandidateVerdict::DidNotConverge;
            break;
    }

    // Three independent signals, because no one of them is sufficient. The
    // solver reports REDUNDANT_OKAY for some redundant systems, plain OKAY with
    // an unchanged dof for others, and plain OKAY with a *reduced* dof for the
    // ones it resolves by substitution. Missing a redundancy is the failure
    // mode that matters — it is silent today and contradictory after the next
    // value edit — so a hit on any signal is enough.
    if(check.verdict == CandidateVerdict::Consistent) {
        const bool consumedNothing = check.dofBefore >= 0 && check.dofAfter >= 0 &&
                                     check.dofBefore == check.dofAfter;
        if(consumedNothing || alreadyDeclared(doc, candidate)) {
            check.verdict = CandidateVerdict::Redundant;
        }
    }

    return check;
}

}  // namespace paroculus
