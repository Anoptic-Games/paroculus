// Analytic sampling of the composition: layers, punch-through, region algebra.
//
// Occlusion and cut-outs are compositional, never destructive. What that means
// on screen is what is sampled here — a punch takes away what its layer had
// accumulated and nothing else, a composite draws the combination of operands
// that both still exist, and permuting the layer order permutes the result and
// leaves the document alone.
//
// Sampled at points rather than against a golden image: the property is which
// side of an edge a pixel falls on, and a golden would additionally freeze
// every colour choice into a test that is not about colour.
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "core/composition.h"
#include "core/persist.h"
#include "render/view.h"
#include "support/build.h"

using namespace paroculus;
using namespace paroculus::test;

namespace {

constexpr int W = 400;
constexpr int H = 300;
constexpr uint32_t BACKGROUND = 0xff14161au;

std::vector<uint32_t> paint(const Pose &pose, const ViewTransform &view,
                            const Adornment &adornment = {}) {
    std::vector<uint32_t> pixels(static_cast<size_t>(W) * H, 0u);
    renderDocument(pose, view, adornment, reinterpret_cast<uint8_t *>(pixels.data()), W, H,
                   static_cast<size_t>(W) * 4);
    return pixels;
}

ViewTransform centredView() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(W * 0.5, H * 0.5));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    return ViewTransform(m);
}

uint32_t at(const std::vector<uint32_t> &pixels, const ViewTransform &view, Point p) {
    const Eigen::Vector2d s = view.toScreen(p);
    const int x = static_cast<int>(s.x());
    const int y = static_cast<int>(s.y());
    if(x < 0 || y < 0 || x >= W || y >= H) return BACKGROUND;
    return pixels[static_cast<size_t>(y) * W + x];
}

bool painted(const std::vector<uint32_t> &pixels, const ViewTransform &view, Point p) {
    return at(pixels, view, p) != BACKGROUND;
}

// An axis-aligned filled rectangle, its corners separate points joined by
// coincidence — what a rectangle actually is in this model.
RegionId addFilledRect(Document &doc, double cx, double cy, double halfW, double halfH,
                       LayerId layer = LayerId(), int32_t z = 0, bool punch = false) {
    const Point corners[4] = {{cx - halfW, cy - halfH},
                              {cx + halfW, cy - halfH},
                              {cx + halfW, cy + halfH},
                              {cx - halfW, cy + halfH}};
    std::vector<EntityId> edges;
    EntityId ends[4][2];
    for(int i = 0; i < 4; i++) {
        const Point a = corners[i];
        const Point b = corners[(i + 1) % 4];
        ends[i][0] = addPoint(doc, a.x, a.y);
        ends[i][1] = addPoint(doc, b.x, b.y);
        edges.push_back(addSegment(doc, ends[i][0], ends[i][1]));
    }
    for(int i = 0; i < 4; i++) {
        addConstraint(doc, ConstraintKind::Coincident, {ends[i][1], ends[(i + 1) % 4][0]});
    }
    for(EntityId e : edges) {
        EntityRecord r = *doc.entities().find(e);
        r.layer = layer;
        REQUIRE(doc.apply(SetRecord<EntityRecord>{r}).ok());
    }
    for(int i = 0; i < 4; i++) {
        for(EntityId p : {ends[i][0], ends[i][1]}) {
            EntityRecord r = *doc.entities().find(p);
            r.layer = layer;
            REQUIRE(doc.apply(SetRecord<EntityRecord>{r}).ok());
        }
    }

    RegionRecord region;
    region.boundary = edges;
    region.layer = layer;
    region.z = z;
    region.punch = punch;
    const CommandResult result = doc.apply(AddRecord<RegionRecord>{region});
    REQUIRE(result.ok());
    return RegionId(result.allocated);
}

StyleId addOpaque(Document &doc, uint32_t colour) {
    StyleRecord s;
    s.name = "flat";
    s.filled = true;
    s.fillColor = colour;
    const CommandResult r = doc.apply(AddRecord<StyleRecord>{s});
    REQUIRE(r.ok());
    return StyleId(r.allocated);
}

void styleRegion(Document &doc, RegionId region, StyleId style) {
    RegionRecord r = *doc.regions().find(region);
    r.style = style;
    REQUIRE(doc.apply(SetRecord<RegionRecord>{r}).ok());
}

