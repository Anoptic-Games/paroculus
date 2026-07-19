// Sketch model, constraint solve, and raster output. Deliberately Qt-free: the
// document and its rendering must not depend on the UI toolkit.
#pragma once

#include <cstddef>
#include <cstdint>

namespace paroculus {

struct Point {
    double x = 0.0;
    double y = 0.0;
};

// A solved two-segment sketch. Segment A is driven horizontal at a fixed
// length; segment B is held parallel to A with len(A)/len(B) pinned to a
// ratio. Parallelism and proportion as primitives, not derived results.
struct Solution {
    int result = -1;  // SLVS_RESULT_*
    int dof = -1;     // remaining degrees of freedom; 0 == fully constrained
    Point a0, a1;     // segment A endpoints
    Point b0, b1;     // segment B endpoints

    bool ok() const;
};

// Builds the demo sketch and solves group 2 against the fixed base workplane.
// ratio: len(A)/len(B), must be > 0.
// Returns the solved geometry; on solver failure the points hold the last
// iterate and result carries the SLVS_RESULT_* code.
Solution solveDemoSketch(double ratio);

// Rasterises a Solution with Skia into a caller-owned BGRA8888 premultiplied
// buffer. pixels must have at least height*rowBytes bytes.
// The sketch->pixel mapping is an Eigen affine: centre the origin, flip Y so
// sketch space is Y-up, and fit the content with a margin.
void renderSketch(const Solution &sln, uint8_t *pixels, int width, int height,
                  size_t rowBytes);

}  // namespace paroculus
