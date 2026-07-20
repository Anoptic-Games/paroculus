// Dimension text.
//
// A dimension is document-anchored and screen-scaled: it travels with the
// geometry it measures and stays the same size at every zoom, which are the two
// halves of the same rule every adorner follows. What is asserted here is that
// it is drawn at all, that it moves with its operands, and that a driving
// dimension and a reference measurement are visibly different — because they
// are the same object with a toggle, and a toggle nobody can see is not one.
//
// Sampled analytically rather than against a golden image. The typeface is
// pinned by the flake, so a golden would be meaningful, but "some pixels are lit
// near the anchor and none are far from it" is the property; which pixels is the
// font's business.
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "interact/glyphs.h"
#include "interact/registry.h"
#include "interact/session.h"
#include "render/typeface.h"
#include "render/view.h"
#include "support/build.h"

using namespace paroculus;
using namespace paroculus::test;

namespace {

constexpr int W = 600;
constexpr int H = 400;
constexpr uint32_t BACKGROUND = 0xff14161au;

ViewTransform centred() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(W * 0.5, H * 0.5));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    return ViewTransform(m);
}

std::vector<uint32_t> paint(const Pose &pose, const ViewTransform &view,
                            const Adornment &adornment) {
    std::vector<uint32_t> pixels(static_cast<size_t>(W) * H, 0u);
    renderDocument(pose, view, adornment, reinterpret_cast<uint8_t *>(pixels.data()), W, H,
                   static_cast<size_t>(W) * 4);
    return pixels;
}

// How many non-background pixels lie within `radius` of a screen point.
size_t litNear(const std::vector<uint32_t> &pixels, const Eigen::Vector2d &at, int radius) {
    size_t count = 0;
    for(int dy = -radius; dy <= radius; dy++) {
        for(int dx = -radius; dx <= radius; dx++) {
            const int x = int(at.x()) + dx;
            const int y = int(at.y()) + dy;
            if(x < 0 || y < 0 || x >= W || y >= H) continue;
            if(pixels[size_t(y) * W + size_t(x)] != BACKGROUND) count++;
        }
    }
    return count;
}

// A segment with a distance dimension on it, and a session to place the marks.
struct Dimensioned {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session;
    EntityId a, b;
    ConstraintId dimension;

    Dimensioned() {
        a = addPoint(doc, -80.0, 0.0);
        b = addPoint(doc, 80.0, 0.0);
        addSegment(doc, a, b);
        dimension = addConstraint(doc, ConstraintKind::PointPointDistance, {a, b}, Slot(160.0));

        session = std::make_unique<Session>(doc, journal);
        Viewport viewport;
        viewport.view = centred();
        viewport.width = W;
        viewport.height = H;
        session->setViewport(viewport);
    }

    Adornment adornment() const {
        Adornment out;
        out.glyphs = session->glyphs();
        return out;
    }
};

}  // namespace

TEST_CASE("the bundled typeface is compiled in") {
    // Refused at configure time when absent, so this is not a runtime
    // possibility so much as a statement of what the build guarantees.
    const std::span<const unsigned char> bytes = bundledTypefaceBytes();
    CHECK(bytes.size() > 1000);
    // A TrueType file starts with a version tag; 0x00010000 is the usual one.
    REQUIRE(bytes.size() >= 4);
    CHECK(bytes[0] == 0x00);
    CHECK(bytes[1] == 0x01);
}

TEST_CASE("a dimension draws its value on the geometry it measures") {
    Dimensioned d;
    const ViewTransform view = d.session->viewport().view;

    const std::vector<GlyphMark> marks = d.session->glyphs();
    REQUIRE(!marks.empty());
    const std::vector<Eigen::Vector2d> places = layOutGlyphs(marks, view);
    REQUIRE(!places.empty());

    const std::vector<uint32_t> pixels = paint(d.session->pose(), view, d.adornment());

    // Something is drawn where the mark was placed. A number is many more lit
    // pixels than a stroke glyph, so this is a generous floor rather than an
    // exact count — which pixels is the font's business.
    CHECK(litNear(pixels, places.front(), 12) > 20);

    // And nothing is drawn far from any geometry.
    CHECK(litNear(pixels, view.toScreen(Point{0.0, 150.0}), 8) == 0);
}

TEST_CASE("a dimension travels with what it measures") {
    // Document-anchored: the label is placed from the pose every frame, so it
    // moves with the geometry rather than being positioned once and left.
    Dimensioned d;
    const ViewTransform view = d.session->viewport().view;

    const Eigen::Vector2d before = layOutGlyphs(d.session->glyphs(), view).front();

    // Move the whole segment down. Written as a seed change, which is what a
    // committed drag is.
    for(EntityId id : {d.a, d.b}) {
        EntityRecord r = *d.doc.entities().find(id);
        r.seeds[1] -= 90.0;
        REQUIRE(d.doc.apply(SetRecord<EntityRecord>{r}).ok());
    }
    d.session->refresh();

    const Eigen::Vector2d after = layOutGlyphs(d.session->glyphs(), view).front();
    CHECK(std::fabs(after.y() - before.y()) > 50.0);

    // The label is where the geometry now is, and not where it was.
    const std::vector<uint32_t> pixels = paint(d.session->pose(), view, d.adornment());
    CHECK(litNear(pixels, after, 12) > 20);
    CHECK(litNear(pixels, before, 6) == 0);
}

TEST_CASE("a reference measurement reads differently from a driving one") {
    // The same object with a toggle. A reference shows what the geometry is
    // doing; a driving dimension shows the value it holds. A reference whose
    // slot still carried the last value it drove at would be a readout that
    // lies, which is the whole reason promotion re-captures.
    Dimensioned d;
    const ViewTransform view = d.session->viewport().view;
    const std::vector<uint32_t> driving = paint(d.session->pose(), view, d.adornment());

    d.session->selectConstraint(d.dimension);
    REQUIRE(invokeAction(*d.session, "relation.toggle-driving"));
    REQUIRE_FALSE(d.doc.constraints().find(d.dimension)->driving);

    const std::vector<uint32_t> reference = paint(d.session->pose(), view, d.adornment());
    CHECK(driving != reference);
}

TEST_CASE("a relation with no value keeps its stroke glyph") {
    // Only the valued kinds draw a number. A horizontal has nothing to say in
    // digits, and replacing its mark with an empty label would make it
    // invisible from the geometry it binds.
    Document doc;
    const EntityId a = addPoint(doc, -60.0, 0.0);
    const EntityId b = addPoint(doc, 60.0, 0.0);
    const EntityId segment = addSegment(doc, a, b);
    addConstraint(doc, ConstraintKind::Horizontal, {segment});

    UndoJournal journal;
    Session session(doc, journal);
    Viewport viewport;
    viewport.view = centred();
    viewport.width = W;
    viewport.height = H;
    session.setViewport(viewport);

    Adornment adornment;
    adornment.glyphs = session.glyphs();
    REQUIRE(!adornment.glyphs.empty());

    const std::vector<Eigen::Vector2d> places =
        layOutGlyphs(adornment.glyphs, viewport.view);
    const std::vector<uint32_t> pixels = paint(session.pose(), viewport.view, adornment);
    CHECK(litNear(pixels, places.front(), 10) > 0);
}
