// The asynchronous solve path, exercised through the session it degrades.
//
// Three properties the plan names, at the level a session sees them: the
// synchronous path is unchanged until async is opted into; an async result
// equals the synchronous one; and a component solving off-thread shows its last
// coherent pose throughout and lands the new one whole — never a partial solution
// blended into a coherent one. The worker is gated by the injected-delay hook so
// "lands whole" is a controlled sequence rather than a timing hope.
#include <doctest/doctest.h>

#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "core/undo.h"
#include "interact/session.h"
#include "support/build.h"

using namespace paroculus;
using namespace paroculus::test;

namespace {

Document solvableDoc() {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 90.0, 30.0);
    const EntityId s = addSegment(doc, a, b);
    addConstraint(doc, ConstraintKind::Pin, {a});
    addConstraint(doc, ConstraintKind::Horizontal, {s});
    addConstraint(doc, ConstraintKind::PointPointDistance, {a, b}, Slot(100.0));
    return doc;
}

EntityId farPoint(const Document &doc) {
    for(const EntityRecord &e : doc.entities().records())
        if(e.kind == EntityKind::Point && e.seeds[0] == 90.0) return e.id;
    return EntityId();
}

Point syncSolution(const Document &doc, EntityId b) {
    SolveContext ref = SolveContext::forWholeDocument(doc);
    REQUIRE(solve(doc, ref).ok());
    const std::optional<Point> p = ref.point(b);
    REQUIRE(p);
    return *p;
}

}  // namespace

TEST_CASE("async: the synchronous path solves in-frame until async is enabled") {
    Document doc = solvableDoc();
    const EntityId b = farPoint(doc);
    UndoJournal journal;
    Session session(doc, journal);  // the ctor's refresh solves synchronously

    const std::optional<Point> p = session.pose().point(b);
    REQUIRE(p);
    CHECK(p->x == doctest::Approx(100.0).epsilon(1e-6));
    CHECK(p->y == doctest::Approx(0.0).epsilon(1e-6));
    CHECK_FALSE(session.asyncBusy());
}

TEST_CASE("async: inline execution solves through the machinery and matches sync") {
    Document doc = solvableDoc();
    const EntityId b = farPoint(doc);
    const Point want = syncSolution(doc, b);

    UndoJournal journal;
    Session session(doc, journal);
    session.enableAsyncSolving(/*sizeThreshold=*/0, /*workers=*/0);  // all async, inline
    session.refresh();

    const std::optional<Point> p = session.pose().point(b);
    REQUIRE(p);
    CHECK(p->x == doctest::Approx(want.x).epsilon(1e-9));
    CHECK(p->y == doctest::Approx(want.y).epsilon(1e-9));
    CHECK_FALSE(session.asyncBusy());
}

TEST_CASE("async: a threaded solve holds the last coherent pose, then lands whole") {
    Document doc = solvableDoc();
    const EntityId b = farPoint(doc);
    const Point want = syncSolution(doc, b);
    const Point seed{90.0, 30.0};

    UndoJournal journal;
    Session session(doc, journal);

    std::mutex mtx;
    std::condition_variable cv;
    bool go = false;

    session.enableAsyncSolving(/*sizeThreshold=*/0, /*workers=*/2);
    session.setAsyncSolveHook([&](uint64_t) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return go; });
    });
    session.refresh();  // submits async; the worker blocks in the hook

    // The component keeps its committed-seed pose while the worker is held — a
    // coherent state, not a partial one, and the UI did not block to get it.
    {
        const std::optional<Point> p = session.pose().point(b);
        REQUIRE(p);
        CHECK(p->x == doctest::Approx(seed.x));
        CHECK(p->y == doctest::Approx(seed.y));
    }
    CHECK(session.asyncBusy());

    // Release the worker and pump. Every pose observed on the way is either the
    // seed pose or the solved pose — the result is applied whole, never blended.
    {
        std::lock_guard<std::mutex> lock(mtx);
        go = true;
    }
    cv.notify_all();

    bool landed = false;
    for(int spin = 0; spin < 200000 && !landed; spin++) {
        const bool changed = session.pumpAsync();
        const std::optional<Point> p = session.pose().point(b);
        REQUIRE(p);
        const bool atSeed = std::hypot(p->x - seed.x, p->y - seed.y) < 1e-6;
        const bool atSolution = std::hypot(p->x - want.x, p->y - want.y) < 1e-6;
        CHECK(atSeed != atSolution);  // exactly one — coherent, never partial
        if(atSolution) landed = true;
        if(!changed) std::this_thread::yield();
    }
    CHECK(landed);

    const std::optional<Point> p = session.pose().point(b);
    REQUIRE(p);
    CHECK(p->x == doctest::Approx(want.x).epsilon(1e-9));
    CHECK(p->y == doctest::Approx(want.y).epsilon(1e-9));
}
