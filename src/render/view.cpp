#include "render/view.h"

#include "core/composition.h"
#include "core/measure.h"
#include "core/tags.h"
#include "render/typeface.h"

#include "core/SkBitmap.h"
#include "core/SkCanvas.h"
#include "core/SkColor.h"
#include "core/SkImageInfo.h"
#include "core/SkPaint.h"
#include "core/SkPath.h"
#include "core/SkPathBuilder.h"
#include "core/SkFont.h"
#include "core/SkFontMgr.h"
#include "core/SkTypeface.h"
#include "core/SkData.h"
#include "ports/SkFontMgr_data.h"
#include "pathops/SkPathOps.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

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

constexpr double CONTENT_WIDTH = 120.0;

// A fill with no style of its own. Stage 6 gives regions their styling; until
// then a region reads as the outline it belongs to, translucent enough that
// what is underneath still shows — which is the honest look for a fill that is
// a reference to a cycle rather than an object with a surface.
constexpr SkColor FILL = SkColorSetARGB(0x33, 0x6e, 0xc7, 0xff);
constexpr SkColor FILL_SELECTED = SkColorSetARGB(0x4c, 0xff, 0xa8, 0x5c);

// A region that no longer encloses anything. Diagnostic rather than dissolved:
// the fill the user asked for is still declared, and one undo has it back.
constexpr SkColor BROKEN = SkColorSetARGB(0xcc, 0xff, 0x8c, 0x42);
constexpr float BROKEN_STROKE = 3.5f;

// A tag's handles. Larger than a vertex and hollow, so the affordance the tag
// supplies reads as an affordance rather than as the point beneath it.
constexpr SkColor HANDLE = SkColorSetARGB(0xff, 0x2f, 0x6f, 0xd0);
constexpr float HANDLE_RADIUS = 5.5f;
constexpr float HANDLE_STROKE = 1.5f;

// Walks a region's boundary into a closed path, in pixels.
//
// The path is rebuilt from the pose every frame rather than cached, because a
// region has no geometry of its own: it names a cycle of edges, and where those
// edges are is a question only the current pose can answer. A cached path would
// be exactly the second representation the whole equivalence exists to avoid.
//
// Which points the ring runs through is core's answer, not one computed again
// here. Whether a region encloses anything decides both what is drawn and
// whether the degradation state says broken, and two implementations of that
// would eventually disagree about a region on screen.
//
// Returns false when the boundary cannot be walked — an edge deleted out from
// under it, or two edges that do not meet. Drawing a partial fill would be
// worse than drawing none: it would show an area the document does not bound.
// The broken state is drawn as a diagnostic instead, by its caller.
bool buildBoundaryPath(const Pose &pose, const ViewTransform &view,
                       const RegionRecord &region, SkPathBuilder &out) {
    // Arcs are boundary-capable in the taxonomy but are drawn here by their
    // chord: a curved boundary needs the fill tessellated along the sweep,
    // which lands with the arcs-as-boundaries work the three-edge minimum is
    // already waiting on.
    const std::optional<std::vector<EntityId>> ring = boundaryRing(pose.document(), region);
    if(!ring) return false;

    bool first = true;
    for(EntityId id : *ring) {
        const std::optional<Point> at = pose.point(id);
        if(!at) return false;
        const Eigen::Vector2d v = view.toScreen(*at);
        const SkPoint p = SkPoint::Make(static_cast<SkScalar>(v.x()),
                                        static_cast<SkScalar>(v.y()));
        if(first) {
            out.moveTo(p);
            first = false;
        } else {
            out.lineTo(p);
        }
    }
    out.close();
    return true;
}

// A region's area as a path, resolving the algebra over its live operands.
//
// The booleans are evaluated here, per frame, from the operands as they
// currently stand — never baked into a stored outline. That is what keeps a
// composite editable: drag a vertex of the cutter and the hole follows, because
// the hole was never anything but this computation.
bool buildRegionPath(const Pose &pose, const ViewTransform &view, const RegionRecord &region,
                     SkPath &out) {
    if(region.op == CompositeOp::Outline) {
        SkPathBuilder builder(SkPathFillType::kEvenOdd);
        if(!buildBoundaryPath(pose, view, region, builder)) return false;
        out = builder.detach();
        return true;
    }

    SkPathOp op = kUnion_SkPathOp;
    switch(region.op) {
        case CompositeOp::Union:     op = kUnion_SkPathOp; break;
        case CompositeOp::Intersect: op = kIntersect_SkPathOp; break;
        case CompositeOp::Subtract:  op = kDifference_SkPathOp; break;
        case CompositeOp::Outline:   break;
    }

    bool started = false;
    for(RegionId id : region.operands) {
        const RegionRecord *operand = pose.document().regions().find(id);
        if(operand == nullptr) return false;
        SkPath next;
        if(!buildRegionPath(pose, view, *operand, next)) return false;
        if(!started) {
            out = next;
            started = true;
            continue;
        }
        SkPath combined;
        if(!Op(out, next, op, &combined)) return false;
        out = combined;
    }
    return started;
}

