// The solve layer's stage-0 surface: build the demo sketch, solve it, hand back
// solved state in core types. Stage 2 replaces this with document-component ->
// Slvs_System translation; the seam it establishes now is the permanent one.
//
// slvs.h reaches exactly one translation unit in the project, below this header.
// The solver is linked PRIVATE, so an include from any other layer is a build
// error rather than a convention — see tests/boundary.
#pragma once

#include "core/solution.h"

namespace paroculus {

// ratio: len(A)/len(B). Must be > 0; non-positive falls back to 1.
// Returns solved geometry. On failure the points hold the last iterate and
// status carries the mapped solver code, because a document under a failed
// solve stays editable and holds its last pose.
Solution solveDemoSketch(double ratio);

}  // namespace paroculus
