// The applicability conformance sweep, at full strength.
//
// Every constraint kind against every signature the entity vocabulary can
// generate, asserting that the predicate the action surface consults agrees
// with what actually happens when the action is attempted. This is the sweep
// that keeps the taxonomy's projections from drifting: applicability, model
// validation and the action surface all read one operand table, and the way
// that stays true is that a disagreement fails a test rather than shipping as
// a menu offering something the model refuses.
//
// It is deliberately exhaustive rather than sampled. The failure mode being
// guarded against is a single row of the table drifting, and a sample is
// exactly the instrument that misses one row.
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/persist.h"
#include "interact/impose.h"
#include "interact/registry.h"
#include "interact/session.h"
#include "interact/surface.h"
#include "support/build.h"

using namespace paroculus;
using namespace paroculus::test;

namespace {

Viewport sweepViewport() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    Viewport v;
    v.view = ViewTransform(m);
    v.width = 800.0;
    v.height = 600.0;
    return v;
}

// Every multiset of entity kinds up to the widest operand list the catalogue
// has. Generated rather than listed, for the same reason the actions are: a
// fifth entity kind should extend the sweep by existing, not by being
// remembered.
std::vector<std::vector<EntityKind>> generatedSignatures() {
    std::vector<std::vector<EntityKind>> out;
    std::vector<EntityKind> current;

    // Non-decreasing over the enum's order, which is what makes each multiset
    // appear exactly once.
    auto grow = [&](auto &&self, size_t from) -> void {
        if(!current.empty()) out.push_back(current);
        if(current.size() == MAX_OPERANDS) return;
        for(size_t k = from; k < ENTITY_KINDS.size(); k++) {
            current.push_back(ENTITY_KINDS[k].kind);
            self(self, k);
            current.pop_back();
        }
    };
    grow(grow, 0);
    return out;
}

// A document holding one entity of each requested kind, well clear of one
// another and of anything degenerate.
//
// Non-degenerate on purpose: a ratio against a zero-length segment has no
// measurement, and a fixture that produced one would be testing the geometry
// rather than the predicate. Placement is a function of the index alone, so the
// sweep is deterministic.
struct Sample {
    Document doc;
    std::vector<EntityId> selection;

    explicit Sample(const std::vector<EntityKind> &kinds) {
        for(size_t i = 0; i < kinds.size(); i++) {
            const double x = 60.0 * static_cast<double>(i);
            const double y = 25.0 * static_cast<double>(i);
            switch(kinds[i]) {
                case EntityKind::Point:
                    selection.push_back(addPoint(doc, x, y + 12.0));
                    break;
                case EntityKind::Segment: {
                    // Each one a different length and a different angle, so no
                    // pair is accidentally parallel or equal.
                    const EntityId a = addPoint(doc, x, y);
                    const EntityId b = addPoint(doc, x + 30.0 + 7.0 * i, y + 11.0 * (i + 1));
                    selection.push_back(addSegment(doc, a, b));
                    break;
                }
                case EntityKind::Circle:
                    selection.push_back(
                        addCircle(doc, addPoint(doc, x, y - 40.0), 9.0 + 3.0 * i));
                    break;
                case EntityKind::Arc: {
                    const EntityId centre = addPoint(doc, x, y + 70.0);
                    const EntityId start = addPoint(doc, x + 14.0, y + 70.0);
                    const EntityId end = addPoint(doc, x, y + 84.0);
                    selection.push_back(addArc(doc, centre, start, end));
                    break;
                }
            }
        }
    }
};

std::string describe(const std::vector<EntityKind> &kinds) {
    std::string out = "{";
    for(size_t i = 0; i < kinds.size(); i++) {
        if(i) out += ", ";
        out += entityInfo(kinds[i]).name;
    }
    return out + "}";
}

}  // namespace

