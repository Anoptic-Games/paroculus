// The raster layer's stage-0 surface: paint solved geometry into a caller-owned
// buffer. Skia reaches this layer only, linked PRIVATE, so no Skia type appears
// in any header above render — which is what lets the known QQuickPaintedItem
// shortcut be swapped for the GPU path without anything above noticing.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/geom.h"
#include "core/solution.h"

namespace paroculus {

// The stage-0 framing policy: fit the demo sketch's content into a viewport
// with a margin, centred, Y flipped. Stage 3 replaces this with pan/zoom view
// state owned by the shell; it is exposed now because hit testing and the
// raster must agree on one transform, and a test can assert that agreement.
// width, height: viewport pixels. Both must be > 0.
ViewTransform fitView(int width, int height);

// Rasterises a Solution into a caller-owned BGRA8888 premultiplied buffer.
// pixels must have at least height*rowBytes bytes; rowBytes at least 4*width.
// No-ops on a degenerate viewport or a null buffer.
void renderSketch(const Solution &sln, uint8_t *pixels, int width, int height,
                  size_t rowBytes);

}  // namespace paroculus
