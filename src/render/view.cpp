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
// The ghost reads as the geometry it will become, dimmed — not as a different
// kind of thing. What the user sees mid-gesture is what commit produces.
constexpr SkColor GHOST = SkColorSetARGB(0x99, 0x6e, 0xc7, 0xff);

// Adorners are screen-space and do not scale with zoom: a handle is a handle at
// every magnification, which is also what makes the pixel hit radii honest.
constexpr float POINT_RADIUS = 4.0f;
constexpr float SELECTED_POINT_RADIUS = 6.0f;
constexpr float STROKE_WIDTH = 2.0f;
constexpr float SELECTED_STROKE_WIDTH = 3.0f;

constexpr SkColor GLYPH = SkColorSetARGB(0xcc, 0x9a, 0xb0, 0xc8);
constexpr SkColor GLYPH_FRESH = SkColorSetRGB(0x7c, 0xe0, 0xa8);
constexpr SkColor GLYPH_GHOST = SkColorSetARGB(0x88, 0x7c, 0xe0, 0xa8);

// Adorner sizes, in logical pixels: a glyph is a glyph at every magnification.
constexpr float GLYPH_SIZE = 4.5f;
constexpr float GLYPH_STROKE = 1.6f;
// Marks sit clear of the geometry they annotate rather than on top of it.
constexpr float GLYPH_OFFSET = 9.0f;

constexpr double CONTENT_WIDTH = 120.0;

