#include "sketch.h"

#include <slvs.h>

#include <Eigen/Geometry>

#include "core/SkBitmap.h"
#include "core/SkCanvas.h"
#include "core/SkColor.h"
#include "core/SkImageInfo.h"
#include "core/SkPaint.h"
#include "core/SkPath.h"

#include <algorithm>
#include <vector>

namespace paroculus {
namespace {

// Handle plan. Group 1 is the fixed base (origin, normal, workplane); group 2
// is the sketch the solver is allowed to move.
enum : Slvs_hGroup { GROUP_BASE = 1, GROUP_SKETCH = 2 };
enum : Slvs_hEntity {
    E_ORIGIN = 101,
    E_NORMAL = 102,
    E_WORKPLANE = 103,
    E_PA0 = 201, E_PA1 = 202, E_PB0 = 203, E_PB1 = 204,
    E_LINE_A = 301, E_LINE_B = 302,
};

constexpr double SEGMENT_A_LENGTH = 120.0;

}  // namespace

bool Solution::ok() const {
    return result == SLVS_RESULT_OKAY || result == SLVS_RESULT_REDUNDANT_OKAY;
}

// Slvs_Solve mutates sys.param in place, so the solved values are read back
// out of the same array we seeded. Initial guesses are deliberately off-
// constraint (A is not horizontal, B is not parallel) so a successful solve
// proves the solver actually converged rather than accepting the seed.
Solution solveDemoSketch(double ratio) {
    if(!(ratio > 0.0)) ratio = 1.0;

    std::vector<Slvs_Param> params;
    std::vector<Slvs_Entity> entities;
    std::vector<Slvs_Constraint> constraints;

    // Base: origin at 0,0,0 and an identity-quaternion normal, giving the XY
    // workplane. Both live in GROUP_BASE, so the solver treats them as fixed.
    params.push_back(Slvs_MakeParam(1, GROUP_BASE, 0.0));
    params.push_back(Slvs_MakeParam(2, GROUP_BASE, 0.0));
    params.push_back(Slvs_MakeParam(3, GROUP_BASE, 0.0));
    entities.push_back(Slvs_MakePoint3d(E_ORIGIN, GROUP_BASE, 1, 2, 3));

    params.push_back(Slvs_MakeParam(4, GROUP_BASE, 1.0));
    params.push_back(Slvs_MakeParam(5, GROUP_BASE, 0.0));
    params.push_back(Slvs_MakeParam(6, GROUP_BASE, 0.0));
    params.push_back(Slvs_MakeParam(7, GROUP_BASE, 0.0));
    entities.push_back(Slvs_MakeNormal3d(E_NORMAL, GROUP_BASE, 4, 5, 6, 7));

    entities.push_back(Slvs_MakeWorkplane(E_WORKPLANE, GROUP_BASE, E_ORIGIN, E_NORMAL));

    // Sketch: four 2D points, two segments.
    const double seed[8] = {
        0.0,   0.0,     // A start  (anchored where it sits)
        100.0, 18.0,    // A end    (off-horizontal on purpose)
        0.0,  -60.0,    // B start  (anchored where it sits)
        80.0, -44.0,    // B end    (not parallel on purpose)
    };
    for(int i = 0; i < 8; i++) {
        params.push_back(Slvs_MakeParam(8 + i, GROUP_SKETCH, seed[i]));
    }
    entities.push_back(Slvs_MakePoint2d(E_PA0, GROUP_SKETCH, E_WORKPLANE, 8, 9));
    entities.push_back(Slvs_MakePoint2d(E_PA1, GROUP_SKETCH, E_WORKPLANE, 10, 11));
    entities.push_back(Slvs_MakePoint2d(E_PB0, GROUP_SKETCH, E_WORKPLANE, 12, 13));
    entities.push_back(Slvs_MakePoint2d(E_PB1, GROUP_SKETCH, E_WORKPLANE, 14, 15));
    entities.push_back(Slvs_MakeLineSegment(E_LINE_A, GROUP_SKETCH, E_WORKPLANE, E_PA0, E_PA1));
    entities.push_back(Slvs_MakeLineSegment(E_LINE_B, GROUP_SKETCH, E_WORKPLANE, E_PB0, E_PB1));

    // Eight parameters against eight equations: 1 + 1 + 1 + 1 + 2 + 2. A fully
    // constrained system, so a correct solve reports dof == 0.
    auto addConstraint = [&](Slvs_hConstraint h, int type, double valA,
                             Slvs_hEntity ptA, Slvs_hEntity ptB,
                             Slvs_hEntity entA, Slvs_hEntity entB) {
        constraints.push_back(Slvs_MakeConstraint(h, GROUP_SKETCH, type, E_WORKPLANE,
                                                  valA, ptA, ptB, entA, entB));
    };
    addConstraint(1, SLVS_C_HORIZONTAL,     0.0,              0,     0,     E_LINE_A, 0);
    addConstraint(2, SLVS_C_PT_PT_DISTANCE, SEGMENT_A_LENGTH, E_PA0, E_PA1, 0,        0);
    addConstraint(3, SLVS_C_PARALLEL,       0.0,              0,     0,     E_LINE_A, E_LINE_B);
    // valA is len(A)/len(B), per constrainteq.cpp's LENGTH_RATIO equation.
    addConstraint(4, SLVS_C_LENGTH_RATIO,   ratio,            0,     0,     E_LINE_A, E_LINE_B);
    addConstraint(5, SLVS_C_WHERE_DRAGGED,  0.0,              E_PA0, 0,     0,        0);
    addConstraint(6, SLVS_C_WHERE_DRAGGED,  0.0,              E_PB0, 0,     0,        0);

    std::vector<Slvs_hConstraint> failed(constraints.size());

    Slvs_System sys{};
    sys.param = params.data();
    sys.params = static_cast<int>(params.size());
    sys.entity = entities.data();
    sys.entities = static_cast<int>(entities.size());
    sys.constraint = constraints.data();
    sys.constraints = static_cast<int>(constraints.size());
    sys.failed = failed.data();
    sys.faileds = static_cast<int>(failed.size());
    sys.calculateFaileds = 1;

    Slvs_Solve(&sys, GROUP_SKETCH);

    Solution sln;
    sln.result = sys.result;
    sln.dof = sys.dof;
    // Sketch params start at handle 8, which is index 7 in the array.
    const Slvs_Param *p = params.data() + 7;
    sln.a0 = {p[0].val, p[1].val};
    sln.a1 = {p[2].val, p[3].val};
    sln.b0 = {p[4].val, p[5].val};
    sln.b1 = {p[6].val, p[7].val};
    return sln;
}

// Skia writes straight into the caller's buffer via installPixels, so there is
// no intermediate copy between here and the Qt image that wraps it.
void renderSketch(const Solution &sln, uint8_t *pixels, int width, int height,
                  size_t rowBytes) {
    if(width <= 0 || height <= 0 || pixels == nullptr) return;

    const SkImageInfo info =
        SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    SkBitmap bitmap;
    if(!bitmap.installPixels(info, pixels, rowBytes)) return;

    SkCanvas canvas(bitmap);
    canvas.clear(SkColorSetRGB(0x14, 0x16, 0x1a));

    // Sketch space is Y-up and roughly 200 units wide; fit it to the viewport
    // with a margin, centre it, and flip Y. Eigen owns the view transform so
    // the same matrix can later drive hit-testing and snapping.
    const double scale = std::min(width / 260.0, height / 200.0);
    Eigen::Affine2d view = Eigen::Affine2d::Identity();
    view.translate(Eigen::Vector2d(width * 0.5, height * 0.5));
    view.scale(Eigen::Vector2d(scale, -scale));
    view.translate(Eigen::Vector2d(-SEGMENT_A_LENGTH * 0.5, 30.0));

    auto toPixel = [&](const Point &p) {
        const Eigen::Vector2d v = view * Eigen::Vector2d(p.x, p.y);
        return SkPoint::Make(static_cast<SkScalar>(v.x()), static_cast<SkScalar>(v.y()));
    };

    // Grid, on 20-unit sketch centres.
    SkPaint grid;
    grid.setAntiAlias(false);
    grid.setColor(SkColorSetARGB(0x22, 0xff, 0xff, 0xff));
    grid.setStrokeWidth(1.0f);
    for(int i = -8; i <= 8; i++) {
        const SkPoint v0 = toPixel({i * 20.0, -120.0}), v1 = toPixel({i * 20.0, 120.0});
        const SkPoint h0 = toPixel({-160.0, i * 20.0}), h1 = toPixel({160.0, i * 20.0});
        canvas.drawLine(v0, v1, grid);
        canvas.drawLine(h0, h1, grid);
    }

    SkPaint stroke;
    stroke.setAntiAlias(true);
    stroke.setStyle(SkPaint::kStroke_Style);
    stroke.setStrokeWidth(3.0f);
    stroke.setStrokeCap(SkPaint::kRound_Cap);

    SkPaint dot;
    dot.setAntiAlias(true);
    dot.setStyle(SkPaint::kFill_Style);

    // A drives, B follows: colour-code the distinction.
    struct Seg { Point p0, p1; SkColor color; };
    const Seg segs[2] = {
        {sln.a0, sln.a1, SkColorSetRGB(0x6e, 0xc7, 0xff)},
        {sln.b0, sln.b1, SkColorSetRGB(0xff, 0xa8, 0x5c)},
    };
    for(const Seg &s : segs) {
        const SkPoint q0 = toPixel(s.p0), q1 = toPixel(s.p1);
        stroke.setColor(s.color);
        canvas.drawLine(q0, q1, stroke);
        dot.setColor(s.color);
        canvas.drawCircle(q0, 5.0f, dot);
        canvas.drawCircle(q1, 5.0f, dot);
    }
}

}  // namespace paroculus
