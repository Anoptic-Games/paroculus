#include <doctest/doctest.h>

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
    // A corner well away from geometry and off the 20-unit grid.
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
    // Outside it, background survives — sampled off the 20-unit grid, which
    // now covers the whole viewport rather than a fixed range.
    CHECK(at(pixels, Eigen::Vector2d(210.0, 20.0)) == BACKGROUND);
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
