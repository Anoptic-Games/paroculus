// Imposition: declaring intent about the present.
//
// The property under test throughout is that declaring what is already true
// changes nothing — not the geometry, and for a preview not the document
// either. Everything else here is about what happens when it cannot be true:
// the downgrade offer, the conflicting set, and the fact that neither of them
// is allowed to be silent.
#include <doctest/doctest.h>

#include <memory>

#include "core/measure.h"
#include "core/persist.h"
#include "interact/impose.h"
#include "interact/session.h"
#include "interact/surface.h"
#include "support/build.h"

using namespace paroculus;
using namespace paroculus::test;

namespace {

Viewport imposeViewport() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    Viewport v;
    v.view = ViewTransform(m);
    v.width = 800.0;
    v.height = 600.0;
    return v;
}

// Two segments, deliberately neither parallel nor equal, in one document.
struct TwoSegments {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session;
    EntityId a, b;

    TwoSegments() {
        const EntityId p0 = addPoint(doc, 0.0, 0.0);
        const EntityId p1 = addPoint(doc, 40.0, 0.0);
        const EntityId p2 = addPoint(doc, 0.0, 30.0);
        const EntityId p3 = addPoint(doc, 25.0, 44.0);
        a = addSegment(doc, p0, p1);
        b = addSegment(doc, p2, p3);
        session = std::make_unique<Session>(doc, journal);
        session->setViewport(imposeViewport());
        session->select({a, b});
    }
};

// Every point in the document, so a test can assert that nothing moved.
std::vector<Point> allPoints(const Session &session) {
    std::vector<Point> out;
    const Pose pose = session.pose();
    for(const EntityRecord &e : session.document().entities().records()) {
        if(const std::optional<Point> p = pose.point(e.id)) out.push_back(*p);
    }
    return out;
}

double furthestMove(const std::vector<Point> &before, const std::vector<Point> &after) {
    double worst = 0.0;
    const size_t n = std::min(before.size(), after.size());
    for(size_t i = 0; i < n; i++) {
        const double dx = after[i].x - before[i].x;
        const double dy = after[i].y - before[i].y;
        worst = std::max(worst, std::sqrt(dx * dx + dy * dy));
    }
    return worst;
}

}  // namespace

TEST_CASE("a valued imposition captures what is measured and moves nothing") {
    // The whole promise: impose a distance and it becomes the distance already
    // there, so the solver has nothing to do and the drawing does not shift
    // under the declaration.
    for(ConstraintKind kind : {ConstraintKind::LengthRatio, ConstraintKind::LengthDifference,
                               ConstraintKind::Angle}) {
        CAPTURE(std::string(constraintInfo(kind).name));
        TwoSegments f;
        const std::vector<Point> before = allPoints(*f.session);

        REQUIRE(invokeAction(*f.session, std::string("constrain.") +
                                             std::string(constraintInfo(kind).name)));

        const std::vector<Point> after = allPoints(*f.session);
        CHECK(furthestMove(before, after) < 1e-6);
        CHECK(f.session->presentation().impositionMotion < 1e-6);

        // And what it recorded is what was there, not a default.
        const ConstraintId id = f.session->presentation().imposed;
        REQUIRE(id.valid());
        const ConstraintRecord *r = f.doc.constraints().find(id);
        REQUIRE(r != nullptr);
        CHECK(r->driving);
        const std::optional<double> gap = residual(f.session->pose(), *r);
        REQUIRE(gap);
        CHECK(*gap < 1e-6);
    }
}

