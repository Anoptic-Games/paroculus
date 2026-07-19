// The inference corpus: precision and recall, held by a test.
//
// The gesture corpus next door covers what stage 3 commits to — drag locality,
// saturation, release-commits-seeds. This is the stage 4 half: given a gesture,
// which relations does the layer declare?
//
// Two failure modes, and they pull against each other. Recall is missing what
// the user meant: aiming at a vertex and getting geometry that merely sits near
// it, so the first drag peels it away. Precision is declaring what they did not
// mean: a sketch that fights back because a run drawn at a deliberate angle was
// quietly made horizontal. Helpful rigidity is the worse of the two, which is
// why the assertions here are mostly exact-set rather than contains.
//
// Every entry drives synthetic input through the interact layer with no toolkit
// present, and asserts the whole declared set rather than a member of it. An
// assertion that only checks what should be there cannot fail on what should
// not.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "core/topology.h"
#include "interact/session.h"
#include "solve/solve.h"
#include "support/build.h"

using namespace paroculus;
using paroculus::test::addPoint;
using paroculus::test::addSegment;

namespace {

Viewport inferenceViewport() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    Viewport v;
    v.view = ViewTransform(m);
    v.width = 800.0;
    v.height = 600.0;
    return v;
}

// One authoring gesture over a document the caller has already populated.
//
// The grid is off by default. It places and never declares, so leaving it on
// would move every click a few units before inference sees it and make each
// case a test of two things at once. The grid entry turns it back on.
struct Authoring {
    Document &doc;
    UndoJournal journal;
    Session session;

    Authoring(Document &document, ToolKind tool) : doc(document), session(document, journal) {
        session.setViewport(inferenceViewport());
        session.snapPolicy().gridEnabled = false;
        session.setTool(tool);
    }

    void moveTo(Point p) {
        session.handle(PointerEvent::at(PointerAction::Move,
                                        session.viewport().view.toScreen(p),
                                        session.viewport().view));
    }
    void clickAt(Point p) {
        moveTo(p);
        session.handle(PointerEvent::at(PointerAction::Press,
                                        session.viewport().view.toScreen(p),
                                        session.viewport().view, Button::Left));
    }

    // Every relation in the document, sorted. The whole set, because precision
    // is a claim about what is absent.
    std::vector<ConstraintKind> declared() const {
        std::vector<ConstraintKind> out;
        for(const ConstraintRecord &c : doc.constraints().records()) out.push_back(c.kind);
        std::sort(out.begin(), out.end());
        return out;
    }

    size_t countOf(ConstraintKind kind) const {
        const std::vector<ConstraintKind> all = declared();
        return static_cast<size_t>(std::count(all.begin(), all.end(), kind));
    }

    // Degrees of freedom over the component the seed belongs to, which is what
    // "the shape came out rigid" means when it is stated as a number.
    int dofAround(EntityId seed) const {
        Topology topology(doc);
        SolveContext context = SolveContext::forComponent(doc, topology, seed);
        const SolveOutcome outcome = solve(doc, context);
        return outcome.ok() ? outcome.dof : -1;
    }

    EntityId anySegment() const {
        for(const EntityRecord &e : doc.entities().records()) {
            if(e.kind == EntityKind::Segment) return e.id;
        }
        return EntityId();
    }
};

std::vector<ConstraintKind> sorted(std::vector<ConstraintKind> v) {
    std::sort(v.begin(), v.end());
    return v;
}

}  // namespace

TEST_CASE("inference: a sloppy rectangle comes out square") {
    // The thesis, as one gesture. Edges a degree or two off axis and a closing
    // click a few units adrift, drawn the way a hand actually draws — and the
    // result is rigid but for position and size.
    //
    // Ten parameters: five points at two each, less the four axis relations and
    // the two the closing coincidence removes. Four left: x, y, width, height.
    Document doc;
    Authoring g(doc, ToolKind::Rectangle);

    g.clickAt(Point{-97.0, -61.0});
    g.clickAt(Point{101.0, 63.0});

    CHECK(g.countOf(ConstraintKind::Horizontal) == 2);
    CHECK(g.countOf(ConstraintKind::Vertical) == 2);
    CHECK(g.countOf(ConstraintKind::Coincident) == 4);
    CHECK(g.dofAround(g.anySegment()) == 4);
}

