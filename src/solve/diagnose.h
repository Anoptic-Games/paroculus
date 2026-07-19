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
    // May be empty on an Inconsistent verdict, and often is: SolveSpace tends to
    // blame the constraint it could not satisfy, which is the candidate, and
    // says nothing about which of the existing ones it disagrees with. Empty
    // means the failure could not be attributed more finely than "this
    // candidate", not that there is no conflict — the verdict is what the
    // downgrade reads. Attributing it properly is stage 5's conflict walking.
    std::vector<ConstraintId> conflicting;

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

// Solves the affected components with `candidate` added, and reports what would
// happen if it were committed.
//
// candidate: a constraint record that need not have an ID yet.
// Returns Malformed without solving when the record would be refused anyway,
// so the action surface gets one answer rather than two failure modes.
CandidateCheck checkCandidate(const Document &doc, const Topology &topology,
                              const ConstraintRecord &candidate);

}  // namespace paroculus