TEST_CASE("imposing parallelism on near-parallel segments shows its motion") {
    // The exception that proves the rule. Parallel carries no value, so there
    // is nothing to capture; the small angle snaps shut and the residual motion
    // is reported rather than made quietly.
    Document doc;
    UndoJournal journal;
    const EntityId p0 = addPoint(doc, 0.0, 0.0);
    const EntityId p1 = addPoint(doc, 40.0, 0.0);
    const EntityId p2 = addPoint(doc, 0.0, 20.0);
    const EntityId p3 = addPoint(doc, 40.0, 22.0);  // ~3 degrees off
    const EntityId a = addSegment(doc, p0, p1);
    const EntityId b = addSegment(doc, p2, p3);

    Session session(doc, journal);
    session.setViewport(imposeViewport());
    session.select({a, b});

    const std::vector<Point> before = allPoints(session);
    REQUIRE(invokeAction(session, "constrain.parallel"));

    CHECK(session.presentation().impositionMotion > 1e-3);
    CHECK(furthestMove(before, allPoints(session)) > 1e-3);

    // And it is actually parallel afterwards, which is the point of moving.
    const ConstraintRecord *r = doc.constraints().find(session.presentation().imposed);
    REQUIRE(r != nullptr);
    const std::optional<double> gap = residual(session.pose(), *r);
    REQUIRE(gap);
    CHECK(*gap < 1e-6);
}

TEST_CASE("a hover storm leaves the document byte-identical") {
    // Preview forks a component's parameters and throws the copy away. The
    // document is immutable throughout, by construction rather than by anyone
    // remembering not to save.
    TwoSegments f;
    const std::string before = serialize(f.doc);

    for(int round = 0; round < 20; round++) {
        for(const RelationOffer &offer : f.session->relationOffers()) {
            for(size_t i = 0; i < offer.assignments.size(); i++) {
                const auto preview = f.session->previewImposition(offer.kind, i);
                CHECK(preview.has_value());
            }
        }
    }
    CHECK(serialize(f.doc) == before);
}

TEST_CASE("length ratio asks which way round") {
    // The role ambiguity PRINCIPLES sends to the surface rather than resolving
    // in prose. Two segments, one relation, two readings — and the two readings
    // record different values, which is what makes the question a real one.
    TwoSegments f;
    const std::vector<RelationOffer> offers = f.session->relationOffers();

    const RelationOffer *ratio = nullptr;
    const RelationOffer *equal = nullptr;
    for(const RelationOffer &o : offers) {
        if(o.kind == ConstraintKind::LengthRatio) ratio = &o;
        if(o.kind == ConstraintKind::EqualLength) equal = &o;
    }
    REQUIRE(ratio != nullptr);
    REQUIRE(equal != nullptr);

    CHECK(ratio->ambiguous());
    // Equal length is the same relation either way round, so offering it twice
    // would be offering the same thing under two names.
    CHECK_FALSE(equal->ambiguous());
    REQUIRE(ratio->assignments.size() == 2);
    CHECK(ratio->assignments[0].operands[0] == ratio->assignments[1].operands[1]);

    const auto first = f.session->previewImposition(ConstraintKind::LengthRatio, 0);
    const auto second = f.session->previewImposition(ConstraintKind::LengthRatio, 1);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->check.committable());
    CHECK(second->check.committable());

    // Both readings are movement-free; they simply record reciprocal values.
    CHECK(first->motion < 1e-6);
    CHECK(second->motion < 1e-6);

    const auto ra = candidateFor(f.session->pose(), ConstraintKind::LengthRatio,
                                 ratio->assignments[0], Strength::Impose);
    const auto rb = candidateFor(f.session->pose(), ConstraintKind::LengthRatio,
                                 ratio->assignments[1], Strength::Impose);
    REQUIRE(ra);
    REQUIRE(rb);
    CHECK(ra->value.constant() != doctest::Approx(rb->value.constant()));
    CHECK(ra->value.constant() * rb->value.constant() == doctest::Approx(1.0));
}

