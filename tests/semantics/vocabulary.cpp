// The U1 vocabulary as properties.
//
// The new families each make one claim worth pinning before a surface binds it:
// retarget is the same rewrite rotate performs; a style edit forks or mutates by
// what shares the style; a set-value that cannot hold refuses byte-identically
// and offers the downgrade; a parameter cycle is caught before it commits. Each
// is checkable headlessly, which is what makes the surfaces above them cheap to
// trust.
#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "core/measure.h"
#include "core/persist.h"
#include "core/pose.h"
#include "core/transform.h"
#include "interact/registry.h"
#include "interact/session.h"
#include "support/build.h"

using namespace paroculus;
using namespace paroculus::test;

namespace {

Viewport vocabViewport() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    Viewport v;
    v.view = ViewTransform(m);
    v.width = 800.0;
    v.height = 600.0;
    return v;
}

double worstResidual(const Document &doc) {
    const Pose pose(doc);
    double worst = 0.0;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        if(const std::optional<double> r = residual(pose, c)) worst = std::max(worst, std::abs(*r));
    }
    return worst;
}

bool applyAll(Document &doc, const std::vector<Command> &commands) {
    for(const Command &c : commands) {
        if(!doc.apply(c).ok()) return false;
    }
    return true;
}

StyleId addStyle(Document &doc, uint32_t stroke) {
    StyleRecord style;
    style.strokeColor = stroke;
    return StyleId(doc.apply(AddRecord<StyleRecord>{style}).allocated);
}

void setStyle(Document &doc, EntityId entity, StyleId style) {
    EntityRecord e = *doc.entities().find(entity);
    e.style = style;
    doc.apply(SetRecord<EntityRecord>{std::move(e)});
}

}  // namespace

TEST_CASE("retarget to a new cluster frame is rotate at zero degrees") {
    // A cluster carrying a horizontal and a vertical, document-framed: exactly
    // what a retarget makes its own.
    Document doc;
    const EntityId p0 = addPoint(doc, 0.0, 0.0);
    const EntityId p1 = addPoint(doc, 40.0, 0.0);
    const EntityId p2 = addPoint(doc, 40.0, 30.0);
    const EntityId bottom = addSegment(doc, p0, p1);
    const EntityId right = addSegment(doc, p1, p2);
    addConstraint(doc, ConstraintKind::Horizontal, {bottom});
    addConstraint(doc, ConstraintKind::Vertical, {right});
    const std::vector<EntityId> selection = {bottom, right};

    const Point centre{20.0, 15.0};
    RetargetOptions ro;
    ro.centre = centre;
    ro.target = RetargetTarget::NewClusterFrame;
    const TransformStep retarget = retargetAxesStep(doc, selection, ro);

    RotateOptions rot;
    rot.centre = centre;
    rot.angle = 0.0;
    rot.axes = AxisAnswer::RetargetToClusterFrame;
    const TransformStep rotated = rotateStep(doc, selection, rot);

    REQUIRE(retarget.ok());
    CHECK(retarget.retargeted == 2);
    CHECK(retarget.frame.valid());

    // The two rewrites land on the same document, byte for byte — the parity the
    // second entrance exists to guarantee.
    Document afterRetarget = doc;
    Document afterRotate = doc;
    REQUIRE(applyAll(afterRetarget, retarget.commands));
    REQUIRE(applyAll(afterRotate, rotated.commands));
    CHECK(afterRetarget == afterRotate);

    // And it is an isometry: every internal residual is exactly zero before any
    // re-solve, because a zero-degree rotation moves no seed and the new frame is
    // parallel to the axes the relations already held.
    CHECK(worstResidual(afterRetarget) == doctest::Approx(0.0).epsilon(1e-12));
}

