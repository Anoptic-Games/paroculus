#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "render/view.h"
#include "solve/demosketch.h"
#include "solve/solve.h"
#include "support/build.h"

using namespace paroculus;
using paroculus::test::addPoint;
using paroculus::test::addSegment;

namespace {

constexpr int W = 400;
constexpr int H = 300;
// canvas.clear(SkColorSetRGB(0x14, 0x16, 0x1a)) as a little-endian BGRA word.
constexpr uint32_t BACKGROUND = 0xff14161au;

std::vector<uint32_t> paint(const Pose &pose, const ViewTransform &view,
                            const Adornment &adornment = {}) {
    std::vector<uint32_t> pixels(static_cast<size_t>(W) * H, 0u);
    renderDocument(pose, view, adornment, reinterpret_cast<uint8_t *>(pixels.data()), W, H,
                   static_cast<size_t>(W) * 4);
    return pixels;
}

uint32_t at(const std::vector<uint32_t> &pixels, const Eigen::Vector2d &p) {
    const int x = static_cast<int>(p.x());
    const int y = static_cast<int>(p.y());
    REQUIRE(x >= 0);
    REQUIRE(y >= 0);
    REQUIRE(x < W);
    REQUIRE(y < H);
    return pixels[static_cast<size_t>(y) * W + x];
}

// Whether anything was painted within a pixel of `p`. A one-pixel non-antialiased
// line lands on one side or the other of a coordinate depending on how the
// rasteriser rounds, which is not what any of these tests are about.
bool paintedNear(const std::vector<uint32_t> &pixels, const Eigen::Vector2d &p) {
    for(int dy = -1; dy <= 1; dy++) {
        for(int dx = -1; dx <= 1; dx++) {
            const int x = static_cast<int>(p.x()) + dx;
            const int y = static_cast<int>(p.y()) + dy;
            if(x < 0 || y < 0 || x >= W || y >= H) continue;
            if(pixels[static_cast<size_t>(y) * W + x] != BACKGROUND) return true;
        }
    }
    return false;
}

// 1:1 with the origin at the viewport centre and Y flipped, so a fixture can
// place geometry where it wants it without reasoning about the demo's framing.
ViewTransform centredView() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(W * 0.5, H * 0.5));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    return ViewTransform(m);
}

// Pixels whose red beats their blue. Every adorned tint is warm and both the
// background and ordinary geometry are cool, so this counts what is adorned
// without pinning an exact colour word an antialiased rim may never produce.
size_t warmPixels(const std::vector<uint32_t> &pixels) {
    size_t count = 0;
    for(uint32_t p : pixels) {
        if(((p >> 16) & 0xffu) > (p & 0xffu)) count++;
    }
    return count;
}

// The demo, solved, as the shell would show it.
Pose settledDemo(Document &doc) {
    doc = demoDocument(1.618);
    SolveContext context = SolveContext::forWholeDocument(doc);
    REQUIRE(solve(doc, context).ok());
    for(const Command &c : context.commitCommands(doc)) REQUIRE(doc.apply(c).ok());
    return Pose(doc);
}

}  // namespace

TEST_CASE("the raster and the view transform agree") {
    // Analytic sampling rather than a golden image: the point the transform
    // says a vertex lands on must be the point Skia painted it on. Hit testing
    // reads the same transform, so this is what keeps picking aligned with what
    // the user sees.
    Document doc;
    const Pose pose = settledDemo(doc);
    const ViewTransform view = defaultView(W, H);
    const std::vector<uint32_t> pixels = paint(pose, view);

    for(const EntityRecord &e : doc.entities().records()) {
        const std::optional<Point> p = pose.point(e.id);
        if(!p) continue;
        CHECK(at(pixels, view.toScreen(*p)) != BACKGROUND);
    }
}