TEST_CASE("equal angle asks which segments are paired") {
    // Four segments admit three pairings and they are three different
    // declarations: angle(A,B) = angle(C,D) is not angle(A,C) = angle(B,D). The
    // kind read its operands in whatever order the IDs happened to sort into and
    // imposed a pairing nobody chose — the same question length-ratio gets a
    // surface for, on a kind where the wrong reading is much harder to see.
    Document doc;
    UndoJournal journal;
    std::vector<EntityId> segments;
    // Four deliberately unequal directions, so no two pairings agree by
    // accident and the values a capture records tell them apart.
    const double angles[4] = {0.0, 25.0, 70.0, 130.0};
    for(double degrees : angles) {
        const double radians = degrees * M_PI / 180.0;
        const EntityId from = addPoint(doc, 0.0, 0.0);
        const EntityId to = addPoint(doc, 50.0 * std::cos(radians), 50.0 * std::sin(radians));
        segments.push_back(addSegment(doc, from, to));
    }

    Session session(doc, journal);
    session.setViewport(imposeViewport());
    session.select(segments);

    const std::vector<RoleAssignment> assignments =
        assignmentsFor(doc, ConstraintKind::EqualAngle, segments);
    // Three pairings, each in its two forms — the angle as drawn and its
    // supplement — and not the twenty-four the permutations would suggest.
    REQUIRE(assignments.size() == 6);

    // Every reading names all four segments once, and no two readings pair the
    // same way.
    std::vector<std::pair<EntityId, EntityId>> pairings;
    for(const RoleAssignment &a : assignments) {
        REQUIRE(a.count == 4);
        std::vector<EntityId> named(a.operands.begin(), a.operands.begin() + 4);
        std::sort(named.begin(), named.end());
        std::vector<EntityId> sorted = segments;
        std::sort(sorted.begin(), sorted.end());
        CHECK(named == sorted);
        if(a.alternative == 0) pairings.emplace_back(a.operands[0], a.operands[1]);
    }
    REQUIRE(pairings.size() == 3);
    for(size_t i = 0; i < pairings.size(); i++) {
        for(size_t j = i + 1; j < pairings.size(); j++) CHECK(pairings[i] != pairings[j]);
    }

    // And the readings are genuinely different declarations: each captures the
    // angle its own pairing is at, so no two record the same value.
    std::vector<double> captured;
    for(const RoleAssignment &a : assignments) {
        if(a.alternative != 0) continue;
        const auto r = candidateFor(session.pose(), ConstraintKind::EqualAngle, a,
                                    Strength::Impose);
        REQUIRE(r);
        captured.push_back(*measure(session.pose(), ConstraintKind::Angle,
                                    std::span<const EntityId>(r->operands.data(), 2)));
    }
    REQUIRE(captured.size() == 3);
    CHECK(captured[0] != doctest::Approx(captured[1]));
    CHECK(captured[1] != doctest::Approx(captured[2]));
    CHECK(captured[0] != doctest::Approx(captured[2]));

    // The surface says the question exists, so it can ask it.
    const std::vector<RelationOffer> offers = session.relationOffers();
    const RelationOffer *equalAngle = nullptr;
    for(const RelationOffer &o : offers) {
        if(o.kind == ConstraintKind::EqualAngle) equalAngle = &o;
    }
    REQUIRE(equalAngle != nullptr);
    CHECK(equalAngle->ambiguous());
}

TEST_CASE("an out-of-range reading is refused rather than clamped") {
    // A script written against a different selection has asked for something
    // that is not there. Imposing the other reading instead would declare the
    // wrong relation and look like it worked.
    TwoSegments f;
    CHECK_FALSE(f.session->impose(ConstraintKind::LengthRatio, Strength::Impose, 7));
    CHECK(f.doc.constraints().records().empty());
}