TEST_CASE("inference recall: a click that means a vertex binds to it") {
    // Aiming at a vertex and getting geometry that merely sits near it is the
    // recall failure that matters most: nothing looks wrong until the first
    // drag peels the two apart, and by then the gesture is far behind.
    Document doc;
    const EntityId vertex = addPoint(doc, 40.0, 40.0);
    Authoring g(doc, ToolKind::Line);

    g.clickAt(Point{38.0, 41.0});   // within reach of the vertex
    g.clickAt(Point{140.0, 120.0});  // an angle that means nothing in particular

    CHECK(g.declared() == std::vector<ConstraintKind>{ConstraintKind::Coincident});
    // Bound to the vertex the user aimed at, not to some point of its own.
    const ConstraintRecord &c = doc.constraints().records().front();
    CHECK((c.operands[0] == vertex || c.operands[1] == vertex));
}

TEST_CASE("inference precision: near is not touching") {
    // The same gesture, further out. A vertex two spans away is scenery, and
    // declaring a coincidence to it would move geometry the user placed.
    Document doc;
    addPoint(doc, 40.0, 40.0);
    Authoring g(doc, ToolKind::Line);

    g.clickAt(Point{80.0, 75.0});
    g.clickAt(Point{180.0, 155.0});

    // The segment exists: "nothing declared" is inference staying silent, not a
    // gesture that never landed.
    REQUIRE(g.anySegment().valid());
    CHECK(g.declared().empty());
}

TEST_CASE("inference precision: a deliberate angle stays at it") {
    // A run drawn at an angle a hand chose is not a failed attempt at an axis.
    // Snapping it is the failure mode that makes a sketch fight back, and it is
    // worse than missing a relation because the user cannot see what happened.
    Document doc;
    Authoring g(doc, ToolKind::Line);

    g.clickAt(Point{-100.0, -60.0});
    g.clickAt(Point{0.0, 0.0});  // 31 degrees: nothing to mistake for an axis

    REQUIRE(g.anySegment().valid());
    CHECK(g.declared().empty());
}

TEST_CASE("inference: the axis tolerance has an edge, and it holds on both sides") {
    // A tolerance nobody has measured is a tolerance that drifts. Two runs of
    // the same length either side of it, and the assertion is the whole set on
    // each: inside declares horizontal and nothing else, outside declares
    // nothing at all.
    const double tolerance = SnapPolicy{}.angleTolerance;
    const double span = 200.0;

    for(double fraction : {0.5, 1.5}) {
        CAPTURE(fraction);
        const double degrees = tolerance * fraction;
        const double rise = span * std::tan(degrees * 3.14159265358979323846 / 180.0);

        Document doc;
        Authoring g(doc, ToolKind::Line);
        g.clickAt(Point{-100.0, 0.0});
        g.clickAt(Point{-100.0 + span, rise});

        REQUIRE(g.anySegment().valid());
        if(fraction < 1.0) {
            CHECK(g.declared() == std::vector<ConstraintKind>{ConstraintKind::Horizontal});
        } else {
            CHECK(g.declared().empty());
        }
    }
}

TEST_CASE("inference precision: the grid places and never declares") {
    // A document where every point is grid-pinned is rigidity by helpfulness.
    // The grid moves the click and says nothing about it, so the geometry is
    // free to be dragged off the grid the moment the user wants it off.
    Document doc;
    Authoring g(doc, ToolKind::Line);
    g.session.snapPolicy().gridEnabled = true;
    const double step = g.session.snapPolicy().gridStep;

    g.clickAt(Point{step * 2.0 + 1.0, step * 3.0 - 1.0});
    g.clickAt(Point{step * 5.0 + 1.0, step * 7.0 + 1.0});

    REQUIRE(g.anySegment().valid());
    CHECK(g.declared().empty());

    // And it did place: the ends landed on the grid rather than where the
    // cursor was.
    const EntityRecord *segment = nullptr;
    for(const EntityRecord &e : doc.entities().records()) {
        if(e.kind == EntityKind::Segment) segment = &e;
    }
    REQUIRE(segment != nullptr);
    const EntityRecord *end = doc.entities().find(segment->points[0]);
    REQUIRE(end != nullptr);
    CHECK(std::fmod(std::abs(end->seeds[0]), step) == doctest::Approx(0.0));
    CHECK(std::fmod(std::abs(end->seeds[1]), step) == doctest::Approx(0.0));
}