TEST_CASE("the raster clears the whole surface") {
    Document doc;
    const Pose pose = settledDemo(doc);
    const std::vector<uint32_t> pixels = paint(pose, defaultView(W, H));
    // A corner well away from geometry. No grid: the default adornment names
    // no step, and render no longer keeps one of its own.
    CHECK(pixels[2u * W + 2u] == BACKGROUND);
}

TEST_CASE("a degenerate viewport paints nothing") {
    Document doc;
    const Pose pose = settledDemo(doc);
    std::vector<uint32_t> pixels(16u, 0u);
    renderDocument(pose, defaultView(W, H), Adornment{},
                   reinterpret_cast<uint8_t *>(pixels.data()), 0, 0, 0);
    renderDocument(pose, defaultView(W, H), Adornment{}, nullptr, 4, 4, 16);
    for(uint32_t p : pixels) CHECK(p == 0u);
}

TEST_CASE("fitView frames the content and flips Y") {
    Document doc;
    const Pose pose = settledDemo(doc);
    const ViewTransform view = fitView(pose, W, H);

    const Eigen::Vector2d up = view.toScreen({0.0, 10.0});
    const Eigen::Vector2d down = view.toScreen({0.0, -10.0});
    CHECK(up.y() < down.y());

    // Everything the document places lands inside the viewport.
    for(const EntityRecord &e : doc.entities().records()) {
        const std::optional<Point> p = pose.point(e.id);
        if(!p) continue;
        const Eigen::Vector2d s = view.toScreen(*p);
        CHECK(s.x() >= 0.0);
        CHECK(s.y() >= 0.0);
        CHECK(s.x() <= W);
        CHECK(s.y() <= H);
    }
}

TEST_CASE("an empty document still gets a sensible view") {
    Document doc;
    const Pose pose(doc);
    const ViewTransform view = fitView(pose, W, H);
    // Falls back to the default framing rather than dividing by an empty bound.
    CHECK(view.toScreen({0.0, 0.0}).allFinite());
    const std::vector<uint32_t> pixels = paint(pose, view);
    CHECK(pixels[2u * W + 2u] == BACKGROUND);
}

TEST_CASE("selection changes what is drawn") {
    // Adornment is presentation only: the same pose with a different selection
    // paints differently, and the document is not consulted about it.
    Document doc;
    const Pose pose = settledDemo(doc);
    const ViewTransform view = defaultView(W, H);

    const EntityId first = doc.entities().records().front().id;
    const std::vector<uint32_t> plain = paint(pose, view);

    Adornment adornment;
    adornment.selected = {first};
    const std::vector<uint32_t> selected = paint(pose, view, adornment);

    CHECK(plain != selected);
    // The selected vertex itself changed colour.
    CHECK(at(plain, view.toScreen(*pose.point(first))) !=
          at(selected, view.toScreen(*pose.point(first))));
}

TEST_CASE("a marquee is drawn in screen space") {
    Document doc;
    const Pose pose = settledDemo(doc);
    const ViewTransform view = defaultView(W, H);

    Adornment adornment;
    adornment.marqueeActive = true;
    adornment.marqueeFrom = Eigen::Vector2d(10.0, 10.0);
    adornment.marqueeTo = Eigen::Vector2d(60.0, 60.0);

    const std::vector<uint32_t> pixels = paint(pose, view, adornment);
    // Inside the band, and away from any geometry.
    CHECK(at(pixels, Eigen::Vector2d(30.0, 30.0)) != BACKGROUND);
    // Outside it, background survives.
    CHECK(at(pixels, Eigen::Vector2d(210.0, 20.0)) == BACKGROUND);
}