TEST_CASE("a driving imposition that cannot hold offers the downgrade") {
    // Over-constraint's first moment. The candidate is solved speculatively and
    // on failure the surface offers the reference measurement — it does not
    // commit one nobody asked for, which is the one thing this stage changes
    // about a mechanism that already worked.
    Document doc;
    UndoJournal journal;
    const EntityId p0 = addPoint(doc, 0.0, 0.0);
    const EntityId p1 = addPoint(doc, 40.0, 0.0);
    addConstraint(doc, ConstraintKind::Pin, {p0});
    addConstraint(doc, ConstraintKind::Pin, {p1});
    const EntityId seg = addSegment(doc, p0, p1);

    Session session(doc, journal);
    session.setViewport(imposeViewport());
    session.select({p0, p1});

    const size_t relationsBefore = doc.constraints().records().size();
    // Both ends are pinned, so any distance but the one they are already at is
    // a contradiction.
    CHECK_FALSE(session.impose(ConstraintKind::PointPointDistance, Strength::Impose, 0, 90.0));
    CHECK(doc.constraints().records().size() == relationsBefore);
    CHECK(session.presentation().impositionVerdict == CandidateVerdict::Inconsistent);

    // The offer names the reading it was refused for, because that is what
    // makes it an offer rather than a flag: the surface has to be able to
    // invoke it, and invoking a reading means naming it.
    REQUIRE(session.presentation().downgrade);
    CHECK(session.presentation().downgrade->kind == ConstraintKind::PointPointDistance);

    // The conflicting set is attributed, not merely reported empty — and it
    // names the pins, which are what the distance disagrees with.
    CHECK(session.presentation().conflictAttributed);
    CHECK_FALSE(session.presentation().conflicting.empty());
    for(ConstraintId id : session.presentation().conflicting) {
        const ConstraintRecord *r = doc.constraints().find(id);
        REQUIRE(r != nullptr);
        CHECK(r->kind == ConstraintKind::Pin);
    }

    // And it is walkable: the set becomes an ordinary selection the user steps
    // through, which is also why it replaces the geometry selection — the
    // question has moved from "these points" to "these relations".
    REQUIRE(invokeAction(session, "relation.walk-conflicts"));
    CHECK(session.selection().constraints() == session.presentation().conflicting);
    CHECK(session.selection().items().empty());
    CHECK(seg.valid());

    // The offer belongs to the selection it was refused for, so selecting
    // something else takes it back rather than leaving it to invoke a
    // measurement over whatever is selected now.
    session.select({p0, p1});
    CHECK_FALSE(session.presentation().downgrade);

    // Refuse it again, and this time take the offer the way a user would: off
    // the strip, where the refusal happened. Knowing that the palette spells the
    // measurement with "(reference)" and going to find it was the only route
    // before, which is the choice existing while the offer did not.
    CHECK_FALSE(session.impose(ConstraintKind::PointPointDistance, Strength::Impose, 0, 90.0));
    REQUIRE(session.presentation().downgrade);

    const std::vector<SurfaceEntry> strip = stripEntries(session);
    const SurfaceEntry *offer = nullptr;
    for(const SurfaceEntry &entry : strip) {
        if(entry.action != nullptr && entry.action->name == "constrain.distance.reference") {
            offer = &entry;
        }
    }
    REQUIRE(offer != nullptr);
    // At the top, because it is the answer to the thing that just happened.
    CHECK(offer == &strip.front());

    REQUIRE(invokeAction(session, offer->action->name, offer->arguments));
    const ConstraintRecord *measurement =
        doc.constraints().find(session.presentation().imposed);
    REQUIRE(measurement != nullptr);
    CHECK_FALSE(measurement->driving);
    // And taking it clears the offer: the question has been answered.
    CHECK_FALSE(session.presentation().downgrade);
}

TEST_CASE("driving and reference are one object with a toggle") {
    TwoSegments f;
    REQUIRE(invokeAction(*f.session, "constrain.equal-length"));
    const ConstraintId id = f.session->presentation().imposed;
    REQUIRE(id.valid());
    CHECK(f.doc.constraints().find(id)->driving);

    f.session->selectConstraint(id);
    REQUIRE(invokeAction(*f.session, "relation.toggle-driving"));
    CHECK_FALSE(f.doc.constraints().find(id)->driving);

    REQUIRE(invokeAction(*f.session, "relation.toggle-driving"));
    CHECK(f.doc.constraints().find(id)->driving);

    // Round trip through the file, because the toggle is a document property
    // and not a presentation one.
    const std::string text = serialize(f.doc);
    Document reloaded;
    REQUIRE(deserialize(text, reloaded).ok);
    CHECK(reloaded.constraints().find(id)->driving);
}

