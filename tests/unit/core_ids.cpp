#include <doctest/doctest.h>

#include <type_traits>
#include <unordered_set>

#include "core/ids.h"

using paroculus::ConstraintId;
using paroculus::EntityId;
using paroculus::IdAllocator;

TEST_CASE("a default id is null and never issued") {
    const EntityId none;
    CHECK_FALSE(none.valid());
    CHECK(none.value() == 0u);

    IdAllocator<EntityId> alloc;
    for(int i = 0; i < 100; i++) CHECK(alloc.allocate() != none);
}

TEST_CASE("ids are issued in increasing order and never repeat") {
    IdAllocator<EntityId> alloc;
    std::unordered_set<EntityId> seen;
    EntityId previous;
    for(int i = 0; i < 1000; i++) {
        const EntityId id = alloc.allocate();
        CHECK(seen.insert(id).second);
        CHECK(previous < id);
        previous = id;
    }
}

TEST_CASE("removal does not recycle") {
    // The point of the whole design: a freed ID must never come back, because
    // a stale reference would silently rebind to a different object.
    IdAllocator<EntityId> alloc;
    const EntityId first = alloc.allocate();
    const EntityId second = alloc.allocate();
    // Pretend both were deleted; the allocator is told nothing, by design.
    const EntityId third = alloc.allocate();
    CHECK(third != first);
    CHECK(third != second);
    CHECK(second < third);
}

TEST_CASE("reserveAbove keeps loaded ids out of the issue range") {
    IdAllocator<EntityId> alloc;
    alloc.reserveAbove(EntityId(500));
    const EntityId next = alloc.allocate();
    CHECK(next.value() == 501u);

    // A lower watermark must not walk the counter backwards.
    alloc.reserveAbove(EntityId(10));
    CHECK(alloc.allocate().value() == 502u);
}

TEST_CASE("the allocator watermark round-trips") {
    // Serialized with the document: a reopened file that reset this counter
    // would hand a live ID to a new object.
    IdAllocator<EntityId> alloc;
    for(int i = 0; i < 7; i++) alloc.allocate();
    const auto saved = alloc.next();

    IdAllocator<EntityId> loaded;
    loaded.setNext(saved);
    CHECK(loaded.allocate().value() == 8u);
}

TEST_CASE("setNext refuses to make the null id issuable") {
    IdAllocator<EntityId> alloc;
    alloc.setNext(0);
    CHECK(alloc.allocate().valid());
}

TEST_CASE("ids over different domains are distinct types") {
    // Compile-time guarantee; the runtime check is incidental. If these were
    // the same type the assignment below would compile.
    static_assert(!std::is_convertible_v<EntityId, ConstraintId>);
    static_assert(!std::is_convertible_v<ConstraintId, EntityId>);
    CHECK(EntityId(3).value() == ConstraintId(3).value());
}