TEST_CASE("relation.retarget-to-document applies only to a clustered selection and sheds it") {
    // The second inspector target: back to the document frame. It dims when there
    // is nothing clustered to shed and applies exactly when there is, so a row
    // that reads runnable is runnable.
    Document doc;
    UndoJournal journal;
    Session session(doc, journal);
    const EntityId p0 = addPoint(doc, 0.0, 0.0);
    const EntityId p1 = addPoint(doc, 40.0, 0.0);
    const EntityId seg = addSegment(doc, p0, p1);
    const ConstraintId h = addConstraint(doc, ConstraintKind::Horizontal, {seg});
    session.refresh();
    session.select({seg});

    const Action *toDoc = findAction("relation.retarget-to-document");
    REQUIRE(toDoc != nullptr);
    // Document-framed: nothing to shed, so the row is dimmed.
    CHECK_FALSE(toDoc->applicable(contextOf(session), *toDoc));

    // Cluster it via the new-frame retarget, then re-select the segment.
    REQUIRE(invokeAction(session, "relation.retarget-axes"));
    session.select({seg});
    // Now there is a clustered relation to shed, so the row is live and runs.
    CHECK(toDoc->applicable(contextOf(session), *toDoc));
    REQUIRE(invokeAction(session, "relation.retarget-to-document"));
    CHECK(boundOperandCount(*doc.constraints().find(h)) ==
          constraintInfo(ConstraintKind::Horizontal).operandCount);
}

TEST_CASE("retarget back to the document frame sheds the reference") {
    // Round-trip: to a cluster frame and back to the document frame leaves the
    // relations document-framed again.
    Document doc;
    const EntityId p0 = addPoint(doc, 0.0, 0.0);
    const EntityId p1 = addPoint(doc, 40.0, 0.0);
    const EntityId bottom = addSegment(doc, p0, p1);
    const ConstraintId h = addConstraint(doc, ConstraintKind::Horizontal, {bottom});
    const std::vector<EntityId> selection = {bottom};

    RetargetOptions toFrame;
    toFrame.centre = Point{20.0, 0.0};
    toFrame.target = RetargetTarget::NewClusterFrame;
    const TransformStep step = retargetAxesStep(doc, selection, toFrame);
    REQUIRE(step.ok());
    REQUIRE(applyAll(doc, step.commands));
    REQUIRE(boundOperandCount(*doc.constraints().find(h)) >
            constraintInfo(ConstraintKind::Horizontal).operandCount);

    RetargetOptions toDoc;
    toDoc.centre = Point{20.0, 0.0};
    toDoc.target = RetargetTarget::DocumentFrame;
    const TransformStep back = retargetAxesStep(doc, selection, toDoc);
    REQUIRE(back.ok());
    CHECK(back.retargeted == 1);
    REQUIRE(applyAll(doc, back.commands));
    CHECK(boundOperandCount(*doc.constraints().find(h)) ==
          constraintInfo(ConstraintKind::Horizontal).operandCount);
}