// One distinct mark per constraint kind, drawn as strokes rather than text
// because no typeface is bundled yet — dimension text arrives with the styling
// work. The shapes follow drafting convention where there is one, so the marks
// are readable before they are learned.
void drawGlyph(SkCanvas &canvas, SkPaint &paint, ConstraintKind kind, SkPoint at) {
    const float s = GLYPH_SIZE;
    switch(kind) {
        case ConstraintKind::Horizontal:
            canvas.drawLine({at.fX - s, at.fY}, {at.fX + s, at.fY}, paint);
            break;
        case ConstraintKind::Vertical:
            canvas.drawLine({at.fX, at.fY - s}, {at.fX, at.fY + s}, paint);
            break;
        case ConstraintKind::Parallel:
            // Two strokes leaning the same way.
            canvas.drawLine({at.fX - s * 0.9f, at.fY + s}, {at.fX - s * 0.1f, at.fY - s}, paint);
            canvas.drawLine({at.fX + s * 0.1f, at.fY + s}, {at.fX + s * 0.9f, at.fY - s}, paint);
            break;
        case ConstraintKind::Perpendicular:
            canvas.drawLine({at.fX - s, at.fY + s}, {at.fX + s, at.fY + s}, paint);
            canvas.drawLine({at.fX, at.fY + s}, {at.fX, at.fY - s}, paint);
            break;
        case ConstraintKind::Coincident:
            canvas.drawCircle(at, s * 0.8f, paint);
            break;
        case ConstraintKind::Midpoint:
            canvas.drawLine({at.fX - s, at.fY + s * 0.7f}, {at.fX + s, at.fY + s * 0.7f}, paint);
            canvas.drawLine({at.fX, at.fY + s * 0.7f}, {at.fX, at.fY - s * 0.7f}, paint);
            canvas.drawLine({at.fX - s * 0.5f, at.fY - s * 0.7f},
                            {at.fX + s * 0.5f, at.fY - s * 0.7f}, paint);
            break;
        case ConstraintKind::PointOnLine:
        case ConstraintKind::PointOnCircle:
            canvas.drawLine({at.fX - s, at.fY + s * 0.8f}, {at.fX + s, at.fY + s * 0.8f}, paint);
            canvas.drawCircle({at.fX, at.fY - s * 0.4f}, s * 0.45f, paint);
            break;
        case ConstraintKind::Pin:
            canvas.drawLine({at.fX - s, at.fY - s}, {at.fX + s, at.fY + s}, paint);
            canvas.drawLine({at.fX - s, at.fY + s}, {at.fX + s, at.fY - s}, paint);
            break;
        default:
            // Every constraint has a mark, including the ones without a
            // convention yet: an unlabelled mark is still reachable, and an
            // absent one is an invisible constraint.
            canvas.drawCircle(at, s * 0.5f, paint);
            break;
    }
}

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
        for(size_t i = 0; i < boundOperandCount(*c); i++) {
            resistingEntities.push_back(c->operands[i]);
        }
    }

    // How an entity is tinted, in one place for every kind that draws. Written
    // out per loop it was written out four times and one copy drifted: circles
    // took no hover and no resistance, so a circle resisting a saturated drag
    // was attributed everywhere except on itself. Precedence runs
    // role, then selection, then hover, then resistance — the transient states
    // over the persistent ones, and resistance last because it is the answer to
    // a question the user is asking right now.
    auto tintOf = [&](const EntityRecord &e) {
        SkColor colour = e.role == Role::Construction ? CONSTRUCTION : GEOMETRY;
        if(contains(adornment.selected, e.id)) colour = SELECTED;
        if(adornment.hovered == e.id) colour = HOVERED;
        if(contains(resistingEntities, e.id)) colour = RESISTING;
        return colour;
    };

    // Construction geometry reads as a guide: dashed would be better and is a
    // stage 6 styling concern; for now it is thinner and dimmer.
    auto strokeWidthOf = [&](const EntityRecord &e) {
        if(e.role == Role::Construction) return 1.0f;
        return contains(adornment.selected, e.id) ? SELECTED_STROKE_WIDTH : STROKE_WIDTH;
    };

    SkPaint stroke;
    stroke.setAntiAlias(true);
    stroke.setStyle(SkPaint::kStroke_Style);
    stroke.setStrokeCap(SkPaint::kRound_Cap);

    // Edges first, so vertices sit on top of what they join — which matches
    // the hit priority that puts points above edges.
    for(const EntityRecord &e : pose.document().entities().records()) {
        const auto ends = pose.segment(e.id);
        if(!ends) continue;

        stroke.setColor(tintOf(e));
        stroke.setStrokeWidth(strokeWidthOf(e));
        canvas.drawLine(toPixel(ends->first), toPixel(ends->second), stroke);
    }

    // Arcs, tessellated in document space rather than handed to Skia as an
    // angular sweep. Everything else here goes through toPixel, and an arc
    // drawn by angle would have to undo the view's Y flip by hand — a second
    // way of converting is a second way of being wrong about zoom.
    for(const EntityRecord &e : pose.document().entities().records()) {
        const std::optional<Pose::ArcGeometry> g = pose.arc(e.id);
        if(!g) continue;

        stroke.setColor(tintOf(e));
        stroke.setStrokeWidth(strokeWidthOf(e));

        // Enough segments that the flat spots are under a pixel at this zoom,
        // bounded so a huge arc does not cost a thousand line calls.
        const double onScreen =
            (view.toScreen(Point{g->centre.x + g->radius, g->centre.y}) -
             view.toScreen(g->centre))
                .norm();
        const int steps = std::clamp(static_cast<int>(onScreen * g->sweep * 0.5), 8, 240);
        SkPoint previous = toPixel(Point{g->centre.x + g->radius * std::cos(g->startAngle),
                                         g->centre.y + g->radius * std::sin(g->startAngle)});
        for(int i = 1; i <= steps; i++) {
            const double angle = g->startAngle + g->sweep * i / steps;
            const SkPoint next = toPixel(Point{g->centre.x + g->radius * std::cos(angle),
                                               g->centre.y + g->radius * std::sin(angle)});
            canvas.drawLine(previous, next, stroke);
            previous = next;
        }
    }

    // Circles.
    for(const EntityRecord &e : pose.document().entities().records()) {
        const std::optional<double> radius = pose.radius(e.id);
        if(!radius) continue;
        const std::optional<Point> centre = pose.point(e.points[0]);
        if(!centre) continue;

        stroke.setColor(tintOf(e));
        stroke.setStrokeWidth(strokeWidthOf(e));
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

        dot.setColor(tintOf(e));
        canvas.drawCircle(toPixel(*p),
                          contains(adornment.selected, e.id) ? SELECTED_POINT_RADIUS
                                                             : POINT_RADIUS,
                          dot);
    }

    // The tool's rubber band, over the geometry and under the marquee. Drawn
    // last among document-space things so it is never hidden by what it will
    // sit alongside.
    if(adornment.ghostActive) {
        stroke.setColor(GHOST);
        stroke.setStrokeWidth(STROKE_WIDTH);
        dot.setColor(GHOST);

        auto ghostArc = [&](Point centre, double radius, double from, double sweep) {
            const double onScreen =
                (view.toScreen(Point{centre.x + radius, centre.y}) - view.toScreen(centre))
                    .norm();
            const int steps = std::clamp(static_cast<int>(onScreen * std::abs(sweep) * 0.5),
                                         8, 240);
            SkPoint previous = toPixel(Point{centre.x + radius * std::cos(from),
                                             centre.y + radius * std::sin(from)});
            for(int i = 1; i <= steps; i++) {
                const double angle = from + sweep * i / steps;
                const SkPoint next = toPixel(Point{centre.x + radius * std::cos(angle),
                                                   centre.y + radius * std::sin(angle)});
                canvas.drawLine(previous, next, stroke);
                previous = next;
            }
        };

        switch(adornment.ghostShape) {
            case Adornment::GhostShape::Circle: {
                const double radius = std::hypot(adornment.ghostTo.x - adornment.ghostFrom.x,
                                                 adornment.ghostTo.y - adornment.ghostFrom.y);
                ghostArc(adornment.ghostFrom, radius, 0.0, 6.283185307179586);
                canvas.drawCircle(toPixel(adornment.ghostFrom), POINT_RADIUS, dot);
                break;
            }
            case Adornment::GhostShape::Rectangle: {
                const Point a = adornment.ghostFrom;
                const Point b = adornment.ghostTo;
                const Point corners[4] = {a, Point{b.x, a.y}, b, Point{a.x, b.y}};
                for(int i = 0; i < 4; i++) {
                    canvas.drawLine(toPixel(corners[i]), toPixel(corners[(i + 1) % 4]), stroke);
                }
                for(const Point &c : corners) canvas.drawCircle(toPixel(c), POINT_RADIUS, dot);
                break;
            }
            case Adornment::GhostShape::Arc:
                ghostArc(adornment.ghostCentre, adornment.ghostRadius, adornment.ghostStart,
                         adornment.ghostSweep);
                canvas.drawCircle(toPixel(adornment.ghostFrom), POINT_RADIUS, dot);
                canvas.drawCircle(toPixel(adornment.ghostTo), POINT_RADIUS, dot);
                break;
            case Adornment::GhostShape::Line:
                canvas.drawLine(toPixel(adornment.ghostFrom), toPixel(adornment.ghostTo), stroke);
                canvas.drawCircle(toPixel(adornment.ghostFrom), POINT_RADIUS, dot);
                canvas.drawCircle(toPixel(adornment.ghostTo), POINT_RADIUS, dot);
                break;
        }
    }

    // Constraint marks, above the geometry they annotate. The set arrived
    // already chosen; drawing does not second-guess it.
    if(!adornment.glyphs.empty()) {
        SkPaint glyph;
        glyph.setAntiAlias(true);
        glyph.setStyle(SkPaint::kStroke_Style);
        glyph.setStrokeCap(SkPaint::kRound_Cap);
        glyph.setStrokeWidth(GLYPH_STROKE);

        // Marks on the same anchor would otherwise stack invisibly, so they fan
        // out around it — a vertex with three relations has to read as three.
        std::vector<std::pair<SkPoint, int>> occupied;
        for(const GlyphMark &m : adornment.glyphs) {
            const SkPoint base = toPixel(m.anchor);
            int index = 0;
            for(auto &[p, n] : occupied) {
                if(std::hypot(p.fX - base.fX, p.fY - base.fY) < 0.5f) {
                    index = ++n;
                    break;
                }
            }
            if(index == 0) occupied.emplace_back(base, 0);

            // Around the anchor rather than on it, so the mark never hides the
            // vertex or edge it is describing.
            const double angle = -1.0 + static_cast<double>(index) * 1.3;
            const SkPoint at{base.fX + static_cast<SkScalar>(std::cos(angle) * GLYPH_OFFSET),
                             base.fY + static_cast<SkScalar>(std::sin(angle) * GLYPH_OFFSET)};

            glyph.setColor(m.ghost ? GLYPH_GHOST : (m.fresh ? GLYPH_FRESH : GLYPH));
            if(m.selected || m.hovered) glyph.setColor(SELECTED);
            drawGlyph(canvas, glyph, m.kind, at);
        }
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
