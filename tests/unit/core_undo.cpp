#include <doctest/doctest.h>

#include "core/undo.h"
#include "support/build.h"

using namespace paroculus;
using paroculus::test::Rng;
using paroculus::test::addPoint;
using paroculus::test::addSegment;

namespace {

// A random but always-legal edit script. Every command is built from records
// that exist, so refusals are the exception the rollback path is tested with
// rather than the rule the property is drowned in.
std::vector<Command> randomStep(const Document &doc, Rng &rng) {
    std::vector<Command> step;
    const auto &entities = doc.entities().records();
    const auto &constraints = doc.constraints().records();

    const uint32_t choice = rng.below(10);

    if(choice < 4 || entities.empty()) {
        EntityRecord p;
        p.kind = EntityKind::Point;
        p.seeds = {rng.real(-100.0, 100.0), rng.real(-100.0, 100.0)};
        step.push_back(AddRecord<EntityRecord>{p});
        return step;
    }

    // Collect what exists, so the script stays legal.
    std::vector<EntityId> points, segments;
    for(const EntityRecord &e : entities) {
        if(e.kind == EntityKind::Point) points.push_back(e.id);
        if(e.kind == EntityKind::Segment) segments.push_back(e.id);
    }

    if(choice < 6 && points.size() >= 2) {
        EntityRecord s;
        s.kind = EntityKind::Segment;
        s.points = {points[rng.below(points.size())], points[rng.below(points.size())],
                    EntityId()};
        if(s.points[0] == s.points[1]) return step;
        step.push_back(AddRecord<EntityRecord>{s});
        return step;
    }

    if(choice < 7 && !segments.empty()) {
        ConstraintRecord c;
        c.kind = ConstraintKind::Horizontal;
        c.operands = {segments[rng.below(segments.size())], EntityId(), EntityId(), EntityId()};
        step.push_back(AddRecord<ConstraintRecord>{c});
        return step;
    }

    if(choice < 8 && !points.empty()) {
        // Move a seed: the shape a drag commit takes.
        EntityRecord moved = *doc.entities().find(points[rng.below(points.size())]);
        moved.seeds = {rng.real(-100.0, 100.0), rng.real(-100.0, 100.0)};
        step.push_back(SetRecord<EntityRecord>{moved});
        return step;
    }

    if(choice < 9 && !constraints.empty()) {
        step.push_back(RemoveRecord<ConstraintRecord>{constraints[rng.below(constraints.size())].id});
        return step;
    }

    // Delete an entity plus everything that would otherwise dangle, as one
    // gesture — which is the only way a deletion is ever expressed.
    if(!points.empty()) {
        step = deletionStep(doc, points[rng.below(points.size())]);
    }
    return step;
}

}  // namespace

TEST_CASE("a step undoes and redoes to the states either side of it") {
    Document doc;
    UndoJournal journal;
    const Document empty = doc;

    EntityRecord p;
    p.kind = EntityKind::Point;
    p.seeds = {1.0, 2.0};
    REQUIRE(journal.applyStep(doc, "add point", AddRecord<EntityRecord>{p}) ==
            CommandError::None);
    const Document afterAdd = doc;
    CHECK(doc.entities().size() == 1);

    REQUIRE(journal.undo(doc));
    CHECK(sameRecords(doc, empty));
    REQUIRE(journal.redo(doc));
    CHECK(doc == afterAdd);
}

TEST_CASE("redo reinstates the same ids, never fresh ones") {
    // An add left with a null id would allocate a second, different id the
    // second time through, and redo-all would not equal the state it claims to
    // reproduce.
    Document doc;
    UndoJournal journal;

    EntityRecord p;
    p.kind = EntityKind::Point;
    REQUIRE(journal.applyStep(doc, "add", AddRecord<EntityRecord>{p}) == CommandError::None);
    const EntityId original = doc.entities().records().front().id;

    REQUIRE(journal.undo(doc));
    REQUIRE(journal.redo(doc));
    CHECK(doc.entities().records().front().id == original);
}

TEST_CASE("a composite gesture is one undo step") {
    // Place-with-inferences bundles: the geometry and the constraints it
    // provoked undo together, because that is what the user did.
    Document doc;
    UndoJournal journal;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 10.0, 0.0);
    const Document beforeGesture = doc;

    EntityRecord seg;
    seg.kind = EntityKind::Segment;
    seg.points = {a, b, EntityId()};

    // The constraint names an entity that does not exist until the first
    // command in this same step has run, which is why inverses are computed
    // per command rather than up front.
    std::vector<Command> gesture;
    gesture.push_back(AddRecord<EntityRecord>{seg});
    const uint32_t predicted = doc.entities().allocator().next();
    ConstraintRecord horizontal;
    horizontal.kind = ConstraintKind::Horizontal;
    horizontal.operands = {EntityId(predicted), EntityId(), EntityId(), EntityId()};
    gesture.push_back(AddRecord<ConstraintRecord>{horizontal});

    REQUIRE(journal.applyStep(doc, "draw segment", gesture) == CommandError::None);
    CHECK(doc.entities().size() == 3);
    CHECK(doc.constraints().size() == 1);

    REQUIRE(journal.undo(doc));
    CHECK(sameRecords(doc, beforeGesture));
    CHECK(journal.depth() == 0);
}