// Every edge a region reaches, its operands' included. What the broken
// diagnostic is drawn from: a composite has no boundary of its own, so showing
// what a broken one still refers to means walking to the outlines underneath it.
//
// Not how "is this region selected" is answered — that is core's regionSelected,
// because render and the region actions must agree on it. This is a drawing
// question and has one caller.
void boundaryEdgesOf(const Document &doc, const RegionRecord &region,
                     std::vector<EntityId> &out) {
    for(EntityId id : region.boundary) out.push_back(id);
    for(RegionId id : region.operands) {
        if(const RegionRecord *operand = doc.regions().find(id)) {
            boundaryEdgesOf(doc, *operand, out);
        }
    }
}

// How much of a style's opacity slot reaches the paint. Evaluated rather than
// read, because it is a slot: a named document parameter can drive every fill's
// transparency at once, which is the whole reason styling values are slots.
uint8_t alphaOf(const Document &doc, const StyleRecord *style, uint8_t base) {
    if(style == nullptr) return base;
    const double factor = std::clamp(doc.evaluate(style->opacity).value_or(1.0), 0.0, 1.0);
    return static_cast<uint8_t>(std::lround(base * factor));
}

SkColor fillColourOf(const Document &doc, const RegionRecord &region,
                     const Adornment &adornment) {
    const StyleRecord *style = doc.styles().find(region.style);

    // A region is selected when the geometry bounding it is: the fill has no
    // identity of its own to select, which is the same reason it has no
    // geometry of its own to go stale.
    //
    // Core's question, not a second one asked here. Highlighting on any one edge
    // while every region action wanted all of them meant a user could watch a
    // fill light up and then watch punch, raise, lower and subtract refuse it.
    if(regionSelected(doc, region, adornment.selected)) {
        return SkColorSetA(FILL_SELECTED, alphaOf(doc, style, SkColorGetA(FILL_SELECTED)));
    }
    // A style is honoured when the document names one and asks to be filled.
    // Regions without one take the default rather than vanishing, because a
    // fill nobody styled is still a fill somebody asked for.
    if(style != nullptr && style->filled && (style->fillColor >> 24) != 0) {
        return SkColorSetA(style->fillColor,
                           alphaOf(doc, style, SkColorGetA(style->fillColor)));
    }
    return SkColorSetA(FILL, alphaOf(doc, style, SkColorGetA(FILL)));
}

constexpr SkColor DIMENSION_TEXT = SkColorSetRGB(0xd2, 0xd8, 0xe2);
// Screen-space, like every other adorner: dimension text does not scale with
// zoom, because a label is for reading and reading has one comfortable size.
constexpr float DIMENSION_TEXT_SIZE = 11.0f;

// The bundled face, built once.
//
// A function-local static rather than a global: the face is only needed when
// something valued is on screen, and a headless run that draws no dimension
// should not pay to parse a megabyte of font tables. Returns null only if the
// embedded bytes are not a face Skia understands, which is a build error
// wearing a runtime disguise — the caller draws no text rather than crashing.
sk_sp<SkTypeface> bundledTypeface() {
    static const sk_sp<SkTypeface> face = [] {
        const std::span<const unsigned char> bytes = bundledTypefaceBytes();
        if(bytes.empty()) return sk_sp<SkTypeface>();
        // Custom_Data rather than the system manager: the whole point is that
        // nothing about what this draws depends on what the host has installed.
        sk_sp<SkData> data = SkData::MakeWithoutCopy(bytes.data(), bytes.size());
        sk_sp<SkFontMgr> manager = SkFontMgr_New_Custom_Data(SkSpan(&data, 1));
        if(manager == nullptr) return sk_sp<SkTypeface>();
        return manager->makeFromData(data);
    }();
    return face;
}