TEST_CASE("constraint marks reach the raster") {
    // No invisible constraints has to be true of the pixels, not only of the
    // data: a mark that is computed and never drawn is an invisible constraint
    // with extra steps.
    Document doc;
    const Pose pose = settledDemo(doc);
    const ViewTransform view = defaultView(W, H);
    const std::vector<uint32_t> plain = paint(pose, view);

    GlyphMark mark;
    mark.kind = ConstraintKind::Horizontal;
    // Somewhere empty, so what changes is the mark and not an overlap.
    mark.anchor = view.toDocument(Eigen::Vector2d(120.0, 250.0));

    Adornment adornment;
    adornment.glyphs = {mark};
    const std::vector<uint32_t> marked = paint(pose, view, adornment);
    CHECK(plain != marked);

    // And a ghost draws differently from a declared one, because "about to
    // exist" and "exists" are different claims.
    GlyphMark ghost = mark;
    ghost.ghost = true;
    Adornment ghosted;
    ghosted.glyphs = {ghost};
    CHECK(paint(pose, view, ghosted) != marked);
}

TEST_CASE("marks on one anchor fan out rather than stacking") {
    // A vertex with three relations has to read as three.
    Document doc;
    const Pose pose = settledDemo(doc);
    const ViewTransform view = defaultView(W, H);
    const Point anchor = view.toDocument(Eigen::Vector2d(200.0, 240.0));

    GlyphMark a;
    a.kind = ConstraintKind::Horizontal;
    a.anchor = anchor;
    a.constraint = ConstraintId(1);
    GlyphMark b = a;
    b.kind = ConstraintKind::Vertical;
    b.constraint = ConstraintId(2);

    Adornment one;
    one.glyphs = {a};
    Adornment two;
    two.glyphs = {a, b};
    // Two marks on the same anchor cover more of the surface than one does; if
    // they stacked, the second would be invisible.
    auto painted = [&](const Adornment &adornment) {
        const std::vector<uint32_t> pixels = paint(pose, view, adornment);
        return std::count_if(pixels.begin(), pixels.end(),
                             [](uint32_t p) { return p != BACKGROUND; });
    };
    CHECK(painted(two) > painted(one));
}

TEST_CASE("a device scale rasterises denser without moving anything") {
    // The HiDPI contract. The view transform, the adornment's coordinates and
    // every cosmetic size stay logical — that is what lets interact keep hit
    // radii in units of the hand — and only the raster gets denser. So geometry
    // must land at the logical position scaled up, not somewhere else.
    Document doc;
    const Pose pose = settledDemo(doc);
    const ViewTransform view = defaultView(W, H);

    constexpr int SCALE = 2;
    constexpr int DW = W * SCALE, DH = H * SCALE;
    std::vector<uint32_t> hi(static_cast<size_t>(DW) * DH, 0u);
    renderDocument(pose, view, Adornment{}, reinterpret_cast<uint8_t *>(hi.data()), DW, DH,
                   static_cast<size_t>(DW) * 4, SCALE);

    auto hiAt = [&](Eigen::Vector2d p) {
        const int x = static_cast<int>(p.x()), y = static_cast<int>(p.y());
        REQUIRE(x >= 0);
        REQUIRE(y >= 0);
        REQUIRE(x < DW);
        REQUIRE(y < DH);
        return hi[static_cast<size_t>(y) * DW + x];
    };

    for(const EntityRecord &e : doc.entities().records()) {
        const std::optional<Point> p = pose.point(e.id);
        if(!p) continue;
        CHECK(hiAt(view.toScreen(*p) * SCALE) != BACKGROUND);
    }

    // The grid is clipped to the *logical* viewport, so a doubled buffer must
    // not read as a doubled view: the same corner is still empty background.
    CHECK(hiAt(Eigen::Vector2d(2.0 * SCALE, 2.0 * SCALE)) == BACKGROUND);

    // And a vertex covers more device pixels than it does at 1x, which is what
    // separates a genuinely denser raster from a 1x image in a bigger buffer.
    const std::vector<uint32_t> lo = paint(pose, view);
    const Eigen::Vector2d vertex = view.toScreen(*pose.point(doc.entities().records().front().id));
    auto coverage = [&](const std::vector<uint32_t> &px, int stride, Eigen::Vector2d centre) {
        int painted = 0;
        for(int dy = -10; dy <= 10; dy++) {
            for(int dx = -10; dx <= 10; dx++) {
                const int x = static_cast<int>(centre.x()) + dx;
                const int y = static_cast<int>(centre.y()) + dy;
                if(x < 0 || y < 0 || x >= stride) continue;
                if(px[static_cast<size_t>(y) * stride + x] != BACKGROUND) painted++;
            }
        }
        return painted;
    };
    CHECK(coverage(hi, DW, vertex * SCALE) > coverage(lo, W, vertex));
}