TEST_CASE("inference precision: construction geometry does not attract") {
    // An arc leaves a construction centre behind every time it is drawn, so a
    // sketch full of arcs would be a sketch full of magnets nobody aimed at.
    // Construction constrains normally and attracts never.
    Document doc;
    {
        Authoring arc(doc, ToolKind::Arc);
        arc.clickAt(Point{-60.0, 0.0});
        arc.clickAt(Point{60.0, 0.0});
        arc.clickAt(Point{0.0, 60.0});
    }

    // The centre the arc left behind, and a line drawn right past it.
    EntityId centre;
    for(const EntityRecord &e : doc.entities().records()) {
        if(e.kind == EntityKind::Arc) centre = e.points[0];
    }
    REQUIRE(centre.valid());
    const EntityRecord *centreRecord = doc.entities().find(centre);
    REQUIRE(centreRecord != nullptr);
    REQUIRE(centreRecord->role == Role::Construction);
    const Point at{centreRecord->seeds[0], centreRecord->seeds[1]};

    const size_t before = doc.constraints().size();
    Authoring g(doc, ToolKind::Line);
    g.clickAt(Point{at.x + 1.0, at.y - 1.0});  // all but on the construction centre
    g.clickAt(Point{at.x + 137.0, at.y + 94.0});

    // Nothing new: the arc's own point-on-circle is all there is.
    REQUIRE(g.anySegment().valid());
    CHECK(doc.constraints().size() == before);
}

TEST_CASE("inference: the offered tier withholds until the user supplies it") {
    // Confirmation is the user supplying the confidence the tier withheld. The
    // recall half of an offer is that it is generated; the precision half is
    // that generating it declares nothing.
    Document doc;
    const EntityId a = addPoint(doc, -100.0, -100.0);
    const EntityId b = addPoint(doc, 100.0, -42.0);
    addSegment(doc, a, b);

    SUBCASE("unconfirmed, it declares nothing") {
        Authoring g(doc, ToolKind::Line);
        g.clickAt(Point{-100.0, 40.0});
        g.moveTo(Point{100.0, 100.0});
        REQUIRE_FALSE(g.session.presentation().offers().empty());
        g.clickAt(Point{100.0, 100.0});
        REQUIRE(doc.entities().size() > 3);  // the run landed
        CHECK(g.declared().empty());
    }

    SUBCASE("confirmed, it declares exactly what was offered") {
        Authoring g(doc, ToolKind::Line);
        g.clickAt(Point{-100.0, 40.0});
        g.moveTo(Point{100.0, 100.0});
        REQUIRE_FALSE(g.session.presentation().offers().empty());
        g.session.confirmOffer(0);
        g.clickAt(Point{100.0, 100.0});
        CHECK(g.declared() == std::vector<ConstraintKind>{ConstraintKind::Parallel});
    }
}

TEST_CASE("inference: decline takes back one relation, not the stroke") {
    // Undo takes back the placement and everything it declared, because that
    // was one gesture. Decline is the finer step and it is its own undoable
    // one, which is what makes an over-eager inference cheap to correct.
    Document doc;
    addPoint(doc, 40.0, 40.0);
    Authoring g(doc, ToolKind::Line);

    g.clickAt(Point{38.0, 41.0});
    g.clickAt(Point{238.0, 41.0});  // starts on the vertex and runs flat

    const std::vector<ConstraintKind> both =
        sorted({ConstraintKind::Coincident, ConstraintKind::Horizontal});
    REQUIRE(g.declared() == both);
    const size_t entitiesBefore = doc.entities().size();

    g.session.declineInference(0);

    // One relation gone, the geometry untouched.
    CHECK(g.declared().size() == 1);
    CHECK(doc.entities().size() == entitiesBefore);

    // And it is its own step: undo brings back the relation, not the segment.
    g.session.handle(Key::Undo);
    CHECK(g.declared() == both);
    CHECK(doc.entities().size() == entitiesBefore);
}