TEST_CASE("the applicability predicate agrees with what attempting it does") {
    const std::vector<std::vector<EntityKind>> signatures = generatedSignatures();
    REQUIRE(signatures.size() > 60);

    // Which kinds were reachable at all. A stronger vacuity guard than a count:
    // it catches a row whose operand list no entity in the vocabulary can
    // satisfy, which is a kind the surface could never offer and nobody would
    // notice was missing.
    std::vector<bool> reachable(CONSTRAINT_KINDS.size(), false);

    for(const std::vector<EntityKind> &kinds : signatures) {
        for(const ConstraintKindInfo &info : CONSTRAINT_KINDS) {
            const std::string name(info.name);
            const std::string signature = describe(kinds);
            CAPTURE(name);
            CAPTURE(signature);

            // The predicate, asked the way a surface asks it.
            Sample probe(kinds);
            const bool predicted = !assignmentsFor(probe.doc, info.kind, probe.selection).empty();

            // And the attempt, on its own document so one case cannot leave a
            // constraint behind that changes the next one's answer.
            Sample subject(kinds);
            UndoJournal journal;
            Session session(subject.doc, journal);
            session.setViewport(sweepViewport());
            session.select(subject.selection);

            const bool applied = session.impose(info.kind, Strength::Impose);
            CHECK(applied == predicted);
            if(!predicted) {
                CHECK(subject.doc.constraints().records().empty());
                continue;
            }
            reachable[static_cast<size_t>(info.kind)] = true;

            // What landed is a well-formed record of the kind asked for, and
            // the model would have accepted it on its own — which is the other
            // half of "one source of truth": the surface did not talk the model
            // into something.
            REQUIRE(subject.doc.constraints().records().size() == 1);
            const ConstraintRecord &r = subject.doc.constraints().records().front();
            CHECK(r.kind == info.kind);
            CHECK(subject.doc.validate(r) == CommandError::None);
            CHECK(boundOperandCount(r) >= info.operandCount);
        }
    }
    // Every kind in the catalogue is reachable from some signature the entity
    // vocabulary can produce. A kind that is not is one the surface could never
    // offer, which no other assertion here would notice.
    for(const ConstraintKindInfo &info : CONSTRAINT_KINDS) {
        CAPTURE(std::string(info.name));
        CHECK(reachable[static_cast<size_t>(info.kind)]);
    }
}

TEST_CASE("what the registry says applies is what the surfaces offer") {
    // An action inapplicable in the model must be offerable by no surface, and
    // an applicable one must be reachable from every surface. Both halves,
    // because only checking one direction cannot fail on the other.
    for(const std::vector<EntityKind> &kinds : generatedSignatures()) {
        Sample sample(kinds);
        UndoJournal journal;
        Session session(sample.doc, journal);
        session.setViewport(sweepViewport());
        session.select(sample.selection);
        CAPTURE(describe(kinds));

        const ActionContext context = contextOf(session);
        const std::vector<SurfaceEntry> strip = stripEntries(session);
        const std::vector<SurfaceEntry> palette = paletteEntries(session, "");

        for(const Action &action : actions()) {
            if(!action.generated || action.strength != Strength::Impose) continue;
            const bool applies = action.applicable(context, action);

            // The palette lists everything and marks what applies, so its
            // answer must be the registry's.
            bool inPalette = false;
            for(const SurfaceEntry &e : palette) {
                if(e.action != &action) continue;
                inPalette = true;
                CHECK(e.applicable == applies);
            }
            CHECK(inPalette);

            // The strip only carries what applies. It truncates to a budget, so
            // presence is asserted only in the direction that cannot be
            // explained by truncation.
            bool inStrip = false;
            for(const SurfaceEntry &e : strip) {
                if(e.action == &action) inStrip = true;
            }
            if(inStrip) CHECK(applies);
        }
    }
}

TEST_CASE("every action is invocable headlessly, applicable or not") {
    // The registry is the automation surface as well as the UI's. What matters
    // is that every action answers — refusing when it does not apply — rather
    // than crashing or quietly doing something else.
    Sample sample({EntityKind::Segment, EntityKind::Segment});
    UndoJournal journal;
    Session session(sample.doc, journal);
    session.setViewport(sweepViewport());
    session.select(sample.selection);

    for(const Action &action : actions()) {
        CAPTURE(std::string(action.name));
        ActionArguments arguments;
        for(const ActionParameter &p : action.parameters) arguments.set(p.name, 0.0);

        const bool applies = action.applicable(contextOf(session), action);
        const bool ran = invokeAction(session, action.name, arguments);
        CHECK(ran == applies);
    }
}

TEST_CASE("a kind's generated rows cover exactly its three strengths") {
    // The catalogue is generated from the taxonomy, so this is the assertion
    // that the generation is total: every kind, every strength, once each, and
    // no hand-written row shadowing one.
    for(const ConstraintKindInfo &info : CONSTRAINT_KINDS) {
        CAPTURE(std::string(info.name));
        for(Strength strength :
            {Strength::Impose, Strength::Reference, Strength::Measure}) {
            const Action *action = impositionAction(info.kind, strength);
            REQUIRE(action != nullptr);
            CHECK(action->constraintKind == info.kind);
            CHECK(action->strength == strength);
            // And it is findable by name, which is what a script spells.
            CHECK(findAction(action->name) == action);
        }
    }

    size_t generated = 0;
    for(const Action &a : actions()) {
        if(a.generated) generated++;
    }
    CHECK(generated == CONSTRAINT_KINDS.size() * 3);
}

TEST_CASE("an unknown action name is refused rather than ignored") {
    Sample sample({EntityKind::Segment});
    UndoJournal journal;
    Session session(sample.doc, journal);
    CHECK(findAction("constrain.nonesuch") == nullptr);
    CHECK_FALSE(invokeAction(session, "constrain.nonesuch"));
}
