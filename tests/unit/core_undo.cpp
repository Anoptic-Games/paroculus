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

    const uint32_t choice = rng.below(12);

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

    // Organization: layers and styles, made and then hung off geometry. Without
    // this the property never reaches a document where a layer is in use, which
    // is the only state its removal is interesting in.
    std::vector<LayerId> layers;
    for(const LayerRecord &l : doc.layers().records()) layers.push_back(l.id);
    std::vector<StyleId> styles;
    for(const StyleRecord &s : doc.styles().records()) styles.push_back(s.id);

    if(choice == 9) {
        if(layers.empty()) {
            LayerRecord l;
            l.name = "layer";
            l.order = static_cast<int32_t>(rng.below(5));
            step.push_back(AddRecord<LayerRecord>{l});
            return step;
        }
        if(styles.empty()) {
            StyleRecord s;
            s.name = "style";
            s.strokeWidth = Slot(rng.real(0.5, 4.0));
            step.push_back(AddRecord<StyleRecord>{s});
            return step;
        }
        EntityRecord attached = *doc.entities().find(
            entities[rng.below(entities.size())].id);
        attached.layer = layers[rng.below(layers.size())];
        attached.style = styles[rng.below(styles.size())];
        step.push_back(SetRecord<EntityRecord>{std::move(attached)});
        return step;
    }

    // And removing one, which is the path that used to leave every entity on it
    // naming a record that is gone — a document that saves and will not load.
    // Expressed as a step, like every other removal that could dangle.
    if(choice == 10) {
        if(!layers.empty() && rng.chance(2)) {
            return deletionStep(doc, layers[rng.below(layers.size())]);
        }
        if(!styles.empty()) return deletionStep(doc, styles[rng.below(styles.size())]);
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

TEST_CASE("many adds to one table in one step invert just in time, at every prefix") {
    // The composite gesture above pairs adds across two tables. Two-plus adds to
    // the SAME table within one step is the case the property corpus never
    // reaches: each inverse is computed against the document the previous add
    // already mutated, so the second must name the id the allocator will hand
    // the second — accounting for the first's allocation rather than colliding
    // with it. Checked at every prefix, because a bug that cancels out over the
    // whole run still corrupts the middle.
    Document doc;
    UndoJournal journal;

    std::vector<Document> history;
    history.push_back(doc);

    // Four steps, each adding two or three points at once. Null ids throughout,
    // so every inverse is a just-in-time allocation prediction.
    for(int stepIndex = 0; stepIndex < 4; stepIndex++) {
        const int adds = 2 + (stepIndex % 2);  // 2, 3, 2, 3
        std::vector<Command> step;
        for(int j = 0; j < adds; j++) {
            EntityRecord p;
            p.kind = EntityKind::Point;
            p.seeds = {static_cast<double>(stepIndex), static_cast<double>(j)};
            step.push_back(AddRecord<EntityRecord>{p});
        }
        const size_t before = doc.entities().size();
        REQUIRE(journal.applyStep(doc, "adds", std::move(step)) == CommandError::None);
        REQUIRE(doc.entities().size() == before + static_cast<size_t>(adds));
        history.push_back(doc);
    }

    // The ids landed distinct and monotonic: no inverse predicted an id a later
    // add in the same step also took.
    const auto &recs = doc.entities().records();
    for(size_t i = 1; i < recs.size(); i++) CHECK(recs[i - 1].id < recs[i].id);

    // All the way back, checking each prefix. sameRecords rather than ==: undo
    // restores every record exactly but never rewinds a watermark.
    for(size_t i = history.size(); i-- > 1;) {
        REQUIRE(journal.undo(doc));
        CHECK(sameRecords(doc, history[i - 1]));
    }
    CHECK_FALSE(journal.canUndo());

    // And all the way forward, re-landing the identical records at each prefix
    // because every add was pinned to the id it first took.
    for(size_t i = 1; i < history.size(); i++) {
        REQUIRE(journal.redo(doc));
        CHECK(sameRecords(doc, history[i]));
    }
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

TEST_CASE("revision advances on every mutation and never falsely matches a saved point") {
    // The dirty-tracking contract: the shell captures revision() at save and
    // calls the document dirty when it no longer matches. A committed step, an
    // undo and a redo each count as a mutation, and the tricky case a bare depth
    // comparison gets wrong is proven directly.
    Document doc;
    UndoJournal journal;
    EntityRecord p;
    p.kind = EntityKind::Point;

    CHECK(journal.revision() == 0);  // a fresh journal is a clean, unsaved point

    REQUIRE(journal.applyStep(doc, "a", AddRecord<EntityRecord>{p}) == CommandError::None);
    REQUIRE(journal.applyStep(doc, "b", AddRecord<EntityRecord>{p}) == CommandError::None);
    REQUIRE(journal.applyStep(doc, "c", AddRecord<EntityRecord>{p}) == CommandError::None);
    const uint64_t saved = journal.revision();  // "save" here

    // Undo then redo returns to the same depth and the same document, but each
    // move is a mutation, so revision has advanced past the saved point. Reading
    // dirty there is the conservative error a data-loss prompt must make.
    REQUIRE(journal.undo(doc));
    CHECK(journal.revision() != saved);
    REQUIRE(journal.redo(doc));
    CHECK(journal.revision() != saved);

    // The false-clean a depth comparison would produce: save, undo, then a new
    // step truncates the redo tail and lands at the same depth with a different
    // document. depth() matches the saved depth; revision() must not.
    const size_t savedDepth = journal.depth();
    const uint64_t savedRev = journal.revision();
    REQUIRE(journal.undo(doc));
    REQUIRE(journal.applyStep(doc, "d", AddRecord<EntityRecord>{p}) == CommandError::None);
    CHECK(journal.depth() == savedDepth);       // same position...
    CHECK(journal.revision() != savedRev);      // ...different document, correctly dirty

    journal.clear();
    CHECK(journal.revision() == 0);  // a cleared journal is a fresh start
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
