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

}  // namespace

CandidateCheck checkCandidate(const Document &doc, const Topology &topology,
                              const ConstraintRecord &candidate) {
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
        case SolveStatus::Inconsistent:
            check.verdict = CandidateVerdict::Inconsistent;
            check.conflicting = outcome.failed;
            break;
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
