// Link-boundary canary: this translation unit links only paroculus-core and
// then reaches for a Skia header. It MUST NOT COMPILE.
//
// paroculus-render links Skia PRIVATE, so no layer below or beside render sees
// Skia's include directory. That is what lets the QQuickPaintedItem shortcut be
// replaced by the Ganesh/RHI path (track R) without anything above the raster
// layer noticing.
//
// CTest builds this target with WILL_FAIL. A green build here is the failure.
#include <core/SkCanvas.h>

int main() { return sizeof(SkCanvas) > 0 ? 0 : 1; }
