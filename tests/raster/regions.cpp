// Analytic sampling of region fills.
//
// The property under test is the one that makes segments-to-solid an identity
// rather than a conversion: the fill has no geometry of its own. There is no
// stale-geometry case to test because there is nothing that could go stale —
// so the way to assert it is to move the outline and sample where the fill
// then is, which is what these do.
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "core/composition.h"
#include "interact/session.h"
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

// Whether the pixel at document point `p` differs from the background.
bool painted(const std::vector<uint32_t> &pixels, const ViewTransform &view, Point p) {
    const Eigen::Vector2d s = view.toScreen(p);
    const int x = static_cast<int>(s.x());
    const int y = static_cast<int>(s.y());
    if(x < 0 || y < 0 || x >= W || y >= H) return false;
    return pixels[static_cast<size_t>(y) * W + x] != BACKGROUND;
}

// A square outline with a region over it, built through commands so the
// fixture can never set up a state the command layer would refuse.
struct FilledSquare {
    Document doc;
    std::vector<EntityId> corners;
    RegionId region;

    FilledSquare() {
        const EntityId a = addPoint(doc, -50.0, -40.0);
        const EntityId b = addPoint(doc, 50.0, -40.0);
        const EntityId c = addPoint(doc, 50.0, 40.0);
        const EntityId d = addPoint(doc, -50.0, 40.0);
        corners = {a, b, c, d};
        const EntityId e0 = addSegment(doc, a, b);
        const EntityId e1 = addSegment(doc, b, c);
        const EntityId e2 = addSegment(doc, c, d);
        const EntityId e3 = addSegment(doc, d, a);

        RegionRecord r;
        r.boundary = {e0, e1, e2, e3};
        const CommandResult result = doc.apply(AddRecord<RegionRecord>{r});
        REQUIRE(result.ok());
        region = RegionId(result.allocated);
    }
};

}  // namespace

TEST_CASE("a region fills inside its boundary and not outside it") {
    FilledSquare f;
    const ViewTransform view = centredView();
    const std::vector<uint32_t> pixels = paint(Pose(f.doc), view);

    // Well inside, away from every edge.
    CHECK(painted(pixels, view, Point{0.0, 0.0}));
    CHECK(painted(pixels, view, Point{-40.0, 30.0}));
    CHECK(painted(pixels, view, Point{40.0, -30.0}));

    // Well outside, away from every edge.
    CHECK_FALSE(painted(pixels, view, Point{0.0, 60.0}));
    CHECK_FALSE(painted(pixels, view, Point{-70.0, 0.0}));
    CHECK_FALSE(painted(pixels, view, Point{70.0, 60.0}));
}

TEST_CASE("moving a vertex moves the fill, because there is nothing else to move") {
    // The whole equivalence, sampled. No geometry was copied when the region
    // was attached, so there is no second representation to keep in step — the
    // path is walked from the pose every frame.
    FilledSquare f;
    const ViewTransform view = centredView();

    // A point just outside the top-left corner: background now.
    const Point probe{-62.0, 30.0};
    CHECK_FALSE(painted(paint(Pose(f.doc), view), view, probe));

    // Move the left edge out past the probe. Written as a seed change, which is
    // what a committed drag is.
    for(EntityId id : {f.corners[0], f.corners[3]}) {
        EntityRecord r = *f.doc.entities().find(id);
        r.seeds[0] = -75.0;
        REQUIRE(f.doc.apply(SetRecord<EntityRecord>{r}).ok());
    }

    // The fill followed. Nothing was told to update it.
    CHECK(painted(paint(Pose(f.doc), view), view, probe));
    CHECK(f.doc.regions().find(f.region)->boundary.size() == 4);
}

TEST_CASE("a region whose boundary lost an edge draws nothing rather than a lie") {
    // Degrading the render into a diagnostic state is stage 6's work. What
    // stage 5 owes is that a boundary which cannot be walked is not filled as
    // though it could be — a partial fill would show an area the document does
    // not bound.
    FilledSquare f;
    const ViewTransform view = centredView();
    CHECK(painted(paint(Pose(f.doc), view), view, Point{0.0, 0.0}));

    RegionRecord r = *f.doc.regions().find(f.region);
    r.boundary.pop_back();
    REQUIRE(f.doc.apply(SetRecord<RegionRecord>{r}).ok());

    const std::vector<uint32_t> pixels = paint(Pose(f.doc), view);
    CHECK_FALSE(painted(pixels, view, Point{0.0, 0.0}));
    // The outline is untouched, which is the other half of never blocking a
    // deletion: the geometry survives whatever happened to the fill.
    CHECK(painted(pixels, view, Point{0.0, -40.0}));
}

