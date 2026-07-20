// Creation-time validation.
//
// Over-constraint is handled at two moments, and this is the first: an action
// that would make the system inconsistent or redundant is caught before commit.
// The candidate is solved speculatively, and on failure the action surface
// offers the downgrade — add it as a driven reference measurement instead of a
// driving constraint — with the conflicting constraints highlighted.
//
// Redundant-but-consistent is tolerated at solve time but flagged here, because
// redundancy is where later edits go to die: two constraints that agree today
// will disagree after the next value edit, and the user who added the second
// one was told nothing.
//
// Nothing here mutates the document. The candidate rides in as a speculative
// constraint on a forked context, so there is no copy to keep in sync and no
// way for a check to leave a trace.
#pragma once

#include <cstdint>
#include <vector>

#include "core/document.h"
#include "core/topology.h"

namespace paroculus {

enum class CandidateVerdict : uint8_t {
    Consistent,      // it solves and it says something new
    Redundant,       // it solves but adds nothing; flag it, do not refuse it
    Inconsistent,    // it cannot hold alongside what is already declared
    DidNotConverge,  // no verdict; treat as a refusal the user can retry
    Malformed,       // operands missing, or a signature the taxonomy rejects
};

struct CandidateCheck {
    CandidateVerdict verdict = CandidateVerdict::Malformed;

    // The constraints the solver blamed, for Inconsistent — never the candidate
    // itself, which rides in as an extra and has no ID to highlight. This
    // becomes a selection-like highlight the user can walk, not a modal dialog.
    //
    // The solver alone leaves this empty more often than not: SolveSpace blames
    // the constraint it could not satisfy, which is the candidate, and says
    // nothing about which of the existing ones it disagrees with. So the set is
    // filled by asking the counterfactual instead — which relation, suppressed,
    // would let the candidate hold — one solve per relation in the affected
    // components. See conflictWalk below.
    std::vector<ConstraintId> conflicting;

    // Whether the walk ran to completion. False with an Inconsistent verdict
    // means the conflict could not be attributed more finely than "this
    // candidate", not that there is no conflict — the component was larger than
    // the walk's budget, or no single suppression was enough. The surface has
    // to be able to tell those apart from "nothing conflicts", because offering
    // an empty walkable set as though it were the answer is a lie the user
    // cannot see through.
    bool attributed = false;

    // Degrees of freedom the candidate would consume. Zero with a Consistent
    // verdict is what redundancy looks like from the other side.
    int dofBefore = -1;
    int dofAfter = -1;

    // Whether committing is safe without a prompt.
    bool clean() const { return verdict == CandidateVerdict::Consistent; }
    // Whether committing is possible at all.
    bool committable() const {
        return verdict == CandidateVerdict::Consistent || verdict == CandidateVerdict::Redundant;
    }
};

// How hard to work at attributing a conflict.
struct DiagnoseOptions {
    // Suppress each existing relation in turn and ask whether the candidate can
    // hold without it. Every relation that says yes is one the candidate
    // disagrees with, which is exactly the set PRINCIPLES wants selectable and
    // walkable.
    //
    // This is the same counterfactual stage 3's saturation attribution runs, and
    // it uses the primitive that amendment introduced — SolveOptions::suppressed
    // — for the same reason: a hard pin only ever names itself, and asking the
    // solver who it blames only ever names the question.
    bool conflictWalk = true;

    // How many relations the walk will consider before giving up and reporting
    // the conflict as unattributable. The walk costs one solve each, and it runs
    // at a keystroke rather than per frame, so the bound is generous — but it is
    // a bound, because a component with a thousand relations would otherwise
    // turn one imposition into a thousand solves.
    //
    // Exceeding it leaves `attributed` false rather than reporting a truncated
    // set, because a partial conflict set read as a whole one sends the user
    // walking relations that are not the ones disagreeing.
    size_t conflictWalkLimit = 96;
};

// Solves the affected components with `candidate` added, and reports what would
// happen if it were committed.
//
// candidate: a constraint record that need not have an ID yet.
// Returns Malformed without solving when the record would be refused anyway,
// so the action surface gets one answer rather than two failure modes.
CandidateCheck checkCandidate(const Document &doc, const Topology &topology,
                              const ConstraintRecord &candidate,
                              const DiagnoseOptions &options = {});

}  // namespace paroculus
