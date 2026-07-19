#include "render/view.h"

#include "core/SkBitmap.h"
#include "core/SkCanvas.h"
#include "core/SkColor.h"
#include "core/SkImageInfo.h"
#include "core/SkPaint.h"
#include "core/SkPath.h"

#include <algorithm>
#include <cmath>

namespace paroculus {
namespace {

constexpr SkColor BACKGROUND = SkColorSetRGB(0x14, 0x16, 0x1a);
constexpr SkColor GRID = SkColorSetARGB(0x22, 0xff, 0xff, 0xff);
constexpr SkColor GEOMETRY = SkColorSetRGB(0x6e, 0xc7, 0xff);
constexpr SkColor CONSTRUCTION = SkColorSetARGB(0x99, 0x8a, 0x93, 0xa6);
constexpr SkColor SELECTED = SkColorSetRGB(0xff, 0xa8, 0x5c);
constexpr SkColor HOVERED = SkColorSetRGB(0xff, 0xe0, 0xa0);
constexpr SkColor RESISTING = SkColorSetRGB(0xff, 0x5c, 0x5c);
constexpr SkColor MARQUEE = SkColorSetARGB(0x40, 0x6e, 0xc7, 0xff);

// Adorners are screen-space and do not scale with zoom: a handle is a handle at
// every magnification, which is also what makes the pixel hit radii honest.
constexpr float POINT_RADIUS = 4.0f;
constexpr float SELECTED_POINT_RADIUS = 6.0f;
constexpr float STROKE_WIDTH = 2.0f;
constexpr float SELECTED_STROKE_WIDTH = 3.0f;

constexpr double CONTENT_WIDTH = 120.0;

bool contains(const std::vector<EntityId> &haystack, EntityId needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

}  // namespace

ViewTransform defaultView(int width, int height) {
    const double scale = std::min(width / 260.0, height / 200.0);
    Eigen::Affine2d view = Eigen::Affine2d::Identity();
    view.translate(Eigen::Vector2d(width * 0.5, height * 0.5));
    view.scale(Eigen::Vector2d(scale, -scale));
    view.translate(Eigen::Vector2d(-CONTENT_WIDTH * 0.5, 30.0));
    return ViewTransform(view);
}

ViewTransform fitView(const Pose &pose, int width, int height) {
    double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
    bool any = false;
    for(const EntityRecord &e : pose.document().entities().records()) {
        const std::optional<Point> p = pose.point(e.id);
        if(!p) continue;
        if(!any) {
            minX = maxX = p->x;
            minY = maxY = p->y;
            any = true;
            continue;
        }
        minX = std::min(minX, p->x);
        maxX = std::max(maxX, p->x);
        minY = std::min(minY, p->y);
        maxY = std::max(maxY, p->y);
    }
    // A document that places nothing still needs a sensible view.
    if(!any) return defaultView(width, height);

    const double contentWidth = std::max(maxX - minX, 1.0);
    const double contentHeight = std::max(maxY - minY, 1.0);
    const double scale = std::min(width / (contentWidth * 1.3), height / (contentHeight * 1.3));

    Eigen::Affine2d view = Eigen::Affine2d::Identity();
    view.translate(Eigen::Vector2d(width * 0.5, height * 0.5));
    view.scale(Eigen::Vector2d(scale, -scale));
    view.translate(Eigen::Vector2d(-(minX + maxX) * 0.5, -(minY + maxY) * 0.5));
    return ViewTransform(view);
}

void renderDocument(const Pose &pose, const ViewTransform &view, const Adornment &adornment,
                    uint8_t *pixels, int width, int height, size_t rowBytes,
                    double deviceScale) {
    if(width <= 0 || height <= 0 || pixels == nullptr) return;
    if(!(deviceScale > 0.0)) return;

    const SkImageInfo info =
        SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
    SkBitmap bitmap;
    if(!bitmap.installPixels(info, pixels, rowBytes)) return;

    SkCanvas canvas(bitmap);
    canvas.clear(BACKGROUND);

    // One canvas scale carries the whole device-pixel story. Everything below
    // draws in logical pixels — including the cosmetic constants, which is what
    // keeps a 2 px stroke two *logical* pixels on a HiDPI panel rather than a
    // hairline. Nothing above this line needs to know the ratio exists.
    canvas.scale(static_cast<SkScalar>(deviceScale), static_cast<SkScalar>(deviceScale));
    const double logicalWidth = width / deviceScale;
    const double logicalHeight = height / deviceScale;

    auto toPixel = [&](const Point &p) {
        const Eigen::Vector2d v = view.toScreen(p);
        return SkPoint::Make(static_cast<SkScalar>(v.x()), static_cast<SkScalar>(v.y()));
    };

    // Grid on 20-unit document centres, clipped to whatever the view shows.
    SkPaint grid;
    grid.setAntiAlias(false);
    grid.setColor(GRID);
    grid.setStrokeWidth(1.0f);
    const Point topLeft = view.toDocument(Eigen::Vector2d(0.0, 0.0));
    const Point bottomRight = view.toDocument(Eigen::Vector2d(logicalWidth, logicalHeight));
    const double gridStep = 20.0;
    const double lowX = std::min(topLeft.x, bottomRight.x);
    const double highX = std::max(topLeft.x, bottomRight.x);
    const double lowY = std::min(topLeft.y, bottomRight.y);
    const double highY = std::max(topLeft.y, bottomRight.y);
    // Bounded so a zoomed-far-out view does not try to draw a million lines.
    if((highX - lowX) / gridStep < 400.0) {
        for(double x = std::floor(lowX / gridStep) * gridStep; x <= highX; x += gridStep) {
            canvas.drawLine(toPixel({x, lowY}), toPixel({x, highY}), grid);
        }
        for(double y = std::floor(lowY / gridStep) * gridStep; y <= highY; y += gridStep) {
            canvas.drawLine(toPixel({lowX, y}), toPixel({highX, y}), grid);
        }
    }

    // Every entity that resists a saturated drag tints, so resistance has
    // attribution on screen and not only in a readout.
    std::vector<EntityId> resistingEntities;
    for(ConstraintId id : adornment.resisting) {
        const ConstraintRecord *c = pose.document().constraints().find(id);
        if(c == nullptr) continue;
        for(size_t i = 0; i < constraintInfo(c->kind).operandCount; i++) {
            resistingEntities.push_back(c->operands[i]);
        }
    }

    SkPaint stroke;
    stroke.setAntiAlias(true);
    stroke.setStyle(SkPaint::kStroke_Style);
    stroke.setStrokeCap(SkPaint::kRound_Cap);

    // Edges first, so vertices sit on top of what they join — which matches
    // the hit priority that puts points above edges.
    for(const EntityRecord &e : pose.document().entities().records()) {
        const auto ends = pose.segment(e.id);
        if(!ends) continue;

        const bool isSelected = contains(adornment.selected, e.id);
        const bool isResisting = contains(resistingEntities, e.id);
        SkColor colour = e.role == Role::Construction ? CONSTRUCTION : GEOMETRY;
        if(isSelected) colour = SELECTED;
        if(adornment.hovered == e.id) colour = HOVERED;
        if(isResisting) colour = RESISTING;

        stroke.setColor(colour);
        stroke.setStrokeWidth(isSelected ? SELECTED_STROKE_WIDTH : STROKE_WIDTH);
        // Construction geometry reads as a guide: dashed would be better and is
        // a stage 6 styling concern; for now it is thinner and dimmer.
        if(e.role == Role::Construction) stroke.setStrokeWidth(1.0f);
        canvas.drawLine(toPixel(ends->first), toPixel(ends->second), stroke);
    }

    // Circles.
    for(const EntityRecord &e : pose.document().entities().records()) {
        const std::optional<double> radius = pose.radius(e.id);
        if(!radius) continue;
        const std::optional<Point> centre = pose.point(e.points[0]);
        if(!centre) continue;

        const bool isSelected = contains(adornment.selected, e.id);
        stroke.setColor(isSelected ? SELECTED
                                   : (e.role == Role::Construction ? CONSTRUCTION : GEOMETRY));
        stroke.setStrokeWidth(isSelected ? SELECTED_STROKE_WIDTH : STROKE_WIDTH);
        // The radius is a document length, so it scales with the view.
        const Eigen::Vector2d edge = view.toScreen(Point{centre->x + *radius, centre->y});
        const Eigen::Vector2d middle = view.toScreen(*centre);
        canvas.drawCircle(toPixel(*centre), static_cast<SkScalar>((edge - middle).norm()),
                          stroke);
    }

    // Vertices, in screen space at a fixed size.
    SkPaint dot;
    dot.setAntiAlias(true);
    dot.setStyle(SkPaint::kFill_Style);
    for(const EntityRecord &e : pose.document().entities().records()) {
        const std::optional<Point> p = pose.point(e.id);
        if(!p) continue;

        const bool isSelected = contains(adornment.selected, e.id);
        const bool isResisting = contains(resistingEntities, e.id);
        SkColor colour = e.role == Role::Construction ? CONSTRUCTION : GEOMETRY;
        if(isSelected) colour = SELECTED;
        if(adornment.hovered == e.id) colour = HOVERED;
        if(isResisting) colour = RESISTING;

        dot.setColor(colour);
        canvas.drawCircle(toPixel(*p), isSelected ? SELECTED_POINT_RADIUS : POINT_RADIUS, dot);
    }

    if(adornment.marqueeActive) {
        SkPaint fill;
        fill.setAntiAlias(false);
        fill.setStyle(SkPaint::kFill_Style);
        fill.setColor(MARQUEE);
        const SkRect rect = SkRect::MakeLTRB(
            static_cast<SkScalar>(std::min(adornment.marqueeFrom.x(), adornment.marqueeTo.x())),
            static_cast<SkScalar>(std::min(adornment.marqueeFrom.y(), adornment.marqueeTo.y())),
            static_cast<SkScalar>(std::max(adornment.marqueeFrom.x(), adornment.marqueeTo.x())),
            static_cast<SkScalar>(std::max(adornment.marqueeFrom.y(), adornment.marqueeTo.y())));
        canvas.drawRect(rect, fill);
    }
}

}  // namespace paroculus