TEST_CASE("a degenerate device scale paints nothing") {
    Document doc;
    const Pose pose = settledDemo(doc);
    std::vector<uint32_t> pixels(16u, 0u);
    for(double scale : {0.0, -1.0}) {
        renderDocument(pose, defaultView(W, H), Adornment{},
                       reinterpret_cast<uint8_t *>(pixels.data()), 4, 4, 16, scale);
    }
    for(uint32_t p : pixels) CHECK(p == 0u);
}

TEST_CASE("construction geometry draws differently from ordinary geometry") {
    // A guide is an ordinary line with a render role; only the presentation
    // differs.
    Document plain;
    const EntityId a = addPoint(plain, -40.0, 0.0);
    const EntityId b = addPoint(plain, 40.0, 0.0);
    const EntityId segment = addSegment(plain, a, b);

    Document guided = plain;
    EntityRecord asGuide = *guided.entities().find(segment);
    asGuide.role = Role::Construction;
    REQUIRE(guided.apply(SetRecord<EntityRecord>{asGuide}).ok());

    const ViewTransform view = defaultView(W, H);
    const std::vector<uint32_t> ordinary = paint(Pose(plain), view);
    const std::vector<uint32_t> construction = paint(Pose(guided), view);
    CHECK(ordinary != construction);
}

TEST_CASE("every drawn kind takes the hover and resistance tints") {
    // The tint rule was written out once per entity kind, and the copy under
    // circles drifted: it read selection and role only. A circle resisting a
    // saturated drag was then attributed everywhere except on the circle, which
    // is the one place the user is looking.
    //
    // Counted rather than sampled, because the tint colours are warm and the
    // geometry colour is blue: red over blue in any pixel means some adorned
    // state reached the raster, and that survives antialiasing on a rim.
    Document doc;
    const EntityId a = addPoint(doc, -120.0, -80.0);
    const EntityId b = addPoint(doc, -40.0, -80.0);
    const EntityId segment = addSegment(doc, a, b);

    const EntityId circleCentre = addPoint(doc, 60.0, -50.0);
    const EntityId circle = paroculus::test::addCircle(doc, circleCentre, 25.0);

    const EntityId arcCentre = addPoint(doc, 60.0, 50.0);
    const EntityId arcStart = addPoint(doc, 95.0, 50.0);
    const EntityId arcEnd = addPoint(doc, 60.0, 85.0);
    const EntityId arc = paroculus::test::addArc(doc, arcCentre, arcStart, arcEnd);

    const ConstraintId onCircle = paroculus::test::addConstraint(
        doc, ConstraintKind::PointOnCircle, {arcStart, circle});
    REQUIRE(onCircle.valid());

    const Pose pose(doc);
    const ViewTransform view = centredView();
    REQUIRE(warmPixels(paint(pose, view)) == 0u);

    for(EntityId id : {segment, circle, arc, a}) {
        Adornment hover;
        hover.hovered = id;
        CHECK(warmPixels(paint(pose, view, hover)) > 0u);
    }

    // And resistance, which reaches the circle through the constraint's
    // operands rather than by being named directly.
    Adornment resisting;
    resisting.resisting = {onCircle};
    CHECK(warmPixels(paint(pose, view, resisting)) > 0u);
}