// What a valued relation reads, formatted for a label.
//
// Display rounding never round-trips into stored values — this string is for
// looking at, and an edit session opens on the full-precision number rather
// than on what was rendered here.
//
// A driving dimension shows the value it holds; a reference measurement shows
// what the geometry is doing, because that is the difference between the two.
// A reference whose slot still carried the last value it drove at would be a
// readout that lies, and the toggle exists precisely so the same object can be
// both.
std::string dimensionText(const Pose &pose, const ConstraintRecord &constraint) {
    const ConstraintKindInfo &info = constraintInfo(constraint.kind);
    if(info.valueArity != 1) return {};

    std::optional<double> value;
    if(constraint.driving) {
        value = pose.document().evaluate(constraint.value);
    } else {
        value = measure(pose, constraint);
    }
    if(!value) return {};

    char buffer[32];
    const int written = std::snprintf(buffer, sizeof(buffer),
                                      constraint.kind == ConstraintKind::Angle ? "%.1f°"
                                                                               : "%.2f",
                                      *value);
    if(written <= 0) return {};
    std::string text(buffer, size_t(written));
    // A reference measurement is bracketed, which is the drafting convention
    // for a dimension that reports rather than drives.
    if(!constraint.driving) text = "(" + text + ")";
    return text;
}

// One distinct mark per constraint kind. The kinds that carry no value are
// strokes, following drafting convention where there is one so the marks are
// readable before they are learned; a valued kind draws its number instead,
// because a dimension whose value is invisible is a dimension the user has to
// select to read.
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

ViewTransform ViewState::transform(double width, double height) const {
    const Eigen::Vector2d centre(width * 0.5, height * 0.5);
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(centre + pan);
    m.scale(Eigen::Vector2d(zoom, zoom));
    m.translate(-centre);
    return ViewTransform(m * base.matrix());
}

void ViewState::frameOnce(const Pose &pose, int width, int height, bool sizeIsReal) {
    if(framed) return;
    base = fitView(pose, width, height);
    framed = sizeIsReal;
}

