// Solved state: the derived geometry cache that rendering and hit testing read.
// Stage 0 carries the demo sketch's four points verbatim; stage 1 replaces this
// with the document's parameter tables. What survives the replacement is the
// rule enforced here — no solver type crosses into core, so SolveStatus is the
// only status the layers above ever see.
#pragma once

#include "core/geom.h"

namespace paroculus {

// Values match SLVS_RESULT_* deliberately, so mapping is a cast and the
// selftest's printed result code is unchanged. solve/ static_asserts the
// correspondence; nothing else may assume it.
enum class SolveStatus : int {
    Unsolved = -1,
    Okay = 0,
    Inconsistent = 1,
    DidNotConverge = 2,
    TooManyUnknowns = 3,
    RedundantOkay = 4,
};

// s: any status, including values outside the enum.
// Returns a stable lowercase label; anything unrecognised reads "unsolved".
const char *statusName(SolveStatus s);

// A solved two-segment sketch. Segment A is driven horizontal at a fixed
// length; segment B is held parallel to A with len(A)/len(B) pinned to a ratio.
// Parallelism and proportion as primitives, not derived results.
struct Solution {
    SolveStatus status = SolveStatus::Unsolved;
    int dof = -1;  // remaining degrees of freedom; 0 == fully constrained
    Point a0, a1;  // segment A endpoints
    Point b0, b1;  // segment B endpoints

    // Redundant-but-consistent is a solve-time pass. Flagging redundancy is a
    // creation-time job (stage 2), not a reason to reject solved geometry.
    bool ok() const {
        return status == SolveStatus::Okay || status == SolveStatus::RedundantOkay;
    }
};

}  // namespace paroculus