TEST_CASE("the drawn grid is the placement grid") {
    // Render used to hold its own 20.0 while SnapPolicy held the step placement
    // actually lands on. Two plausible numbers, and changing the policy made the
    // drawn grid lie about where a click goes — silently, since neither looks
    // wrong on its own.
    Document doc;
    const ViewTransform view = centredView();

    // Nothing said, nothing drawn: a caller that supplies no step gets no grid
    // rather than a guess at someone else's policy.
    for(uint32_t p : paint(Pose(doc), view)) CHECK(p == BACKGROUND);

    Adornment adornment;
    adornment.gridStep = 25.0;
    const std::vector<uint32_t> drawn = paint(Pose(doc), view, adornment);

    // A line where the step says placement lands.
    CHECK(paintedNear(drawn, view.toScreen(Point{25.0, 5.0})));
    CHECK(paintedNear(drawn, view.toScreen(Point{5.0, 25.0})));
    // And none where the render-side constant used to put one.
    CHECK(!paintedNear(drawn, view.toScreen(Point{20.0, 5.0})));
}

TEST_CASE("a pan translates the view by exactly its pixels") {
    ViewState state;
    state.base = centredView();
    // Under a zoom, so the assertion is that the pan is not scaled by it. At
    // 1:1 a pan applied anywhere in the composition would look the same.
    state.zoom = 2.0;
    const Point p{30.0, -10.0};
    const Eigen::Vector2d before = state.transform(W, H).toScreen(p);

    state.pan = Eigen::Vector2d(13.0, -7.0);
    const Eigen::Vector2d after = state.transform(W, H).toScreen(p);

    // Pan is the outermost term, which is the property the anchored zoom below
    // is built on: it may add a correction and know it will not be rescaled.
    CHECK((after - before).x() == doctest::Approx(13.0));
    CHECK((after - before).y() == doctest::Approx(-7.0));
}

TEST_CASE("zoom holds the point under the cursor") {
    // Viewport-centre anchoring slides whatever is being examined away exactly
    // as it is magnified, so reaching it costs a zoom and then a pan.
    ViewState state;
    state.base = centredView();

    // Well off centre, or a centre-anchored zoom would pass by coincidence.
    const Eigen::Vector2d cursor(W * 0.25, H * 0.8);
    const Point anchor = state.transform(W, H).toDocument(cursor);

    state.zoomAt(cursor, 2.5, W, H);
    CHECK(state.zoom == doctest::Approx(2.5));
    const Eigen::Vector2d landed = state.transform(W, H).toScreen(anchor);
    CHECK(landed.x() == doctest::Approx(cursor.x()));
    CHECK(landed.y() == doctest::Approx(cursor.y()));

    // Zooming back out returns it too, so the anchoring composes rather than
    // accumulating an error the user pays for over a scroll.
    state.zoomAt(cursor, 1.0, W, H);
    const Eigen::Vector2d back = state.transform(W, H).toScreen(anchor);
    CHECK(back.x() == doctest::Approx(cursor.x()));
    CHECK(back.y() == doctest::Approx(cursor.y()));
}

TEST_CASE("a framed view is not re-framed by what the document grows") {
    // syncViewport re-fitted the framing to the geometry's bounding box on every
    // call, so drawing — or merely confirming an offer — reframed the window
    // under the cursor mid-gesture.
    Document doc;
    const EntityId a = addPoint(doc, -10.0, -10.0);
    addSegment(doc, a, addPoint(doc, 10.0, 10.0));

    ViewState state;
    state.frameOnce(Pose(doc), W, H, true);
    REQUIRE(state.framed);
    const Eigen::Vector2d before = state.transform(W, H).toScreen(Point{0.0, 0.0});

    // Geometry far outside the original bounding box: a re-fit would zoom out
    // to hold it, moving everything the user was looking at.
    addPoint(doc, 4000.0, 4000.0);
    state.frameOnce(Pose(doc), W, H, true);
    const Eigen::Vector2d after = state.transform(W, H).toScreen(Point{0.0, 0.0});
    CHECK(after.x() == doctest::Approx(before.x()));
    CHECK(after.y() == doctest::Approx(before.y()));

    // Clearing the latch is how resetView asks to be re-framed, and it is the
    // only thing that does.
    state.framed = false;
    state.frameOnce(Pose(doc), W, H, true);
    const Eigen::Vector2d reframed = state.transform(W, H).toScreen(Point{0.0, 0.0});
    CHECK(reframed.x() != doctest::Approx(before.x()));
}