TEST_CASE("a style edit forks or mutates by what shares the style") {
    SUBCASE("an unstyled selection forks one shared style") {
        Document doc;
        const EntityId s1 = addSegment(doc, addPoint(doc, 0, 0), addPoint(doc, 40, 0));
        const EntityId s2 = addSegment(doc, addPoint(doc, 0, 10), addPoint(doc, 40, 10));
        UndoJournal journal;
        Session session(doc, journal);
        session.setViewport(vocabViewport());
        session.select({s1, s2});
        REQUIRE(session.setStyleStroke(0xffff0000u));
        REQUIRE(session.document().styles().size() == 1);
        const StyleId forked = session.document().styles().records().front().id;
        CHECK(session.document().entities().find(s1)->style == forked);
        CHECK(session.document().entities().find(s2)->style == forked);
        CHECK(session.document().styles().records().front().strokeColor == 0xffff0000u);
        CHECK(session.presentation().styleForked == 1);
    }

    SUBCASE("a style exclusive to the selection mutates in place") {
        Document doc;
        const EntityId s1 = addSegment(doc, addPoint(doc, 0, 0), addPoint(doc, 40, 0));
        const EntityId s2 = addSegment(doc, addPoint(doc, 0, 10), addPoint(doc, 40, 10));
        const StyleId shared = addStyle(doc, 0xff000000u);
        setStyle(doc, s1, shared);
        setStyle(doc, s2, shared);
        UndoJournal journal;
        Session session(doc, journal);
        session.setViewport(vocabViewport());
        session.select({s1, s2});
        REQUIRE(session.setStyleStroke(0xff00ff00u));
        CHECK(session.document().styles().size() == 1);  // no fork
        CHECK(session.document().styles().find(shared)->strokeColor == 0xff00ff00u);
        CHECK(session.presentation().styleForked == 0);
    }

    SUBCASE("a style shared beyond the selection forks and leaves the outsider") {
        Document doc;
        const EntityId s1 = addSegment(doc, addPoint(doc, 0, 0), addPoint(doc, 40, 0));
        const EntityId s2 = addSegment(doc, addPoint(doc, 0, 10), addPoint(doc, 40, 10));
        const EntityId s3 = addSegment(doc, addPoint(doc, 0, 20), addPoint(doc, 40, 20));
        const StyleId shared = addStyle(doc, 0xff000000u);
        setStyle(doc, s1, shared);
        setStyle(doc, s2, shared);
        setStyle(doc, s3, shared);
        UndoJournal journal;
        Session session(doc, journal);
        session.setViewport(vocabViewport());
        session.select({s1, s2});
        REQUIRE(session.setStyleStroke(0xff0000ffu));
        CHECK(session.document().styles().size() == 2);  // one forked
        CHECK(session.document().entities().find(s3)->style == shared);  // outsider kept
        const StyleId s1style = session.document().entities().find(s1)->style;
        CHECK(s1style != shared);
        CHECK(session.document().entities().find(s2)->style == s1style);  // both to the fork
        CHECK(session.presentation().styleForked == 1);
    }
}

