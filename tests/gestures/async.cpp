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
#include <set>
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

TEST_CASE("async: a fully-async component still reports its dof and status") {
    // Finding 15: refresh() folds each applied async outcome into the readout, so
    // a component solved off the synchronous branch is counted rather than reading
    // dof -1 / unsolved. The inline scheduler runs the whole async machinery on
    // the calling thread, so the fold is exercised within refresh().
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 90.0, 30.0);
    const EntityId s = addSegment(doc, a, b);
    addConstraint(doc, ConstraintKind::Horizontal, {s});

    // The synchronous readout is the reference the async one must match.
    UndoJournal refJournal;
    Session refSession(doc, refJournal);
    REQUIRE(refSession.presentation().status == SolveStatus::Okay);
    const int syncDof = refSession.presentation().dof;
    REQUIRE(syncDof > 0);  // one relation over four parameters leaves several

    // The same document, solved entirely through the inline scheduler.
    UndoJournal journal;
    Session session(doc, journal);
    session.enableAsyncSolving(/*sizeThreshold=*/0, /*workers=*/0);
    session.refresh();

    CHECK(session.presentation().dof == syncDof);
    CHECK(session.presentation().dof != -1);
    CHECK(session.presentation().status == SolveStatus::Okay);
}

TEST_CASE("async: an inconsistent async component escalates the status") {
    // Finding 15, the status half: applyAsyncResults records the outcome and
    // refresh() folds it, so a failing off-thread component escalates the readout
    // rather than being clobbered back to Okay by the old sync-only recompute.
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 100.0, 0.0);
    addConstraint(doc, ConstraintKind::Pin, {a});
    addConstraint(doc, ConstraintKind::Pin, {b});
    // Both ends pinned 100 apart; a driving distance of 50 cannot hold.
    addConstraint(doc, ConstraintKind::PointPointDistance, {a, b}, Slot(50.0));

    UndoJournal journal;
    Session session(doc, journal);
    session.enableAsyncSolving(/*sizeThreshold=*/0, /*workers=*/0);
    session.refresh();

    // The failure escalates the readout past a passing status rather than being
    // clobbered back to Okay by the old sync-only recompute — the exact failure
    // code (Inconsistent or DidNotConverge) is the solver's to choose.
    const SolveStatus status = session.presentation().status;
    CHECK(status != SolveStatus::Okay);
    CHECK(status != SolveStatus::RedundantOkay);
}

