// Link-boundary canary: this translation unit links only paroculus-core and
// then reaches for the solver's header. It MUST NOT COMPILE.
//
// paroculus-solve links paroculus-solver PRIVATE, so the solver's include
// directory never propagates to a consumer. That is what turns "solve is the
// only layer that includes slvs.h" from a convention into a build error.
//
// CTest builds this target with WILL_FAIL. A green build here is the failure:
// it means the seam has been widened and slvs types can leak upward into the
// document model, taking the solver's allocator and C++ standard with them.
#include <slvs.h>

int main() { return SLVS_RESULT_OKAY; }