TEST_CASE("the fill sits under the outline rather than over it") {
    FilledSquare f;
    const ViewTransform view = centredView();
    const std::vector<uint32_t> pixels = paint(Pose(f.doc), view);

    // On an edge, the stroke colour wins. Sampling the exact edge pixel is
    // fragile, so this asserts the weaker and sufficient thing: the edge is
    // painted, and painted differently from the interior.
    const Eigen::Vector2d onEdge = view.toScreen(Point{0.0, -40.0});
    const Eigen::Vector2d inside = view.toScreen(Point{0.0, 0.0});
    const uint32_t edgePixel =
        pixels[static_cast<size_t>(onEdge.y()) * W + static_cast<size_t>(onEdge.x())];
    const uint32_t insidePixel =
        pixels[static_cast<size_t>(inside.y()) * W + static_cast<size_t>(inside.x())];
    CHECK(edgePixel != BACKGROUND);
    CHECK(insidePixel != BACKGROUND);
    CHECK(edgePixel != insidePixel);
}

TEST_CASE("a fill tints as selected on the rule the region actions use") {
    // One question, one answer. Highlighting the fill when any single boundary
    // edge was selected, while punch, raise, lower and subtract all required
    // every edge, meant a user could watch a fill light up and then watch every
    // action on it refuse.
    FilledSquare f;
    const ViewTransform view = centredView();
    const RegionRecord &region = *f.doc.regions().find(f.region);

    const std::vector<uint32_t> plain = paint(Pose(f.doc), view);

    Adornment partial;
    partial.selected = {region.boundary.front()};
    const std::vector<uint32_t> one = paint(Pose(f.doc), view, partial);

    Adornment whole;
    whole.selected = region.boundary;
    const std::vector<uint32_t> all = paint(Pose(f.doc), view, whole);

    // Sampled well inside, where only the fill is drawn — the selected edges
    // themselves tint either way, and that is not what is being asked.
    const Eigen::Vector2d middle = view.toScreen(Point{0.0, 0.0});
    const size_t at = static_cast<size_t>(static_cast<int>(middle.y())) * W +
                      static_cast<size_t>(static_cast<int>(middle.x()));
    CHECK(one[at] == plain[at]);
    CHECK(all[at] != plain[at]);

    // And it is core's rule that says so, which is what makes the two agree.
    CHECK_FALSE(regionSelected(f.doc, region, partial.selected));
    CHECK(regionSelected(f.doc, region, whole.selected));
}

TEST_CASE("a previewed relation ghosts where it would put the geometry") {
    // The payoff the speculative solve was always for. The pose was computed,
    // carried on ImpositionPreview, and thrown away by a surface that reduced
    // the whole preview to a verdict string — so the catalogue stayed something
    // to be read rather than something learnable by looking.
    FilledSquare f;
    const ViewTransform view = centredView();
    const std::vector<uint32_t> plain = paint(Pose(f.doc), view);

    // A pose that moves one corner well clear of the square.
    Adornment ghosted;
    SeedSpan moved;
    moved.entity = f.corners[2];
    moved.seeds = {90.0, 70.0};
    ghosted.ghostPose = {moved};
    const std::vector<uint32_t> preview = paint(Pose(f.doc), view, ghosted);

    // The two edges that corner defines are drawn where they would land — the
    // midpoints of each, both well clear of anything the square drew.
    for(Point on : {Point{20.0, 55.0}, Point{70.0, 15.0}}) {
        CHECK_FALSE(painted(plain, view, on));
        CHECK(painted(preview, view, on));
    }

    // And the document is untouched: a ghost is a promise, not an edit.
    CHECK(f.doc.entities().find(f.corners[2])->seeds[0] == 50.0);

    // A preview that moves nothing draws nothing extra. Imposition is
    // movement-free by design, so a ghost that redrew the whole document on top
    // of itself would be noise on almost every hover.
    Adornment unmoved;
    SeedSpan same;
    same.entity = f.corners[2];
    same.seeds = {50.0, 40.0};
    unmoved.ghostPose = {same};
    CHECK(paint(Pose(f.doc), view, unmoved) == plain);
}

TEST_CASE("fewer than three edges fill nothing") {
    Document doc;
    const EntityId a = addPoint(doc, -40.0, 0.0);
    const EntityId b = addPoint(doc, 40.0, 0.0);
    RegionRecord r;
    r.boundary = {addSegment(doc, a, b)};
    REQUIRE(doc.apply(AddRecord<RegionRecord>{r}).ok());

    const ViewTransform view = centredView();
    const std::vector<uint32_t> pixels = paint(Pose(doc), view);
    CHECK_FALSE(painted(pixels, view, Point{0.0, 20.0}));
    CHECK_FALSE(painted(pixels, view, Point{0.0, -20.0}));
}
