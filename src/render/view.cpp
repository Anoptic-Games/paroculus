#include "render/view.h"

#include "core/SkBitmap.h"
#include "core/SkCanvas.h"
#include "core/SkColor.h"
#include "core/SkImageInfo.h"
#include "core/SkPaint.h"
#include "core/SkPath.h"

#include <algorithm>

namespace paroculus {
namespace {

// Nominal content width of the demo sketch, used only for framing. It happens
// to equal segment A's constrained length; render does not know that and must
// not — stage 3 derives the frame from document bounds and view state instead.
constexpr double CONTENT_WIDTH = 120.0;

}  // namespace

ViewTransform fitView(int width, int height) {
    const double scale = std::min(width / 260.0, height / 200.0);
    Eigen::Affine2d view = Eigen::Affine2d::Identity();
    view.translate(Eigen::Vector2d(width * 0.5, height * 0.5));
    view.scale(Eigen::Vector2d(scale, -scale));
    view.translate(Eigen::Vector2d(-CONTENT_WIDTH * 0.5, 30.0));
    return ViewTransform(view);
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

    const ViewTransform view = fitView(width, height);
    auto toPixel = [&](const Point &p) {
        const Eigen::Vector2d v = view.toScreen(p);
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
