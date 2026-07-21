// SVG as the outside world speaks it: export is a bake, import is a trace.
//
// Export is the bake carried the last mile. bakeForExport flattens the live
// document to rings and strokes and counts what it lost; this turns that value
// into a file. The region algebra that has no SVG equivalent is expressed
// structurally — a subtract becomes a mask, an intersect a clip, an alpha
// overwrite a mask over its layer — rather than resolved by a polygon boolean
// core has no business growing. What SVG cannot say, this says with masks and
// clip paths, which is the honest reading of "regions to paths, alpha overwrite
// to masks".
//
// Import is a trace, and deliberately less. Geometry arrives free and
// unconstrained — points, segments, circles — and inference-on-import is a later
// feature with the same taxonomy, not a second inference system bolted on here.
// The subset is the straight-line and circle vocabulary a layout drawing uses;
// anything outside it is counted skipped rather than guessed at.
//
// Both directions share one coordinate convention: document y is up and SVG y is
// down, so the two are negatives of each other. Applied on the way out and
// undone on the way in, an export re-imported lands where it started.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "core/bake.h"
#include "core/document.h"
#include "core/pose.h"

namespace paroculus {

struct SvgOptions {
    // Document units of padding around the drawing's bounding box.
    double margin = 8.0;
    // Decimal places for coordinates. A bake is lossy and the loss it is allowed
    // is precision; four places puts the chord error well under what a vector
    // consumer resolves.
    int precision = 4;
};

// The baked drawing as a standalone SVG document.
std::string writeSvg(const Bake &bake, const SvgOptions &options = {});

// Bakes the visible document at `pose` and writes it. The convenience the
// session reaches for; the two-argument form is what a test drives to assert the
// structure without a pose of its own.
std::string writeSvg(const Document &doc, const Pose &pose, const SvgOptions &options = {});

// What a trace produced: the unconstrained document, plus an honest count of the
// elements it could turn into geometry and the ones it could not.
struct SvgImport {
    Document document;
    size_t traced = 0;    // elements that became geometry
    size_t skipped = 0;   // elements outside the supported subset
};

// Traces the geometry out of an SVG string. Lines, polylines, polygons, rects
// and circles become points, segments and circles; straight-line paths (M, L, H,
// V, Z, with implicit command repetition) become the same. Curved path commands,
// transforms and everything else are skipped and counted. No constraint is
// imposed and no inference is run.
//
// Two honest limits of a lossy trace. Path coordinates are read in plain and
// exponent-free notation — the form our own export writes — so a path using
// scientific notation is skipped whole rather than half-read. And because the
// trace is unconstrained, an exported filled region — which reaches the file as
// both its fill path and the stroked polylines of its boundary — re-imports as
// two overlapping copies of that boundary; it round-trips its coordinates, not
// its record count.
SvgImport readSvg(std::string_view svg);

}  // namespace paroculus