TEST_CASE("measure-once moves the geometry and records nothing") {
    // Align these now, remember nothing. The relation is applied, the geometry
    // that comes out is kept, and the relation is thrown away.
    Document doc;
    UndoJournal journal;
    const EntityId p0 = addPoint(doc, 0.0, 0.0);
    const EntityId p1 = addPoint(doc, 40.0, 0.0);
    const EntityId p2 = addPoint(doc, 0.0, 20.0);
    const EntityId p3 = addPoint(doc, 40.0, 26.0);
    const EntityId a = addSegment(doc, p0, p1);
    const EntityId b = addSegment(doc, p2, p3);

    Session session(doc, journal);
    session.setViewport(imposeViewport());
    session.select({a, b});

    REQUIRE(invokeAction(session, "constrain.parallel.measure"));
    CHECK(doc.constraints().records().empty());

    // It happened, though: the segments are parallel now.
    const auto gap = parallelGapDegrees(session.pose(), a, b);
    REQUIRE(gap);
    CHECK(*gap < 1e-6);

    // And nothing holds them there — dragging is free, because no relation was
    // recorded. Undo puts the geometry back, since moving geometry is an edit
    // whatever strength asked for it.
    REQUIRE(session.canUndo());
}

TEST_CASE("the strip ranks by what this document reaches for") {
    // Contextual, document-local, deterministic, inspectable. No global learned
    // magic: the whole of the context is a count per kind.
    TwoSegments f;
    const std::vector<SurfaceEntry> before = stripEntries(*f.session);
    REQUIRE(!before.empty());

    // Impose perpendicular a few times on other geometry, then look again.
    for(int i = 0; i < 5; i++) f.doc.noteUsage(ConstraintKind::Perpendicular);

    const std::vector<SurfaceEntry> after = stripEntries(*f.session);
    REQUIRE(!after.empty());
    CHECK(after.front().title == std::string("Perpendicular"));

    // Deterministic: asking twice gives the same list.
    const std::vector<SurfaceEntry> again = stripEntries(*f.session);
    REQUIRE(again.size() == after.size());
    for(size_t i = 0; i < again.size(); i++) CHECK(again[i].title == after[i].title);
}

TEST_CASE("usage history survives a round trip and is droppable") {
    TwoSegments f;
    REQUIRE(invokeAction(*f.session, "constrain.parallel"));
    CHECK(f.doc.usage().count(ConstraintKind::Parallel) == 1);

    const std::string text = serialize(f.doc);
    Document reloaded;
    REQUIRE(deserialize(text, reloaded).ok);
    CHECK(reloaded.usage().count(ConstraintKind::Parallel) == 1);
    CHECK(serialize(reloaded) == text);

    // Droppable: strip the section and the drawing is unchanged, only the
    // ranking loses its history.
    std::string stripped;
    for(size_t at = 0; at < text.size();) {
        const size_t end = text.find('\n', at);
        const std::string_view line(text.data() + at, (end == std::string::npos ? text.size() : end) - at);
        if(line.substr(0, 6) != "usage ") {
            stripped += line;
            stripped += '\n';
        }
        if(end == std::string::npos) break;
        at = end + 1;
    }
    Document dropped;
    REQUIRE(deserialize(stripped, dropped).ok);
    CHECK(dropped.usage().empty());
    CHECK(sameRecords(dropped, reloaded));
}

TEST_CASE("the palette searches by subsequence and dims rather than hides") {
    TwoSegments f;
    const std::vector<SurfaceEntry> all = paletteEntries(*f.session, "");
    CHECK(all.size() == actions().size());

    const std::vector<SurfaceEntry> found = paletteEntries(*f.session, "prl");
    bool sawParallel = false;
    for(const SurfaceEntry &e : found) {
        if(e.title == std::string("Parallel")) sawParallel = true;
    }
    CHECK(sawParallel);

    // Inapplicable entries are present and marked, not filtered out: a command
    // that vanishes is a command the user cannot learn.
    bool sawInapplicable = false;
    for(const SurfaceEntry &e : all) {
        if(!e.applicable) sawInapplicable = true;
    }
    CHECK(sawInapplicable);

    // And the order is the table's, whatever the strip is ranking today.
    for(int i = 0; i < 5; i++) f.doc.noteUsage(ConstraintKind::Perpendicular);
    const std::vector<SurfaceEntry> again = paletteEntries(*f.session, "");
    REQUIRE(again.size() == all.size());
    for(size_t i = 0; i < again.size(); i++) CHECK(again[i].title == all[i].title);
}
