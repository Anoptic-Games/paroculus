#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "core/parameters.h"

using paroculus::ExprOp;
using paroculus::ParameterId;
using paroculus::ParameterRecord;
using paroculus::ParameterTable;
using paroculus::Slot;
using paroculus::evaluateParameter;
using paroculus::findParameterByName;
using paroculus::wouldCycle;

namespace {

ParameterId addParam(ParameterTable &t, std::string name, Slot value) {
    ParameterRecord r;
    r.name = std::move(name);
    r.value = std::move(value);
    return t.add(std::move(r));
}

// Mirrors what the document's command layer does: check, then assign.
bool setValue(ParameterTable &t, ParameterId id, Slot value) {
    const ParameterRecord *existing = t.find(id);
    if(existing == nullptr) return false;
    if(wouldCycle(t, id, value)) return false;
    ParameterRecord updated = *existing;
    updated.value = std::move(value);
    return t.set(std::move(updated));
}

}  // namespace

TEST_CASE("parameters evaluate through references") {
    ParameterTable t;
    const ParameterId gutter = addParam(t, "gutter", Slot(8.0));
    const ParameterId margin =
        addParam(t, "margin", Slot::binary(ExprOp::Multiply, Slot::parameter(gutter), Slot(2.0)));

    CHECK(*evaluateParameter(t, gutter) == doctest::Approx(8.0));
    CHECK(*evaluateParameter(t, margin) == doctest::Approx(16.0));

    // Editing the base propagates; that is the whole point of naming it.
    REQUIRE(setValue(t, gutter, Slot(10.0)));
    CHECK(*evaluateParameter(t, margin) == doctest::Approx(20.0));
}

TEST_CASE("a self-reference is rejected") {
    ParameterTable t;
    const ParameterId a = addParam(t, "a", Slot(1.0));
    CHECK(wouldCycle(t, a, Slot::parameter(a)));
    CHECK_FALSE(setValue(t, a, Slot::parameter(a)));
    CHECK(*evaluateParameter(t, a) == doctest::Approx(1.0));
}

TEST_CASE("an indirect cycle is rejected") {
    ParameterTable t;
    const ParameterId a = addParam(t, "a", Slot(1.0));
    const ParameterId b = addParam(t, "b", Slot::parameter(a));
    const ParameterId c = addParam(t, "c", Slot::parameter(b));

    // c -> b -> a, so a -> c would close the loop.
    CHECK(wouldCycle(t, a, Slot::parameter(c)));
    CHECK_FALSE(setValue(t, a, Slot::parameter(c)));
    CHECK(*evaluateParameter(t, c) == doctest::Approx(1.0));

    // A cycle buried inside arithmetic is still a cycle.
    CHECK(wouldCycle(t, a, Slot::binary(ExprOp::Add, Slot(1.0), Slot::parameter(c))));
}

TEST_CASE("a diamond is not a cycle") {
    ParameterTable t;
    const ParameterId base = addParam(t, "base", Slot(2.0));
    const ParameterId left = addParam(t, "left", Slot::parameter(base));
    const ParameterId right = addParam(t, "right", Slot::parameter(base));
    const ParameterId top = addParam(
        t, "top", Slot::binary(ExprOp::Add, Slot::parameter(left), Slot::parameter(right)));

    CHECK_FALSE(
        wouldCycle(t, top, Slot::binary(ExprOp::Add, Slot::parameter(left),
                                        Slot::parameter(right))));
    CHECK(*evaluateParameter(t, top) == doctest::Approx(4.0));
}

TEST_CASE("an unresolvable reference yields no value") {
    ParameterTable t;
    const ParameterId a = addParam(t, "a", Slot::parameter(ParameterId(999)));
    CHECK_FALSE(evaluateParameter(t, a).has_value());
    CHECK_FALSE(evaluateParameter(t, ParameterId(999)).has_value());
}

