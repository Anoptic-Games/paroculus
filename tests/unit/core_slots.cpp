#include <doctest/doctest.h>

#include "core/slots.h"

using paroculus::ExprOp;
using paroculus::ParameterEnv;
using paroculus::ParameterId;
using paroculus::Slot;

namespace {

struct FixedEnv : ParameterEnv {
    std::optional<double> lookup(ParameterId id) const override {
        if(id == ParameterId(1)) return 10.0;
        if(id == ParameterId(2)) return 4.0;
        return std::nullopt;
    }
};

}  // namespace

TEST_CASE("a constant is the trivial expression and costs no nodes") {
    const Slot s(7.5);
    CHECK(s.isConstant());
    CHECK(s.nodes().empty());
    CHECK(s.constant() == 7.5);
    CHECK(s.evaluate(nullptr) == 7.5);
}

TEST_CASE("arithmetic over constants and parameters evaluates") {
    const FixedEnv env;
    // (p1 + p2) * 2  ==  (10 + 4) * 2
    const Slot sum = Slot::binary(ExprOp::Add, Slot::parameter(ParameterId(1)),
                                  Slot::parameter(ParameterId(2)));
    const Slot expr = Slot::binary(ExprOp::Multiply, sum, Slot(2.0));

    CHECK_FALSE(expr.isConstant());
    const auto v = expr.evaluate(&env);
    REQUIRE(v.has_value());
    CHECK(*v == doctest::Approx(28.0));
}

TEST_CASE("each operator computes what it says") {
    const FixedEnv env;
    const Slot a(9.0), b(2.0);
    CHECK(*Slot::binary(ExprOp::Add, a, b).evaluate(&env) == doctest::Approx(11.0));
    CHECK(*Slot::binary(ExprOp::Subtract, a, b).evaluate(&env) == doctest::Approx(7.0));
    CHECK(*Slot::binary(ExprOp::Multiply, a, b).evaluate(&env) == doctest::Approx(18.0));
    CHECK(*Slot::binary(ExprOp::Divide, a, b).evaluate(&env) == doctest::Approx(4.5));
    CHECK(*Slot::negate(a).evaluate(&env) == doctest::Approx(-9.0));
}

TEST_CASE("an unresolvable reference yields no value, never a silent zero") {
    const FixedEnv env;
    const Slot missing = Slot::parameter(ParameterId(99));
    CHECK_FALSE(missing.evaluate(&env).has_value());
    // A null environment cannot resolve anything either.
    CHECK_FALSE(missing.evaluate(nullptr).has_value());
    // The failure propagates rather than poisoning the arithmetic with a zero.
    CHECK_FALSE(Slot::binary(ExprOp::Add, missing, Slot(1.0)).evaluate(&env).has_value());
}

TEST_CASE("division by zero yields no value") {
    const FixedEnv env;
    CHECK_FALSE(Slot::binary(ExprOp::Divide, Slot(1.0), Slot(0.0)).evaluate(&env).has_value());
}

TEST_CASE("references are reported in first-appearance order without duplicates") {
    const Slot p1 = Slot::parameter(ParameterId(1));
    const Slot p2 = Slot::parameter(ParameterId(2));
    const Slot expr = Slot::binary(ExprOp::Add, Slot::binary(ExprOp::Multiply, p2, p1), p2);

    const std::vector<ParameterId> refs = expr.references();
    REQUIRE(refs.size() == 2);
    CHECK(refs[0] == ParameterId(2));
    CHECK(refs[1] == ParameterId(1));
}

TEST_CASE("a constant slot references nothing") {
    CHECK(Slot(3.0).references().empty());
}

TEST_CASE("composition reindexes operands so slots stay self-contained") {
    const FixedEnv env;
    // Deep nesting on both sides is where a reindexing bug would show up.
    const Slot left = Slot::binary(ExprOp::Add, Slot(1.0), Slot(2.0));
    const Slot right = Slot::binary(ExprOp::Multiply, Slot(3.0), Slot(4.0));
    const Slot both = Slot::binary(ExprOp::Subtract, right, left);
    CHECK(*both.evaluate(&env) == doctest::Approx(9.0));

    // Copies are independent of the originals they were built from.
    const Slot copy = both;
    CHECK(copy == both);
    CHECK(*copy.evaluate(&env) == doctest::Approx(9.0));
}

TEST_CASE("equality is structural, not semantic") {
    // v1 does no constant folding, and persist plus undo byte-identity both
    // want the authored form preserved exactly.
    CHECK(Slot(5.0) == Slot(5.0));
    CHECK(Slot(5.0) != Slot(6.0));
    CHECK(Slot::binary(ExprOp::Add, Slot(2.0), Slot(3.0)) != Slot(5.0));
    CHECK(Slot::binary(ExprOp::Add, Slot(2.0), Slot(3.0)) ==
          Slot::binary(ExprOp::Add, Slot(2.0), Slot(3.0)));
    CHECK(Slot::binary(ExprOp::Add, Slot(2.0), Slot(3.0)) !=
          Slot::binary(ExprOp::Subtract, Slot(2.0), Slot(3.0)));
    CHECK(Slot::parameter(ParameterId(1)) != Slot::parameter(ParameterId(2)));
}
