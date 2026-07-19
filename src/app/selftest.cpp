#include "app/selftest.h"

#include <QImage>

#include <cmath>
#include <cstdio>

#include "core/pose.h"
#include "interact/policies.h"
#include "render/view.h"
#include "solve/demosketch.h"
#include "solve/solve.h"

namespace paroculus {

// Checks the solved geometry against the constraints that were declared, not
// merely that the solver returned OKAY. A solver that no-ops and echoes the
// seed values back would pass the status check but fail every assertion here.
// This is the pattern the stage-2 semantics suite generalises over the whole
// constraint catalogue: assert residuals, never trust a status code.
int selftest() {
    constexpr double RATIO = 1.618;

    const Solution s = solveDemoSketch(RATIO);
    std::printf("solver result=%d dof=%d\n", static_cast<int>(s.status), s.dof);
    if(!s.ok()) {
        std::fprintf(stderr, "FAIL: solver did not converge\n");
        return 1;
    }
    if(s.dof != 0) {
        std::fprintf(stderr, "FAIL: expected 0 dof, got %d\n", s.dof);
        return 1;
    }

    const double ax = s.a1.x - s.a0.x, ay = s.a1.y - s.a0.y;
    const double bx = s.b1.x - s.b0.x, by = s.b1.y - s.b0.y;
    const double lenA = std::hypot(ax, ay), lenB = std::hypot(bx, by);

    std::printf("A=(%.4f,%.4f)->(%.4f,%.4f) |A|=%.6f\n", s.a0.x, s.a0.y, s.a1.x, s.a1.y, lenA);
    std::printf("B=(%.4f,%.4f)->(%.4f,%.4f) |B|=%.6f\n", s.b0.x, s.b0.y, s.b1.x, s.b1.y, lenB);

    struct Check { const char *what; double got; double want; double tol; };
    const Check checks[] = {
        {"|A| == 120",            lenA,               120.0, 1e-6},
        {"A horizontal",          s.a1.y - s.a0.y,    0.0,   1e-6},
        {"B parallel to A",       ax * by - ay * bx,  0.0,   1e-5},
        {"len(A)/len(B) == ratio", lenA / lenB,       RATIO, 1e-6},
    };
    for(const Check &c : checks) {
        if(std::fabs(c.got - c.want) > c.tol) {
            std::fprintf(stderr, "FAIL: %s — got %.9f, want %.9f\n", c.what, c.got, c.want);
            return 1;
        }
        std::printf("  ok: %s\n", c.what);
    }

    // Render and confirm Skia actually marked the surface. A silently failing
    // installPixels would leave the buffer at its fill value.
    //
    // The document is what gets drawn, through the same painter the shell uses,
    // so this also proves the render path the user sees rather than a parallel
    // one kept alive for the test.
    const Document doc = demoDocument(RATIO);
    Pose pose(doc);
    SolveContext context = SolveContext::forWholeDocument(doc);
    solve(doc, context);
    pose.overlay(context.params());

    const int W = 400, H = 300;
    QImage surface(W, H, QImage::Format_ARGB32_Premultiplied);
    surface.fill(Qt::transparent);
    // The grid step comes from the snap policy here for the same reason it does
    // in the shell: there is one number, and this is meant to be the shell's
    // painter rather than a parallel one.
    Adornment adornment;
    adornment.gridStep = SnapPolicy{}.gridStep;
    renderDocument(pose, defaultView(W, H), adornment, surface.bits(), W, H,
                   static_cast<size_t>(surface.bytesPerLine()));

    const QRgb background = surface.pixel(2, 2);
    int painted = 0;
    for(int y = 0; y < H; y++) {
        for(int x = 0; x < W; x++) {
            if(surface.pixel(x, y) != background) painted++;
        }
    }
    std::printf("skia: background=%08x painted=%d of %d px\n", background, painted, W * H);
    if(background == 0u) {
        std::fprintf(stderr, "FAIL: skia never cleared the surface\n");
        return 1;
    }
    if(painted < 1000) {
        std::fprintf(stderr, "FAIL: skia drew almost nothing (%d px)\n", painted);
        return 1;
    }

    std::printf("selftest OK — solvespace, eigen, skia and qt all live\n");
    return 0;
}

}  // namespace paroculus