LayerId addLayer(Document &doc, const char *name, int32_t order) {
    LayerRecord l;
    l.name = name;
    l.order = order;
    const CommandResult r = doc.apply(AddRecord<LayerRecord>{l});
    REQUIRE(r.ok());
    return LayerId(r.allocated);
}

}  // namespace

TEST_CASE("a punch takes away what its layer accumulated, and nothing else") {
    // Alpha overwrite: the cut-out half of the layering thesis. The shape stays
    // a live constrained object, and what it hides is hidden rather than gone.
    Document doc;
    const RegionId plate = addFilledRect(doc, 0.0, 0.0, 60.0, 40.0);
    styleRegion(doc, plate, addOpaque(doc, 0xff2266ccu));

    const ViewTransform view = centredView();
    const std::vector<uint32_t> solid = paint(Pose(doc), view);
    CHECK(painted(solid, view, Point{0.0, 0.0}));

    // A smaller rectangle above it, punching.
    const RegionId hole = addFilledRect(doc, 0.0, 0.0, 20.0, 15.0, LayerId(), 1, true);
    const std::vector<uint32_t> cut = paint(Pose(doc), view);

    // Transparent where punched, and the plate is untouched outside it.
    CHECK_FALSE(painted(cut, view, Point{0.0, 0.0}));
    CHECK(at(cut, view, Point{45.0, 0.0}) == at(solid, view, Point{45.0, 0.0}));

    // Lifting the cut-out out restores exactly what it was hiding: nothing was
    // consumed to make the hole. The outline that was doing the cutting is
    // still there and still drawn — deleting the region deletes the fill, not
    // the geometry, which is the same inverse make-solid has.
    for(const Command &c : deletionStep(doc, hole)) REQUIRE(doc.apply(c).ok());
    const std::vector<uint32_t> restored = paint(Pose(doc), view);
    CHECK(at(restored, view, Point{0.0, 0.0}) == at(solid, view, Point{0.0, 0.0}));
    CHECK(at(restored, view, Point{45.0, 0.0}) == at(solid, view, Point{45.0, 0.0}));
}

TEST_CASE("a punch does not cut through the layer below it") {
    // The cut is against what this layer has accumulated. Cutting to the canvas
    // would make a hole's effect depend on what happened to be underneath,
    // which is the destructive reading of a boolean rather than the
    // compositional one.
    Document doc;
    const LayerId under = addLayer(doc, "under", 0);
    const LayerId over = addLayer(doc, "over", 1);
    const RegionId back = addFilledRect(doc, 0.0, 0.0, 70.0, 50.0, under);
    styleRegion(doc, back, addOpaque(doc, 0xff993322u));
    const RegionId front = addFilledRect(doc, 0.0, 0.0, 50.0, 35.0, over);
    styleRegion(doc, front, addOpaque(doc, 0xff2266ccu));
    addFilledRect(doc, 0.0, 0.0, 20.0, 15.0, over, 1, true);

    const ViewTransform view = centredView();
    const std::vector<uint32_t> pixels = paint(Pose(doc), view);

    // Through the front layer, and the back layer still shows.
    CHECK(painted(pixels, view, Point{0.0, 0.0}));
    CHECK(at(pixels, view, Point{0.0, 0.0}) == at(pixels, view, Point{65.0, 0.0}));
}

TEST_CASE("a composite draws the combination, not its operands") {
    Document doc;
    const RegionId plate = addFilledRect(doc, 0.0, 0.0, 60.0, 40.0);
    const RegionId hole = addFilledRect(doc, 0.0, 0.0, 20.0, 15.0);
    const StyleId ink = addOpaque(doc, 0xff2266ccu);
    styleRegion(doc, plate, ink);
    styleRegion(doc, hole, ink);

    const ViewTransform view = centredView();
    RegionRecord composite;
    composite.op = CompositeOp::Subtract;
    composite.operands = {plate, hole};
    composite.style = ink;
    REQUIRE(doc.apply(AddRecord<RegionRecord>{composite}).ok());
    const RegionId cut = doc.regions().records().back().id;

    const std::vector<uint32_t> pixels = paint(Pose(doc), view);
    // Inside the plate but outside the hole: filled. Inside the hole: not.
    CHECK(painted(pixels, view, Point{45.0, 0.0}));
    CHECK_FALSE(painted(pixels, view, Point{0.0, 0.0}));

    // Intersect over the same operands is the hole's area and nothing else.
    RegionRecord flipped = *doc.regions().find(cut);
    flipped.op = CompositeOp::Intersect;
    REQUIRE(doc.apply(SetRecord<RegionRecord>{flipped}).ok());
    const std::vector<uint32_t> both = paint(Pose(doc), view);
    CHECK(painted(both, view, Point{0.0, 0.0}));
    CHECK_FALSE(painted(both, view, Point{45.0, 0.0}));
}

