#include "solve/demosketch.h"

#include <slvs.h>

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

// core/solution.h declares SolveStatus with these values so the mapping below
// is a cast. If SolveSpace ever renumbers, this breaks the build here rather
// than silently mislabelling every diagnostic above the seam.
static_assert(static_cast<int>(SolveStatus::Okay) == SLVS_RESULT_OKAY);
static_assert(static_cast<int>(SolveStatus::Inconsistent) == SLVS_RESULT_INCONSISTENT);
static_assert(static_cast<int>(SolveStatus::DidNotConverge) == SLVS_RESULT_DIDNT_CONVERGE);
static_assert(static_cast<int>(SolveStatus::TooManyUnknowns) == SLVS_RESULT_TOO_MANY_UNKNOWNS);
static_assert(static_cast<int>(SolveStatus::RedundantOkay) == SLVS_RESULT_REDUNDANT_OKAY);

}  // namespace

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
    sln.status = static_cast<SolveStatus>(sys.result);
    sln.dof = sys.dof;
    // Sketch params start at handle 8, which is index 7 in the array.
    const Slvs_Param *p = params.data() + 7;
    sln.a0 = {p[0].val, p[1].val};
    sln.a1 = {p[2].val, p[3].val};
    sln.b0 = {p[4].val, p[5].val};
    sln.b1 = {p[6].val, p[7].val};
    return sln;
}

}  // namespace paroculus