TEST_CASE("a style edit acts only within the scope its applicability names") {
    SUBCASE("set-filled refuses an entity-only selection, leaving it untouched") {
        Document doc;
        const EntityId s1 = addSegment(doc, addPoint(doc, 0, 0), addPoint(doc, 40, 0));
        UndoJournal journal;
        Session session(doc, journal);
        session.setViewport(vocabViewport());
        session.select({s1});
        // filled is a region concept; with no region selected there is nothing in
        // scope, so the edit refuses rather than forking the entity's style.
        CHECK_FALSE(session.setStyleFilled(true));
        CHECK(session.document().styles().empty());
    }

    SUBCASE("a no-op edit forks nothing and mutates nothing") {
        Document doc;
        const EntityId s1 = addSegment(doc, addPoint(doc, 0, 0), addPoint(doc, 40, 0));
        UndoJournal journal;
        Session session(doc, journal);
        session.setViewport(vocabViewport());
        session.select({s1});
        REQUIRE(session.setStyleStroke(0xffabcdefu));
        REQUIRE(session.document().styles().size() == 1);
        CHECK(session.presentation().styleForked == 1);
        // Setting the same colour again changes nothing: no second style, no fork.
        CHECK(session.setStyleStroke(0xffabcdefu));  // applicable, so it succeeds
        CHECK(session.document().styles().size() == 1);
        CHECK(session.presentation().styleForked == 0);
    }

    SUBCASE("width resists an expression-driven slot rather than flattening it") {
        Document doc;
        const EntityId s1 = addSegment(doc, addPoint(doc, 0, 0), addPoint(doc, 40, 0));
        // A parameter and a style whose width references it.
        ParameterRecord w;
        w.name = "w";
        w.value = Slot(3.0);
        const ParameterId wid(doc.apply(AddRecord<ParameterRecord>{w}).allocated);
        StyleRecord style;
        style.strokeWidth = Slot::parameter(wid);
        const StyleId sid(doc.apply(AddRecord<StyleRecord>{style}).allocated);
        setStyle(doc, s1, sid);

        UndoJournal journal;
        Session session(doc, journal);
        session.setViewport(vocabViewport());
        session.select({s1});
        const std::string before = serialize(session.document());
        // A direct width edit is refused; the expression is not flattened.
        CHECK_FALSE(session.setStyleStrokeWidth(5.0));
        CHECK(serialize(session.document()) == before);
        // Opacity, still a constant, is editable.
        CHECK(session.setStyleOpacity(0.5));
    }

    SUBCASE("two distinct shared styles fork independently in one edit") {
        Document doc;
        const EntityId s1 = addSegment(doc, addPoint(doc, 0, 0), addPoint(doc, 40, 0));
        const EntityId s2 = addSegment(doc, addPoint(doc, 0, 10), addPoint(doc, 40, 10));
        const EntityId out1 = addSegment(doc, addPoint(doc, 0, 20), addPoint(doc, 40, 20));
        const EntityId out2 = addSegment(doc, addPoint(doc, 0, 30), addPoint(doc, 40, 30));
        const StyleId a = addStyle(doc, 0xff111111u);
        const StyleId b = addStyle(doc, 0xff222222u);
        setStyle(doc, s1, a);
        setStyle(doc, out1, a);  // a shared outside
        setStyle(doc, s2, b);
        setStyle(doc, out2, b);  // b shared outside
        UndoJournal journal;
        Session session(doc, journal);
        session.setViewport(vocabViewport());
        session.select({s1, s2});
        REQUIRE(session.setStyleStroke(0xff999999u));
        CHECK(session.presentation().styleForked == 2);  // one per group
        const StyleId f1 = session.document().entities().find(s1)->style;
        const StyleId f2 = session.document().entities().find(s2)->style;
        CHECK(f1 != f2);  // distinct, collision-free ids
        CHECK(f1 != a);
        CHECK(f2 != b);
        CHECK(session.document().entities().find(out1)->style == a);  // outsiders kept
        CHECK(session.document().entities().find(out2)->style == b);
    }
}

TEST_CASE("set-value refuses a value the document cannot hold, byte-identical") {
    // Two coincident points cannot also be fifty apart, so driving the distance
    // to fifty is refused.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 0.0, 0.0);
    addConstraint(doc, ConstraintKind::Coincident, {p, q});
    const ConstraintId dist =
        addConstraint(doc, ConstraintKind::PointPointDistance, {p, q}, Slot(0.0));
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(vocabViewport());

    const std::string before = serialize(session.document());
    CHECK_FALSE(session.setRelationValue(dist, 50.0));
    CHECK(serialize(session.document()) == before);  // byte-identical
    CHECK(session.presentation().downgrade.has_value());
}

TEST_CASE("a non-finite value is refused at the model boundary, not driven") {
    // The panel's numeric parse can hand a NaN to the model — an emptied field, a
    // "mixed" placeholder committed — and the setters must refuse it rather than
    // drive a NaN into the solve. The keyboard entry path already rejects it in
    // units; this is the second surface the model closes off itself.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 40.0, 0.0);
    const EntityId s1 = addSegment(doc, p, q);
    const ConstraintId dist =
        addConstraint(doc, ConstraintKind::PointPointDistance, {p, q}, Slot(40.0));
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(vocabViewport());

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    session.select({s1});
    const std::string before = serialize(session.document());
    CHECK_FALSE(session.setStyleStrokeWidth(nan));
    CHECK_FALSE(session.setStyleStrokeWidth(inf));
    CHECK_FALSE(session.setStyleStrokeWidth(-1.0));  // negative width is not a width
    CHECK_FALSE(session.setStyleOpacity(nan));
    CHECK_FALSE(session.setRelationValue(dist, nan));
    CHECK_FALSE(session.createParameterConstant("bad", nan));
    CHECK(serialize(session.document()) == before);
}