void ViewState::zoomAt(const Eigen::Vector2d &cursor, double factor, double width,
                       double height) {
    if(!(factor > 0.0) || factor == zoom) return;
    const Point anchor = transform(width, height).toDocument(cursor);
    zoom = factor;
    // Where the anchor lands now, against where it must land. Pan is outermost,
    // so the difference is the correction rather than the start of one.
    pan += cursor - transform(width, height).toScreen(anchor);
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

    // The placement grid, on the step the caller was given, clipped to whatever
    // the view shows.
    SkPaint grid;
    grid.setAntiAlias(false);
    grid.setColor(GRID);
    grid.setStrokeWidth(1.0f);
    const Point topLeft = view.toDocument(Eigen::Vector2d(0.0, 0.0));
    const Point bottomRight = view.toDocument(Eigen::Vector2d(logicalWidth, logicalHeight));
    const double gridStep = adornment.gridStep;
    const double lowX = std::min(topLeft.x, bottomRight.x);
    const double highX = std::max(topLeft.x, bottomRight.x);
    const double lowY = std::min(topLeft.y, bottomRight.y);
    const double highY = std::max(topLeft.y, bottomRight.y);
    // Bounded so a zoomed-far-out view does not try to draw a million lines.
    if(gridStep > 0.0 && (highX - lowX) / gridStep < 400.0) {
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
    // Styling sits below the transient states and above nothing: a construction
    // line reads as a guide however it is styled, because its role is what it
    // is rather than how it looks.
    auto tintOf = [&](const EntityRecord &e) {
        SkColor colour = GEOMETRY;
        if(const StyleRecord *style = pose.document().styles().find(e.style)) {
            colour = SkColorSetA(style->strokeColor,
                                 alphaOf(pose.document(), style, SkColorGetA(style->strokeColor)));
        }
        if(e.role == Role::Construction) colour = CONSTRUCTION;
        if(contains(adornment.selected, e.id)) colour = SELECTED;
        if(adornment.hovered == e.id) colour = HOVERED;
        if(contains(resistingEntities, e.id)) colour = RESISTING;
        return colour;
    };

    // Construction geometry reads as a guide: thinner and dimmer, and not
    // styleable, for the same reason it is not tintable.
    auto strokeWidthOf = [&](const EntityRecord &e) {
        if(e.role == Role::Construction) return 1.0f;
        if(contains(adornment.selected, e.id)) return SELECTED_STROKE_WIDTH;
        // A width is a slot, so it can be a constant, an expression, or a named
        // parameter driving every stroke in the document at once.
        if(const StyleRecord *style = pose.document().styles().find(e.style)) {
            const double w = pose.document().evaluate(style->strokeWidth).value_or(1.0);
            if(w > 0.0) return static_cast<float>(w);
        }
        return STROKE_WIDTH;
    };

    SkPaint fill;
    fill.setAntiAlias(true);
    fill.setStyle(SkPaint::kFill_Style);

    SkPaint stroke;
    stroke.setAntiAlias(true);
    stroke.setStyle(SkPaint::kStroke_Style);
    stroke.setStrokeCap(SkPaint::kRound_Cap);

    // One layer's fills, composited among themselves.
    //
    // A region has no geometry of its own. The path is walked from the boundary
    // edges every frame, out of the same pose the outline is drawn from, which
    // is exactly why dragging a vertex moves the fill: there is no second copy
    // to go stale, and nothing to keep in step. A composite is the same claim
    // one level up — its area is a computation over live operands, evaluated
    // now, never a baked outline.
    //
    // Inside a saved layer, because punch-through is an alpha overwrite and has
    // to carve what this layer has accumulated rather than what the canvas has.
    // Cutting through to the background would make a cut-out's effect depend on
    // what happened to be underneath it, which is precisely the destructive
    // reading of a boolean that the compositional one exists to replace.
    auto drawFills = [&](LayerId layer) {
        const std::vector<RegionId> order = regionOrder(pose.document(), layer);
        if(order.empty()) return;
        canvas.saveLayer(nullptr, nullptr);
        for(RegionId id : order) {
            const RegionRecord &region = *pose.document().regions().find(id);

            // Broken renders as a diagnostic, never as nothing and never as a
            // partial fill: what the user deleted is visibly still referred to,
            // and one undo puts it back. A partial fill would show an area the
            // document does not bound; silence would let a fill evaporate.
            if(regionState(pose.document(), region) != RegionState::Whole) {
                stroke.setColor(BROKEN);
                stroke.setStrokeWidth(BROKEN_STROKE);
                std::vector<EntityId> edges;
                boundaryEdgesOf(pose.document(), region, edges);
                for(EntityId edge : edges) {
                    if(const auto ends = pose.segment(edge)) {
                        canvas.drawLine(toPixel(ends->first), toPixel(ends->second), stroke);
                    }
                }
                continue;
            }

            SkPath path;
            if(!buildRegionPath(pose, view, region, path)) continue;
            // Even-odd, so a boundary that crosses itself renders as the
            // alternating fill a user expects rather than as a solid blob.
            path.setFillType(SkPathFillType::kEvenOdd);

            if(region.punch) {
                fill.setBlendMode(SkBlendMode::kClear);
                canvas.drawPath(path, fill);
                fill.setBlendMode(SkBlendMode::kSrcOver);
                continue;
            }
            fill.setColor(fillColourOf(pose.document(), region, adornment));
            canvas.drawPath(path, fill);
        }
        canvas.restore();
    };

    // One layer's strokes. Edges before arcs before circles, and vertices not
    // at all — those go last over every layer, because a handle is an adorner
    // and adorners sit above the drawing rather than inside it, which is also
    // what the hit priority says.
    auto drawStrokes = [&](LayerId layer) {
        for(const EntityRecord &e : pose.document().entities().records()) {
            if(e.layer != layer) continue;
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
        if(e.layer != layer) continue;
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
        if(e.layer != layer) continue;
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
    };

    // Back to front. Hidden layers are skipped here and nowhere else: their
    // geometry is in every solve exactly as it was, which is the whole
    // difference between hiding a thing and deleting it.
    const std::vector<LayerId> layers = layerOrder(pose.document());
    for(LayerId layer : layers) {
        if(!layerVisible(pose.document(), layer)) continue;
        drawFills(layer);
        drawStrokes(layer);
    }

    // Vertices, in screen space at a fixed size, over every layer.
    SkPaint dot;
    dot.setAntiAlias(true);
    dot.setStyle(SkPaint::kFill_Style);
    for(const EntityRecord &e : pose.document().entities().records()) {
        if(!layerVisible(pose.document(), e.layer)) continue;
        const std::optional<Point> p = pose.point(e.id);
        if(!p) continue;

        dot.setColor(tintOf(e));
        canvas.drawCircle(toPixel(*p),
                          contains(adornment.selected, e.id) ? SELECTED_POINT_RADIUS
                                                             : POINT_RADIUS,
                          dot);
    }

    // Tag affordances, above the vertices, because a handle is an adorner over
    // an adorner: the corner it sits on is already a draggable point, and what
    // the handle adds is that this point is a corner of something.
    //
    // Square rather than round, and hollow rather than filled, so a rectangle
    // handle is distinguishable at a glance from the vertex underneath it — the
    // affordance is the tag's, and mistaking it for the point would be mistaking
    // a resize for a drag.
    {
        SkPaint handle;
        handle.setAntiAlias(true);
        handle.setStyle(SkPaint::kStroke_Style);
        handle.setStrokeWidth(HANDLE_STROKE);
        for(TagId id : adornment.handledTags) {
            const std::optional<RectangleFrame> frame = rectangleFrame(pose.document(), id);
            if(!frame) continue;
            const std::optional<std::array<Point, 4>> corners = rectangleHandles(pose, *frame);
            if(!corners) continue;
            handle.setColor(HANDLE);
            for(const Point &corner : *corners) {
                const SkPoint p = toPixel(corner);
                canvas.drawRect(SkRect::MakeLTRB(p.x() - HANDLE_RADIUS, p.y() - HANDLE_RADIUS,
                                                 p.x() + HANDLE_RADIUS, p.y() + HANDLE_RADIUS),
                                handle);
            }
        }

        // And the degradation. A broken tag costs the user the affordances and
        // nothing else — every primitive is untouched — so the diagnostic marks
        // what the tag still names rather than covering the geometry: the user
        // sees which rectangle stopped being one, and one undo puts it back.
        handle.setColor(BROKEN);
        for(TagId id : brokenTags(pose.document())) {
            const TagRecord *tag = pose.document().tags().find(id);
            if(tag == nullptr) continue;
            for(EntityId e : tag->entities) {
                const EntityRecord *record = pose.document().entities().find(e);
                if(record == nullptr || !layerVisible(pose.document(), record->layer)) continue;
                if(const auto ends = pose.segment(e)) {
                    const SkPoint middle =
                        toPixel(Point{0.5 * (ends->first.x + ends->second.x),
                                      0.5 * (ends->first.y + ends->second.y)});
                    canvas.drawCircle(middle, HANDLE_RADIUS, handle);
                }
            }
        }
    }

    // One arc tessellation for both ghosts, the previewed relation's and the
    // tool's. Two would drift, and the flat spots would drift with them.
    auto ghostArc = [&](Point centre, double radius, double from, double sweep) {
        const double onScreen =
            (view.toScreen(Point{centre.x + radius, centre.y}) - view.toScreen(centre)).norm();
        const int steps = std::clamp(static_cast<int>(onScreen * std::abs(sweep) * 0.5), 8, 240);
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

    // Where a hovered relation would put the drawing.
    //
    // The payoff the speculative solve was always for, and the difference
    // between a catalogue that is read and one that is learned by looking. Drawn
    // over the geometry it would replace, in the same dimmed ink a tool's rubber
    // band uses, because it is the same kind of promise: this is what commit
    // produces.
    //
    // Only what moves. An imposition is movement-free by design, so most
    // previews would otherwise redraw the whole document on top of itself; what
    // the user needs to see is the part that is not staying put.
    if(!adornment.ghostPose.empty()) {
        Pose ghost = pose;
        ghost.overlay(adornment.ghostPose);

        stroke.setColor(GHOST);
        stroke.setStrokeWidth(STROKE_WIDTH);
        dot.setColor(GHOST);

        // Far enough to be worth showing, in document units. Below this the
        // solver's last bits are all that differ and the ghost would shimmer
        // over geometry that did not move.
        constexpr double MOVED = 1e-9;
        auto moved = [](Point a, Point b) {
            return std::abs(a.x - b.x) > MOVED || std::abs(a.y - b.y) > MOVED;
        };

        for(const EntityRecord &e : pose.document().entities().records()) {
            if(!layerVisible(pose.document(), e.layer)) continue;

            if(const auto ends = pose.segment(e.id)) {
                const auto after = ghost.segment(e.id);
                if(!after) continue;
                if(!moved(ends->first, after->first) && !moved(ends->second, after->second)) {
                    continue;
                }
                canvas.drawLine(toPixel(after->first), toPixel(after->second), stroke);
                continue;
            }
            if(const std::optional<Pose::ArcGeometry> was = pose.arc(e.id)) {
                const std::optional<Pose::ArcGeometry> now = ghost.arc(e.id);
                if(!now) continue;
                if(!moved(was->centre, now->centre) &&
                   std::abs(was->radius - now->radius) <= MOVED &&
                   std::abs(was->startAngle - now->startAngle) <= MOVED &&
                   std::abs(was->sweep - now->sweep) <= MOVED) {
                    continue;
                }
                ghostArc(now->centre, now->radius, now->startAngle, now->sweep);
                continue;
            }
            if(const std::optional<double> radius = pose.radius(e.id)) {
                const std::optional<Point> centre = pose.point(e.points[0]);
                const std::optional<double> after = ghost.radius(e.id);
                const std::optional<Point> to = ghost.point(e.points[0]);
                if(!centre || !after || !to) continue;
                if(!moved(*centre, *to) && std::abs(*radius - *after) <= MOVED) continue;
                const Eigen::Vector2d edge = view.toScreen(Point{to->x + *after, to->y});
                const Eigen::Vector2d middle = view.toScreen(*to);
                canvas.drawCircle(toPixel(*to), static_cast<SkScalar>((edge - middle).norm()),
                                  stroke);
                continue;
            }
            if(const std::optional<Point> p = pose.point(e.id)) {
                const std::optional<Point> to = ghost.point(e.id);
                if(!to || !moved(*p, *to)) continue;
                canvas.drawCircle(toPixel(*to), POINT_RADIUS, dot);
            }
        }
    }

    // The tool's rubber band, over the geometry and under the marquee. Drawn
    // last among document-space things so it is never hidden by what it will
    // sit alongside.
    if(adornment.ghostActive) {
        stroke.setColor(GHOST);
        stroke.setStrokeWidth(STROKE_WIDTH);
        dot.setColor(GHOST);

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

        // Placed by core rather than here. Marks on one anchor fan out so a
        // vertex with three relations reads as three — and hit testing has to
        // pick them exactly where this draws them, so the arithmetic is shared
        // rather than written twice and kept in agreement.
        // The bundled face, for the marks that carry a number rather than a
        // shape. Built lazily and only when something valued is on screen.
        SkFont font;
        bool haveFont = false;

        const std::vector<Eigen::Vector2d> places = layOutGlyphs(adornment.glyphs, view);
        for(size_t i = 0; i < adornment.glyphs.size() && i < places.size(); i++) {
            const GlyphMark &m = adornment.glyphs[i];
            const SkPoint at{static_cast<SkScalar>(places[i].x()),
                             static_cast<SkScalar>(places[i].y())};

            glyph.setColor(m.ghost ? GLYPH_GHOST : (m.fresh ? GLYPH_FRESH : GLYPH));
            if(m.selected || m.hovered) glyph.setColor(SELECTED);

            // A dimension shows its value. Anchored to the document, so it
            // travels with the geometry it measures, and sized in screen pixels,
            // so it stays readable at every zoom — the two halves of
            // "document-anchored, screen-scaled".
            std::string text;
            const ConstraintRecord *record =
                m.ghost ? nullptr : pose.document().constraints().find(m.constraint);
            if(record != nullptr) text = dimensionText(pose, *record);

            if(!text.empty()) {
                if(!haveFont) {
                    if(const sk_sp<SkTypeface> face = bundledTypeface()) {
                        font = SkFont(face, DIMENSION_TEXT_SIZE);
                        font.setSubpixel(true);
                        haveFont = true;
                    }
                }
                if(haveFont) {
                    SkPaint label;
                    label.setAntiAlias(true);
                    label.setColor(m.selected || m.hovered ? SELECTED : DIMENSION_TEXT);
                    // Centred on the mark's place, so the number sits where the
                    // shape would have and the fan-out keeps two dimensions on
                    // one vertex apart.
                    const SkScalar width =
                        font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8);
                    canvas.drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8,
                                          at.fX - width * 0.5f,
                                          at.fY + DIMENSION_TEXT_SIZE * 0.35f, font, label);
                    continue;
                }
            }
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