TEST_CASE("a step that fails partway rolls back entirely") {
    // A half-applied gesture is not a state the user can reason about or undo
    // out of.
    Document doc;
    UndoJournal journal;
    addPoint(doc, 0.0, 0.0);
    const Document before = doc;

    EntityRecord good;
    good.kind = EntityKind::Point;
    ConstraintRecord bad;
    bad.kind = ConstraintKind::Parallel;
    bad.operands = {EntityId(900), EntityId(901), EntityId(), EntityId()};

    std::vector<Command> step;
    step.push_back(AddRecord<EntityRecord>{good});
    step.push_back(AddRecord<ConstraintRecord>{bad});

    CHECK(journal.applyStep(doc, "doomed", step) != CommandError::None);
    CHECK(sameRecords(doc, before));
    CHECK_FALSE(journal.canUndo());
}

TEST_CASE("a new step truncates the redo tail") {
    Document doc;
    UndoJournal journal;
    EntityRecord p;
    p.kind = EntityKind::Point;

    REQUIRE(journal.applyStep(doc, "one", AddRecord<EntityRecord>{p}) == CommandError::None);
    REQUIRE(journal.applyStep(doc, "two", AddRecord<EntityRecord>{p}) == CommandError::None);
    REQUIRE(journal.undo(doc));
    CHECK(journal.canRedo());

    REQUIRE(journal.applyStep(doc, "three", AddRecord<EntityRecord>{p}) == CommandError::None);
    CHECK_FALSE(journal.canRedo());
    CHECK(journal.records().size() == 2);
}

TEST_CASE("property: apply then undo restores every record, at every prefix") {
    // The property the plan names, run over random legal scripts and checked at
    // every prefix rather than only at the end, because a bug that cancels
    // itself over a whole script still corrupts the state in the middle.
    for(uint64_t seed = 1; seed <= 40; seed++) {
        Rng rng(seed * 7919u);
        Document doc;
        UndoJournal journal;

        std::vector<Document> history;
        history.push_back(doc);

        for(int i = 0; i < 30; i++) {
            std::vector<Command> step = randomStep(doc, rng);
            if(step.empty()) continue;
            if(journal.applyStep(doc, "step", std::move(step)) != CommandError::None) continue;
            history.push_back(doc);
        }

        // Walk all the way back, checking each prefix on the way.
        for(size_t i = history.size(); i-- > 1;) {
            REQUIRE_MESSAGE(journal.undo(doc), "seed ", seed);
            REQUIRE_MESSAGE(sameRecords(doc, history[i - 1]), "seed ", seed, " prefix ", i - 1);
        }
        CHECK_FALSE(journal.canUndo());

        // And all the way forward again.
        for(size_t i = 1; i < history.size(); i++) {
            REQUIRE(journal.redo(doc));
            REQUIRE_MESSAGE(sameRecords(doc, history[i]), "seed ", seed, " redo ", i);
        }
    }
}

TEST_CASE("property: redo-all reaches the final state exactly") {
    for(uint64_t seed = 1; seed <= 25; seed++) {
        Rng rng(seed * 104729u);
        Document doc;
        UndoJournal journal;

        for(int i = 0; i < 40; i++) {
            std::vector<Command> step = randomStep(doc, rng);
            if(step.empty()) continue;
            journal.applyStep(doc, "step", std::move(step));
        }
        const Document final = doc;

        while(journal.undo(doc)) {}
        while(journal.redo(doc)) {}
        CHECK_MESSAGE(doc == final, "seed ", seed);
    }
}

TEST_CASE("property: an undone add never has its id reissued") {
    // Never-reuse outranks byte-identity: the redo record still names the id
    // the undone add took, so reissuing it would rebind that reference.
    Document doc;
    UndoJournal journal;
    EntityRecord p;
    p.kind = EntityKind::Point;

    std::vector<EntityId> issued;
    for(int i = 0; i < 20; i++) {
        REQUIRE(journal.applyStep(doc, "add", AddRecord<EntityRecord>{p}) == CommandError::None);
        issued.push_back(doc.entities().records().back().id);
        REQUIRE(journal.undo(doc));
    }
    for(size_t i = 1; i < issued.size(); i++) CHECK(issued[i - 1] < issued[i]);
}
