// Link-boundary canary: this translation unit links paroculus-solve — a legal
// consumer of the solve layer — and then reaches for the solver's header. It
// MUST NOT COMPILE.
//
// paroculus-solve links paroculus-solver PRIVATE, so the solver's include
// directory never propagates past the solve layer. The core canary proves core
// cannot see slvs.h; this one guards the seam from the other side. A
// PRIVATE->PUBLIC flip on paroculus-solver would leak slvs.h through every
// consumer of paroculus-solve, up into interact and shell, taking the solver's
// allocator and C++ standard with it — and this canary would then compile.
//
// CTest builds this target with WILL_FAIL. A green build here is the failure.
#include <slvs.h>

int main() { return SLVS_RESULT_OKAY; }