TEST_CASE("a composite follows its operands, having no geometry of its own") {
    // The same claim segments-to-solid makes, one level up: dragging a vertex
    // of the cutter moves the hole, because the hole was never anything but
    // this computation.
    Document doc;
    const RegionId plate = addFilledRect(doc, 0.0, 0.0, 60.0, 40.0);
    const RegionId hole = addFilledRect(doc, 0.0, 0.0, 20.0, 15.0);
    const StyleId ink = addOpaque(doc, 0xff2266ccu);
    styleRegion(doc, plate, ink);
    styleRegion(doc, hole, ink);
    RegionRecord composite;
    composite.op = CompositeOp::Subtract;
    composite.operands = {plate, hole};
    composite.style = ink;
    REQUIRE(doc.apply(AddRecord<RegionRecord>{composite}).ok());

    const ViewTransform view = centredView();
    CHECK_FALSE(painted(paint(Pose(doc), view), view, Point{0.0, 0.0}));

    // Slide the cutter right, through the pose rather than through the region.
    Pose moved(doc);
    std::vector<SeedSpan> shifted;
    for(EntityId e : doc.regions().find(hole)->boundary) {
        const EntityRecord *edge = doc.entities().find(e);
        for(size_t i = 0; i < 2; i++) {
            const EntityRecord *p = doc.entities().find(edge->points[i]);
            SeedSpan span;
            span.entity = p->id;
            span.seeds = p->seeds;
            span.seeds[0] += 35.0;
            shifted.push_back(span);
        }
    }
    moved.overlay(shifted);

    const std::vector<uint32_t> after = paint(moved, view);
    CHECK(painted(after, view, Point{0.0, 0.0}));
    CHECK_FALSE(painted(after, view, Point{35.0, 0.0}));
}

TEST_CASE("layer order permutes the result and leaves the document alone") {
    Document doc;
    const LayerId red = addLayer(doc, "red", 0);
    const LayerId blue = addLayer(doc, "blue", 1);
    const RegionId a = addFilledRect(doc, -10.0, 0.0, 40.0, 40.0, red);
    const RegionId b = addFilledRect(doc, 10.0, 0.0, 40.0, 40.0, blue);
    styleRegion(doc, a, addOpaque(doc, 0xffcc3322u));
    styleRegion(doc, b, addOpaque(doc, 0xff2233ccu));

    const ViewTransform view = centredView();
    const uint32_t blueOnTop = at(paint(Pose(doc), view), view, Point{0.0, 0.0});

    LayerRecord over = *doc.layers().find(red);
    over.order = 5;
    REQUIRE(doc.apply(SetRecord<LayerRecord>{over}).ok());
    const uint32_t redOnTop = at(paint(Pose(doc), view), view, Point{0.0, 0.0});

    CHECK(blueOnTop != redOnTop);
    // Where they do not overlap, order changes nothing.
    const std::vector<uint32_t> pixels = paint(Pose(doc), view);
    CHECK(painted(pixels, view, Point{-40.0, 0.0}));
    CHECK(painted(pixels, view, Point{40.0, 0.0}));
}

TEST_CASE("z orders fills within one layer") {
    Document doc;
    const RegionId low = addFilledRect(doc, -10.0, 0.0, 40.0, 40.0, LayerId(), 0);
    const RegionId high = addFilledRect(doc, 10.0, 0.0, 40.0, 40.0, LayerId(), 1);
    styleRegion(doc, low, addOpaque(doc, 0xffcc3322u));
    styleRegion(doc, high, addOpaque(doc, 0xff2233ccu));

    const ViewTransform view = centredView();
    const uint32_t highOnTop = at(paint(Pose(doc), view), view, Point{0.0, 0.0});

    RegionRecord raise = *doc.regions().find(low);
    raise.z = 9;
    REQUIRE(doc.apply(SetRecord<RegionRecord>{raise}).ok());
    CHECK(at(paint(Pose(doc), view), view, Point{0.0, 0.0}) != highOnTop);
}

