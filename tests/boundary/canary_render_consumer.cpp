// Link-boundary canary: this translation unit links paroculus-render — a legal
// consumer of the render layer — and then reaches for a Skia header. It MUST
// NOT COMPILE.
//
// paroculus-render links Skia PRIVATE, so Skia's include directory never
// propagates past the render layer. The core canary proves core cannot see
// Skia; this one guards the seam from the other side. A PRIVATE->PUBLIC flip on
// the Skia link would leak Skia's headers through every consumer of
// paroculus-render, up into the shell — and this canary would then compile.
//
// CTest builds this target with WILL_FAIL. A green build here is the failure.
#include <core/SkCanvas.h>

int main() { return sizeof(SkCanvas) > 0 ? 0 : 1; }