TEST_CASE("lookup by name finds the record") {
    ParameterTable t;
    const ParameterId a = addParam(t, "alpha", Slot(1.0));
    REQUIRE(findParameterByName(t, "alpha") != nullptr);
    CHECK(findParameterByName(t, "alpha")->id == a);
    CHECK(findParameterByName(t, "nope") == nullptr);
}

TEST_CASE("evaluation terminates on a hand-edited loop") {
    // wouldCycle guards the front door; the depth bound contains a loop that
    // arrived some other way, such as a file edited by hand.
    ParameterTable t;
    ParameterRecord a;
    a.id = ParameterId(1);
    a.name = "a";
    a.value = Slot::parameter(ParameterId(2));
    ParameterRecord b;
    b.id = ParameterId(2);
    b.name = "b";
    b.value = Slot::parameter(ParameterId(1));
    REQUIRE(t.addAt(a));
    REQUIRE(t.addAt(b));
    CHECK_FALSE(evaluateParameter(t, ParameterId(1)).has_value());
}

TEST_CASE("a deep doubling chain evaluates in linear time") {
    // Each parameter reads the one below it through two distinct nodes, so the
    // per-slot forward pass resolves that reference twice. Without the
    // cross-slot memo the work doubles at every level, and a depth-60 chain
    // costs 2^60 — longer than the machine's lifetime — so the test completing
    // at all is what pins the fix. The value is exact: doubling a power of two
    // is exact in a double.
    ParameterTable t;
    ParameterId prev = addParam(t, "p0", Slot(1.0));
    for(int k = 1; k <= 60; k++) {
        const Slot doubled =
            Slot::binary(ExprOp::Add, Slot::parameter(prev), Slot::parameter(prev));
        prev = addParam(t, "p" + std::to_string(k), doubled);
    }
    const auto v = evaluateParameter(t, prev);
    REQUIRE(v.has_value());
    CHECK(*v == std::ldexp(1.0, 60));
}

TEST_CASE("a diamond resolves its shared parameter once and consistently") {
    // base is reached through two arms of the same evaluation; the memo keys on
    // its id, so both arms see the same value and the walk does not branch.
    ParameterTable t;
    const ParameterId base = addParam(t, "base", Slot(3.0));
    const ParameterId left =
        addParam(t, "left", Slot::binary(ExprOp::Multiply, Slot::parameter(base), Slot(2.0)));
    const ParameterId right =
        addParam(t, "right", Slot::binary(ExprOp::Add, Slot::parameter(base), Slot(1.0)));
    const ParameterId top = addParam(
        t, "top", Slot::binary(ExprOp::Add, Slot::parameter(left), Slot::parameter(right)));

    // (3 * 2) + (3 + 1) == 10.
    CHECK(*evaluateParameter(t, top) == doctest::Approx(10.0));
}

TEST_CASE("memoization does not defeat cycle termination") {
    // A hand-edited 3-cycle a -> b -> c -> a. wouldCycle would refuse it, but a
    // file can arrive holding it. A cycle never completes, so it never caches a
    // value; the depth bound still terminates it to nullopt with the memo in
    // place, and does not hang.
    ParameterTable t;
    ParameterRecord a;
    a.id = ParameterId(1);
    a.name = "a";
    a.value = Slot::parameter(ParameterId(2));
    ParameterRecord b;
    b.id = ParameterId(2);
    b.name = "b";
    b.value = Slot::parameter(ParameterId(3));
    ParameterRecord c;
    c.id = ParameterId(3);
    c.name = "c";
    c.value = Slot::parameter(ParameterId(1));
    REQUIRE(t.addAt(a));
    REQUIRE(t.addAt(b));
    REQUIRE(t.addAt(c));
    CHECK_FALSE(evaluateParameter(t, ParameterId(1)).has_value());
    CHECK_FALSE(evaluateParameter(t, ParameterId(2)).has_value());
}