TEST_CASE("a hidden layer draws nothing and constrains everything") {
    Document doc;
    const LayerId hidden = addLayer(doc, "hidden", 0);
    const RegionId region = addFilledRect(doc, 0.0, 0.0, 50.0, 35.0, hidden);
    styleRegion(doc, region, addOpaque(doc, 0xff2266ccu));

    const ViewTransform view = centredView();
    CHECK(painted(paint(Pose(doc), view), view, Point{0.0, 0.0}));

    const size_t relations = doc.constraints().size();
    LayerRecord l = *doc.layers().find(hidden);
    l.visible = false;
    REQUIRE(doc.apply(SetRecord<LayerRecord>{l}).ok());

    const std::vector<uint32_t> pixels = paint(Pose(doc), view);
    CHECK_FALSE(painted(pixels, view, Point{0.0, 0.0}));
    CHECK_FALSE(painted(pixels, view, Point{0.0, 35.0}));
    // Hiding is not deleting. Every relation is exactly where it was.
    CHECK(doc.constraints().size() == relations);
}

TEST_CASE("a broken region draws a diagnostic, never a partial fill") {
    // The fill the user asked for is still declared and still visible as
    // something, so a deletion neither blocks nor evaporates.
    Document doc;
    const RegionId region = addFilledRect(doc, 0.0, 0.0, 50.0, 35.0);
    styleRegion(doc, region, addOpaque(doc, 0xff2266ccu));

    const ViewTransform view = centredView();
    CHECK(painted(paint(Pose(doc), view), view, Point{0.0, 0.0}));

    RegionRecord broken = *doc.regions().find(region);
    const EntityId lost = broken.boundary.back();
    broken.boundary.pop_back();
    REQUIRE(doc.apply(SetRecord<RegionRecord>{broken}).ok());
    REQUIRE(regionState(doc, region) == RegionState::Broken);

    const std::vector<uint32_t> pixels = paint(Pose(doc), view);
    // No interior: an area the document does not bound is not drawn as one.
    CHECK_FALSE(painted(pixels, view, Point{0.0, 0.0}));
    // But the edges it still names are marked, so the broken fill is visible
    // rather than silently absent.
    const std::optional<std::pair<Point, Point>> kept =
        Pose(doc).segment(doc.regions().find(region)->boundary.front());
    REQUIRE(kept);
    const Point middle{(kept->first.x + kept->second.x) * 0.5,
                       (kept->first.y + kept->second.y) * 0.5};
    CHECK(painted(pixels, view, middle));
    CHECK(doc.entities().contains(lost));
}

TEST_CASE("opacity is a slot, so one parameter drives every fill") {
    // The whole reason styling values are slots rather than doubles: a named
    // document parameter reaches them, and scrubbing it is an ordinary value
    // edit rather than a styling system of its own.
    Document doc;
    ParameterRecord p;
    p.name = "wash";
    p.value = Slot(1.0);
    const CommandResult made = doc.apply(AddRecord<ParameterRecord>{p});
    REQUIRE(made.ok());

    StyleRecord s;
    s.name = "washed";
    s.filled = true;
    s.fillColor = 0xff2266ccu;
    s.opacity = Slot::parameter(ParameterId(made.allocated));
    const CommandResult style = doc.apply(AddRecord<StyleRecord>{s});
    REQUIRE(style.ok());

    const RegionId region = addFilledRect(doc, 0.0, 0.0, 50.0, 35.0);
    styleRegion(doc, region, StyleId(style.allocated));

    const ViewTransform view = centredView();
    const uint32_t opaque = at(paint(Pose(doc), view), view, Point{0.0, 0.0});

    ParameterRecord faded = *doc.parameters().find(ParameterId(made.allocated));
    faded.value = Slot(0.25);
    REQUIRE(doc.apply(SetRecord<ParameterRecord>{faded}).ok());
    const uint32_t washed = at(paint(Pose(doc), view), view, Point{0.0, 0.0});

    CHECK(opaque != washed);
    CHECK(painted(paint(Pose(doc), view), view, Point{0.0, 0.0}));
}
