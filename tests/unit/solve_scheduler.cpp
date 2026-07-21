// The asynchronous solve path.
//
// The properties the plan names, tested at the level that makes each
// deterministic. The lock-free ring and the discard policy are pure and tested
// as such. Determinism — an async result equals the synchronous one — is tested
// both inline and on real worker threads. Stale-drop and no-partial-blend are
// tested through the injected-delay hook, which lets a test hold one generation
// back while a newer one overtakes it, so "provably dropped" is a controlled
// sequence rather than a timing hope.
#include <doctest/doctest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "solve/scheduler.h"
#include "solve/spsc.h"
#include "support/build.h"

using namespace paroculus;
using namespace paroculus::test;

namespace {

// A single determinate component: a segment pinned at the origin, held
// horizontal, one unit-distance apart — seeded off the solution so a solve has
// to actually move the far point.
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
    // The second point added, seeded at (90, 30).
    for(const EntityRecord &e : doc.entities().records())
        if(e.kind == EntityKind::Point && e.seeds[0] == 90.0) return e.id;
    return EntityId();
}

SolveResult make(uint64_t key, uint64_t gen) {
    SolveResult r;
    r.key = key;
    r.generation = gen;
    return r;
}

}  // namespace

TEST_CASE("spsc ring: fifo, full and empty are distinct, and it wraps") {
    SpscRing<int> ring(4);  // rounds up to a power of two, one slot reserved
    CHECK(ring.empty());
    int count = 0;
    while(ring.push(count)) count++;
    CHECK(count >= 3);            // usable capacity is capacity-1
    CHECK_FALSE(ring.push(999));  // full
    CHECK_FALSE(ring.empty());

    for(int i = 0; i < count; i++) {
        const std::optional<int> v = ring.pop();
        REQUIRE(v);
        CHECK(*v == i);           // fifo
    }
    CHECK(ring.empty());
    CHECK_FALSE(ring.pop());

    // And it keeps working after a wrap.
    for(int cycle = 0; cycle < 10; cycle++) {
        REQUIRE(ring.push(cycle));
        const std::optional<int> v = ring.pop();
        REQUIRE(v);
        CHECK(*v == cycle);
    }
}

TEST_CASE("selectFreshest keeps the newest per key and drops the stale") {
    std::map<uint64_t, uint64_t> applied;

    SUBCASE("newest of a batch wins, older ones are superseded") {
        std::vector<SolveResult> batch;
        batch.push_back(make(1, 3));
        batch.push_back(make(1, 5));
        batch.push_back(make(1, 4));
        batch.push_back(make(2, 2));
        const std::vector<SolveResult> kept = selectFreshest(std::move(batch), applied);
        REQUIRE(kept.size() == 2);            // one per key
        // Key 1 keeps generation 5, key 2 keeps 2.
        for(const SolveResult &r : kept) {
            if(r.key == 1) CHECK(r.generation == 5);
            if(r.key == 2) CHECK(r.generation == 2);
        }
        CHECK(applied[1] == 5);
        CHECK(applied[2] == 2);
    }

    SUBCASE("a result older than what a key already applied is dropped") {
        applied[1] = 5;
        std::vector<SolveResult> late;
        late.push_back(make(1, 4));  // arrives after 5 was applied — stale
        const std::vector<SolveResult> kept = selectFreshest(std::move(late), applied);
        CHECK(kept.empty());          // dropped, never regresses the pose
        CHECK(applied[1] == 5);       // and the applied mark does not move back

        // A genuinely newer one still gets through.
        std::vector<SolveResult> newer;
        newer.push_back(make(1, 6));
        const std::vector<SolveResult> next = selectFreshest(std::move(newer), applied);
        REQUIRE(next.size() == 1);
        CHECK(next.front().generation == 6);
        CHECK(applied[1] == 6);
    }
}