TEST_CASE("async: a solve of a since-edited document is dropped at the epoch tag") {
    // Finding 24: the r.tag != docEpoch_ branch — the drop that per-component
    // generations cannot catch, because an edit can change a component's key. A
    // solve is held in the hook, a partition-changing edit lowers the async
    // component's key so the stale result survives the per-key freshest filter,
    // and the release must see it dropped rather than applied over the edit's
    // answer.
    //
    // The pin is made order-independent, which a single go flag was not: releasing
    // both solves at once let the stale one land in an earlier batch than the
    // fresh one, and then the fresh one overwrote it and the pose looked right even
    // with the epoch check removed. Here the stale pre-edit solve is held in the
    // hook until the fresh post-edit result has been pumped and asserted, so the
    // stale one provably reaches applyAsyncResults last. Only the epoch tag keeps
    // it from being the final write, so a build without the check fails whatever
    // the worker-pool timing.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);  // lowest id; its own component
    const EntityId a = addPoint(doc, 50.0, 0.0);
    const EntityId b = addPoint(doc, 50.0, 80.0);
    const EntityId seg = addSegment(doc, a, b);
    addConstraint(doc, ConstraintKind::Pin, {p});
    addConstraint(doc, ConstraintKind::Vertical, {seg});
    addConstraint(doc, ConstraintKind::PointPointDistance, {a, b}, Slot(80.0));

    UndoJournal journal;
    Session session(doc, journal);

    std::mutex mtx;
    std::condition_variable cv;
    // Each held solve is gated by its own generation: the pre-edit submission gets
    // the lower generation, the post-edit the higher, so releasing them separately
    // is releasing them in a known order without hardcoding the numbers. The hook
    // records every generation it parks in `blocked` and waits until its own is in
    // `released`.
    std::set<uint64_t> blocked;
    std::set<uint64_t> released;

    // Threshold two: the lone point p stays synchronous, {a, b, seg} solves off
    // the thread. Two workers so the pre- and post-edit solves are both in flight,
    // each blocked in the hook until its generation is released.
    session.enableAsyncSolving(/*sizeThreshold=*/2, /*workers=*/2);
    session.setAsyncSolveHook([&](uint64_t generation) {
        std::unique_lock<std::mutex> lock(mtx);
        blocked.insert(generation);
        cv.notify_all();
        cv.wait(lock, [&] { return released.count(generation) != 0; });
    });
    session.refresh();  // submits {a, b, seg} under the first epoch; the worker holds
    REQUIRE(session.asyncBusy());

    // A partition-changing edit through the journal: coincidence(p, a) merges p
    // into the async component, so its key falls to p's and its solve changes — a
    // is pulled onto the pinned p at the origin.
    ConstraintRecord coincide;
    coincide.kind = ConstraintKind::Coincident;
    coincide.operands[0] = p;
    coincide.operands[1] = a;
    REQUIRE(journal.applyStep(doc, "merge", AddRecord<ConstraintRecord>{coincide}) ==
            CommandError::None);
    session.refresh();  // bumps the epoch and resubmits under the new key

    // Wait until both solves are parked in the hook, so their two generations are
    // known: the larger is the fresh post-edit solve, the smaller the stale one.
    uint64_t preEdit = 0, postEdit = 0;
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return blocked.size() == 2; });
        preEdit = *blocked.begin();
        postEdit = *blocked.rbegin();
    }

    // Release only the fresh post-edit solve and pump it in. The stale one stays
    // parked in the hook, so it cannot have pushed a result yet.
    {
        std::lock_guard<std::mutex> lock(mtx);
        released.insert(postEdit);
    }
    cv.notify_all();

    bool landed = false;
    for(int spin = 0; spin < 500000 && !landed; spin++) {
        session.pumpAsync();
        const std::optional<Point> pa = session.pose().point(a);
        REQUIRE(pa);
        if(std::hypot(pa->x, pa->y) < 1e-6) landed = true;
        else std::this_thread::yield();
    }
    REQUIRE(landed);
    // The edit's answer is on screen before the stale solve is even released.
    {
        const std::optional<Point> pa = session.pose().point(a);
        REQUIRE(pa);
        CHECK(std::hypot(pa->x, pa->y) == doctest::Approx(0.0).epsilon(1e-6));
    }

    // Now release the stale pre-edit solve. It reaches applyAsyncResults last, and
    // only the epoch tag stops it from overwriting the edit's answer as the final
    // write — the merge moved a into a lower-keyed component, so the stale result
    // (old key) and the fresh one (new key) sit under different keys and the per-key
    // freshest filter cannot order them; it waves the stale one through.
    {
        std::lock_guard<std::mutex> lock(mtx);
        released.insert(preEdit);
    }
    cv.notify_all();

    for(int spin = 0; spin < 500000 && session.asyncBusy(); spin++) {
        session.pumpAsync();
        std::this_thread::yield();
    }
    for(int i = 0; i < 8; i++) session.pumpAsync();  // apply any final batch

    // The edit's solve still holds: a sits on the pinned origin. The stale pre-edit
    // result, which left a at its seed (50, 0), was dropped at the epoch tag — had
    // it applied last over the newer answer, a would read (50, 0).
    const std::optional<Point> pa = session.pose().point(a);
    REQUIRE(pa);
    CHECK(std::hypot(pa->x, pa->y) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(std::hypot(pa->x - 50.0, pa->y) > 1.0);
}