TEST_CASE("set-value drives a reachable value") {
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 40.0, 0.0);
    const ConstraintId dist =
        addConstraint(doc, ConstraintKind::PointPointDistance, {p, q}, Slot(40.0));
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(vocabViewport());
    REQUIRE(session.setRelationValue(dist, 60.0));
    CHECK(session.document().constraints().find(dist)->value == Slot(60.0));
    CHECK(session.document().constraints().find(dist)->driving);
}

TEST_CASE("flip-alternative toggles an arc's tangency and refuses a plain kind") {
    Document doc;
    const EntityId centre = addPoint(doc, 0.0, 0.0);
    const EntityId start = addPoint(doc, 20.0, 0.0);
    const EntityId end = addPoint(doc, 0.0, 20.0);
    const EntityId arc = addArc(doc, centre, start, end);
    const EntityId a = addPoint(doc, 20.0, -10.0);
    const EntityId b = addPoint(doc, 20.0, 10.0);
    const EntityId seg = addSegment(doc, a, b);
    const ConstraintId tan = addConstraint(doc, ConstraintKind::Tangent, {arc, seg});
    const ConstraintId coin = addConstraint(doc, ConstraintKind::Coincident, {start, a});

    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(vocabViewport());

    CHECK(session.document().constraints().find(tan)->alternative == 0);
    REQUIRE(session.flipAlternative(tan));
    CHECK(session.document().constraints().find(tan)->alternative == 1);
    REQUIRE(session.flipAlternative(tan));
    CHECK(session.document().constraints().find(tan)->alternative == 0);

    // Coincidence has no alternative form.
    CHECK_FALSE(session.flipAlternative(coin));
}

TEST_CASE("a parameter cycle is caught before it commits") {
    Document doc;
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(vocabViewport());

    REQUIRE(session.createParameterConstant("a", 2.0));
    REQUIRE(session.createParameterConstant("b", 3.0));
    const ParameterRecord *a = findParameterByName(session.document().parameters(), "a");
    const ParameterRecord *b = findParameterByName(session.document().parameters(), "b");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    // b depends on a: fine.
    REQUIRE(session.setParameterExpression(b->id, "a * 2"));
    CHECK(evaluateParameter(session.document().parameters(), b->id) == doctest::Approx(4.0));

    // a depending on b would close the loop: refused, inline and on commit.
    CHECK(session.wouldParameterCycle(a->id, "b + 1"));
    const std::string before = serialize(session.document());
    CHECK_FALSE(session.setParameterExpression(a->id, "b + 1"));
    CHECK(serialize(session.document()) == before);

    // A self-reference is the degenerate cycle.
    CHECK(session.wouldParameterCycle(a->id, "a"));
}

TEST_CASE("delete-parameter freezes what it drove to the value it holds") {
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 40.0, 0.0);
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(vocabViewport());
    REQUIRE(session.createParameterConstant("len", 40.0));
    const ParameterRecord *len = findParameterByName(session.document().parameters(), "len");
    REQUIRE(len != nullptr);
    // A distance driven by the parameter.
    ConstraintRecord dim;
    dim.kind = ConstraintKind::PointPointDistance;
    dim.operands[0] = p;
    dim.operands[1] = q;
    dim.value = Slot::parameter(len->id);
    const ConstraintId dimId(doc.apply(AddRecord<ConstraintRecord>{dim}).allocated);
    session.refresh();

    REQUIRE(session.deleteParameter(len->id));
    // The parameter is gone and the dimension froze to 40, a constant now.
    CHECK(session.document().parameters().find(len->id) == nullptr);
    const ConstraintRecord *frozen = session.document().constraints().find(dimId);
    REQUIRE(frozen != nullptr);
    CHECK(frozen->value.isConstant());
    CHECK(frozen->value.constant() == doctest::Approx(40.0));
}