TEST_CASE("inline scheduler: an async result equals the synchronous solve") {
    const Document doc = solvableDoc();
    const EntityId b = farPoint(doc);
    REQUIRE(b.valid());

    SolveContext reference = SolveContext::forWholeDocument(doc);
    const SolveOutcome sync = solve(doc, reference);
    REQUIRE(sync.ok());
    const std::optional<Point> want = reference.point(b);
    REQUIRE(want);

    SolveScheduler scheduler(0);  // inline
    auto snapshot = std::make_shared<const Document>(doc);
    scheduler.submit(1, snapshot, SolveContext::forWholeDocument(doc));

    const std::vector<SolveResult> results = scheduler.drain();
    REQUIRE(results.size() == 1);
    const std::optional<Point> got = results.front().context.point(b);
    REQUIRE(got);
    CHECK(got->x == doctest::Approx(want->x).epsilon(1e-9));
    CHECK(got->y == doctest::Approx(want->y).epsilon(1e-9));
    // Whole, not partial: the context carries every param-owning member.
    CHECK(results.front().context.params().size() == reference.params().size());
}

TEST_CASE("threaded scheduler: a worker-solved component equals the synchronous solve") {
    const Document doc = solvableDoc();
    const EntityId b = farPoint(doc);
    SolveContext reference = SolveContext::forWholeDocument(doc);
    REQUIRE(solve(doc, reference).ok());
    const std::optional<Point> want = reference.point(b);
    REQUIRE(want);

    SolveScheduler scheduler(2);
    auto snapshot = std::make_shared<const Document>(doc);
    scheduler.submit(42, snapshot, SolveContext::forWholeDocument(doc));

    std::optional<SolveResult> got;
    for(int spin = 0; spin < 100000 && !got; spin++) {
        for(SolveResult &r : scheduler.drain()) got = std::move(r);
        std::this_thread::yield();
    }
    REQUIRE(got);
    const std::optional<Point> where = got->context.point(b);
    REQUIRE(where);
    CHECK(where->x == doctest::Approx(want->x).epsilon(1e-9));
    CHECK(where->y == doctest::Approx(want->y).epsilon(1e-9));
}

TEST_CASE("threaded scheduler: a stale generation is provably dropped") {
    // Two submits for one key land on two workers. The hook holds the older one
    // until the newer one has been drained and applied; when the older finally
    // arrives it is dropped, so the pose never regresses.
    const Document doc = solvableDoc();
    auto snapshot = std::make_shared<const Document>(doc);

    std::mutex mtx;
    std::condition_variable cv;
    std::set<uint64_t> allowed;
    auto hold = [&](uint64_t gen) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return allowed.count(gen) > 0; });
    };
    auto release = [&](uint64_t gen) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            allowed.insert(gen);
        }
        cv.notify_all();
    };

    SolveScheduler scheduler(2);
    scheduler.setSolveHook(hold);

    const uint64_t g1 = scheduler.submit(7, snapshot, SolveContext::forWholeDocument(doc));
    const uint64_t g2 = scheduler.submit(7, snapshot, SolveContext::forWholeDocument(doc));
    REQUIRE(g2 > g1);

    // Let the newer one finish first and drain it.
    release(g2);
    std::vector<SolveResult> survivors;
    bool sawG2 = false;
    for(int spin = 0; spin < 100000 && !sawG2; spin++) {
        for(SolveResult &r : scheduler.drain()) {
            if(r.generation == g2) sawG2 = true;
            survivors.push_back(std::move(r));
        }
        std::this_thread::yield();
    }
    REQUIRE(sawG2);

    // Now let the older one arrive. It must be discarded rather than applied.
    release(g1);
    for(int spin = 0; spin < 100000 && !scheduler.idle(); spin++) {
        for(SolveResult &r : scheduler.drain()) survivors.push_back(std::move(r));
        std::this_thread::yield();
    }
    for(SolveResult &r : scheduler.drain()) survivors.push_back(std::move(r));

    bool sawG1 = false;
    for(const SolveResult &r : survivors)
        if(r.generation == g1) sawG1 = true;
    CHECK_FALSE(sawG1);  // the stale generation never surfaced
}
