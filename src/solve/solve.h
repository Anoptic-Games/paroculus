// The solve entry point.
//
// The solver is a stateless function, matching the shape of slvs.h: build a
// system, solve, read parameters back. Solver handles are transient per
// invocation and are regenerated every time; document identity is permanent and
// never leaks downward. The translation between the two is owned here and
// nowhere else.
//
// slvs.h reaches solve.cpp and no other translation unit in the project.
#pragma once

#include <cstdint>
#include <vector>

#include "core/solution.h"
#include "solve/context.h"

namespace paroculus {

struct SolveOptions {
    // Entities whose parameters the solver should favour leaving where they
    // are. This is the drag surface: the cursor's target goes in here and
    // everything else moves as little as the constraints allow.
    //
    // Note the asymmetry with a pin. Dragged is soft — the solver may still
    // move these — while ConstraintKind::Pin is the hard form the user asks for
    // explicitly, and it can over-constrain.
    std::vector<EntityId> dragged;

    // Constraints to include that are not in the document.
    //
    // This is how a candidate is tested before it exists. Speculative contexts
    // fork a component's parameters; this forks its constraint set, so an
    // offered constraint can be previewed or validated without the document
    // being touched at all — no copy to keep in sync, and no way for a preview
    // to leave a trace. Their IDs are reported in `failed` like any other.
    std::vector<ConstraintRecord> extra;

    // Computing the failing set costs roughly another solve, so it is opt-out
    // for the interactive path where only the pose matters.
    bool diagnoseFailures = true;

    // Carried from the first solve so the async path of stage 8 has nothing to
    // retrofit: stale results are discarded by comparing this, never by
    // guessing whether a reply is current.
    uint64_t generation = 0;
};

struct SolveOutcome {
    SolveStatus status = SolveStatus::Unsolved;
    int dof = -1;

    // The document constraints the solver blamed, mapped back from its own
    // handles. This is what the conflict highlight walks; raw solver handles
    // never surface.
    std::vector<ConstraintId> failed;

    uint64_t generation = 0;

    // Measured, because the frame budget is a measured thing and the solver is
    // its variable term. Excluded from every equality comparison.
    double microseconds = 0.0;

    // The translation half of that, measured separately. If building the system
    // ever comes to dominate warm solves, the response is a translation cache
    // keyed by component version — and this is the number that would say so.
    double translateMicroseconds = 0.0;

    // Bytes the translation scratch took from the arena, so bench can keep
    // translation overhead separate from solve time.
    size_t arenaBytes = 0;

    bool ok() const {
        return status == SolveStatus::Okay || status == SolveStatus::RedundantOkay;
    }
};

// Solves `context` in place against the declarations in `doc`.
//
// The document is never mutated: solved values land in the context, and writing
// them back as seeds is a separate command (SolveContext::commitCommands). That
// separation is what makes speculative previews safe by construction rather
// than by remembering not to save.
//
// Constraints are included when all their operands are members of the context,
// so a component solve carries exactly the relations that bind it.
SolveOutcome solve(const Document &doc, SolveContext &context,
                   const SolveOptions &options = {});

}  // namespace paroculus
