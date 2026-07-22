#include <doctest/doctest.h>

#include <string>

#include "core/expression.h"
#include "core/parameters.h"

using paroculus::ExprOp;
using paroculus::formatExpression;
using paroculus::ParameterId;
using paroculus::ParameterRecord;
using paroculus::ParameterTable;
using paroculus::parseExpression;
using paroculus::Slot;
using paroculus::TableParameterEnv;

namespace {

ParameterId addParam(ParameterTable &t, std::string name, Slot value) {
    ParameterRecord r;
    r.name = std::move(name);
    r.value = std::move(value);
    return t.add(std::move(r));
}

double eval(const Slot &s, const ParameterTable &t) {
    const TableParameterEnv env(t);
    return s.evaluate(&env).value();
}

}  // namespace

TEST_CASE("a bare number parses to a constant slot") {
    ParameterTable t;
    const auto s = parseExpression("3.5", t);
    REQUIRE(s);
    CHECK(s->isConstant());
    CHECK(s->constant() == doctest::Approx(3.5));
}

TEST_CASE("a name resolves to the parameter it references") {
    ParameterTable t;
    const ParameterId gutter = addParam(t, "gutter", Slot(8.0));
    const auto s = parseExpression("gutter", t);
    REQUIRE(s);
    CHECK_FALSE(s->isConstant());
    CHECK(s->references() == std::vector<ParameterId>{gutter});
    CHECK(eval(*s, t) == doctest::Approx(8.0));
}

TEST_CASE("arithmetic honors precedence and parentheses") {
    ParameterTable t;
    addParam(t, "a", Slot(2.0));
    addParam(t, "b", Slot(3.0));

    // 2 + 3 * 2 = 8, not 10: multiply binds tighter.
    CHECK(eval(*parseExpression("a + b * 2", t), t) == doctest::Approx(8.0));
    // Parentheses override: (2 + 3) * 2 = 10.
    CHECK(eval(*parseExpression("(a + b) * 2", t), t) == doctest::Approx(10.0));
    // Unary minus and division.
    CHECK(eval(*parseExpression("-a", t), t) == doctest::Approx(-2.0));
    CHECK(eval(*parseExpression("(a + b) / 2", t), t) == doctest::Approx(2.5));
    // Left-associativity: 3 - 2 - 1 = 0, not 2.
    addParam(t, "one", Slot(1.0));
    CHECK(eval(*parseExpression("b - a - one", t), t) == doctest::Approx(0.0));
}

TEST_CASE("an unknown name refuses the whole expression") {
    ParameterTable t;
    addParam(t, "a", Slot(2.0));
    CHECK_FALSE(parseExpression("a + missing", t));
    CHECK_FALSE(parseExpression("missing", t));
}

TEST_CASE("malformed input is refused rather than half-read") {
    ParameterTable t;
    addParam(t, "a", Slot(2.0));
    CHECK_FALSE(parseExpression("", t));
    CHECK_FALSE(parseExpression("a +", t));
    CHECK_FALSE(parseExpression("(a + 1", t));
    CHECK_FALSE(parseExpression("a 1", t));      // trailing junk
    CHECK_FALSE(parseExpression("* a", t));
    CHECK_FALSE(parseExpression("a +* 1", t));
}

TEST_CASE("formatExpression re-parses to an equivalent slot") {
    ParameterTable t;
    addParam(t, "a", Slot(2.0));
    addParam(t, "b", Slot(3.0));
    addParam(t, "c", Slot(5.0));

    for(const char *text : {"a + b * c", "(a + b) * c", "a - (b - c)", "-a * b", "a / b / c",
                            "a - b - c", "(a + b) / (b - c)"}) {
        CAPTURE(text);
        const auto original = parseExpression(text, t);
        REQUIRE(original);
        const std::string rendered = formatExpression(*original, t);
        const auto reparsed = parseExpression(rendered, t);
        REQUIRE(reparsed);
        CHECK(eval(*reparsed, t) == doctest::Approx(eval(*original, t)));
    }
}

TEST_CASE("deeply nested input is refused, not a stack overflow") {
    // Text arrives from panel paste and hand-edited scripts, so evaluation must
    // terminate on any input. Thousands of open parentheses, and thousands of
    // unary minuses, both refuse rather than overflow.
    ParameterTable t;
    std::string parens(20000, '(');
    parens += "1";
    CHECK_FALSE(parseExpression(parens, t));
    CHECK_FALSE(parseExpression(std::string(20000, '-') + "1", t));
    // A merely deep-but-legal expression under the bound still parses.
    CHECK(parseExpression("((((((1))))))", t));
}

TEST_CASE("a constant slot formats as its number") {
    ParameterTable t;
    CHECK(formatExpression(Slot(4.0), t) == "4");
}