TEST_CASE("a provisional framing does not latch") {
    // A shell item has no size during construction. A framing fitted against
    // 1x1 must not become the one the user keeps.
    Document doc;
    addPoint(doc, 0.0, 0.0);
    addPoint(doc, 100.0, 100.0);

    ViewState state;
    state.frameOnce(Pose(doc), 1, 1, false);
    CHECK(!state.framed);

    state.frameOnce(Pose(doc), W, H, true);
    CHECK(state.framed);
    // Fitted to the real viewport and not to the 1x1 one it saw first, which
    // would have crushed the whole document into the top-left pixel.
    const Eigen::Vector2d corner = state.transform(W, H).toScreen(Point{100.0, 100.0});
    CHECK(corner.x() > W * 0.5);
    CHECK(corner.x() < W);
}

TEST_CASE("a real-but-tiny viewport does not latch a framing") {
    // A layout pass hands the item a real-sized-but-still-settling viewport a few
    // pixels tall, with sizeIsReal true. Fitting the empty new-tab document into
    // three pixels of height is a scale so small the first drag spanned tens of
    // thousands of document units, and latching it froze that view — the canvas
    // reported 1x zoom while a subset-of-the-surface drag ran off to 60000, and
    // only resetView escaped. The framing must stay provisional until the
    // viewport is one worth keeping.
    Document doc;  // empty: the new-tab case, where the fit follows the viewport
                   // and a degenerate one is catastrophic.

    ViewState state;
    state.frameOnce(Pose(doc), 800, 3, true);
    CHECK(!state.framed);

    state.frameOnce(Pose(doc), W, H, true);
    CHECK(state.framed);
    // Half the viewport maps to a modest document span, not the tens of thousands
    // the crushed scale produced.
    const ViewTransform view = state.transform(W, H);
    const Point left = view.toDocument(Eigen::Vector2d(W * 0.25, H * 0.5));
    const Point right = view.toDocument(Eigen::Vector2d(W * 0.75, H * 0.5));
    CHECK(std::abs(right.x - left.x) < 5000.0);
}

TEST_CASE("fitView frames a circle by its rim, not by its centre") {
    // What a drawing occupies is not where its defining points are. A document
    // that is one filled circle has exactly one point in it — the centre — so a
    // framing built from points alone fitted to nothing and fell through to the
    // degenerate-extent floor. A circle alone is a complete drawing now that it
    // bounds a region.
    Document circleDoc;
    const EntityId centre = test::addPoint(circleDoc, 0.0, 0.0);
    REQUIRE(test::addCircle(circleDoc, centre, 40.0).valid());

    // The same extent, spelled as points, is the framing to match.
    Document pointDoc;
    test::addPoint(pointDoc, -40.0, -40.0);
    test::addPoint(pointDoc, 40.0, -40.0);
    test::addPoint(pointDoc, 40.0, 40.0);
    test::addPoint(pointDoc, -40.0, 40.0);

    const ViewTransform fromCircle = fitView(Pose(circleDoc), W, H);
    const ViewTransform fromPoints = fitView(Pose(pointDoc), W, H);

    for(Point p : {Point{40.0, 0.0}, Point{-40.0, 0.0}, Point{0.0, 40.0}, Point{0.0, -40.0}}) {
        const Eigen::Vector2d a = fromCircle.toScreen(p);
        const Eigen::Vector2d b = fromPoints.toScreen(p);
        CHECK(a.x() == doctest::Approx(b.x()));
        CHECK(a.y() == doctest::Approx(b.y()));
        // And the rim lands on screen, which is the whole point of framing it.
        CHECK(a.x() >= 0.0);
        CHECK(a.x() <= W);
        CHECK(a.y() >= 0.0);
        CHECK(a.y() <= H);
    }
}
