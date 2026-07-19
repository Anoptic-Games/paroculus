#include <doctest/doctest.h>

#include <cmath>

#include "core/units.h"

using paroculus::ParsedLength;
using paroculus::Unit;
using paroculus::formatLength;
using paroculus::parseLength;
using paroculus::unitFromSuffix;

TEST_CASE("parsing converts to millimetres at the boundary") {
    struct Case { const char *text; double mm; };
    const Case cases[] = {
        {"12", 12.0}, {"12mm", 12.0}, {"1cm", 10.0}, {"1m", 1000.0},
        {"1in", 25.4}, {"72pt", 25.4}, {"-3.5mm", -3.5}, {"  2 cm  ", 20.0},
    };
    for(const Case &c : cases) {
        const auto r = parseLength(c.text, Unit::Millimetre);
        REQUIRE_MESSAGE(r.has_value(), c.text);
        CHECK(std::fabs(r->millimetres - c.mm) <= 1e-9);
    }
}

TEST_CASE("a bare number takes the fallback unit") {
    const auto mm = parseLength("5", Unit::Millimetre);
    const auto inches = parseLength("5", Unit::Inch);
    REQUIRE(mm.has_value());
    REQUIRE(inches.has_value());
    CHECK(mm->millimetres == doctest::Approx(5.0));
    CHECK(inches->millimetres == doctest::Approx(127.0));
    // The caller needs to know whether the user pinned a unit or inherited one.
    CHECK_FALSE(mm->hadSuffix);
    CHECK(parseLength("5mm", Unit::Inch)->hadSuffix);
}

TEST_CASE("trailing garbage is a parse failure, not a truncation") {
    // Accepting "12mmm" as 12mm is how a typo becomes a wrong document.
    for(const char *bad : {"12mmm", "12 furlongs", "mm", "", "   ", "1.2.3", "--4", "12mm5"}) {
        CHECK_MESSAGE(!parseLength(bad, Unit::Millimetre).has_value(), bad);
    }
}

TEST_CASE("non-finite input is rejected") {
    CHECK_FALSE(parseLength("inf", Unit::Millimetre).has_value());
    CHECK_FALSE(parseLength("nan", Unit::Millimetre).has_value());
}

TEST_CASE("formatting is presentation only and never round-trips into storage") {
    // The hygiene rule: an edit session opens on the stored value, not on the
    // string rendered from it. This test pins the loss so the rule stays
    // visible — 1/3 mm does not survive two decimal places, and must not be
    // expected to.
    const double stored = 1.0 / 3.0;
    const std::string shown = formatLength(stored, Unit::Millimetre, 2);
    CHECK(shown == "0.33mm");

    const auto reparsed = parseLength(shown, Unit::Millimetre);
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->millimetres != stored);
}

TEST_CASE("formatting emits the unit suffix and converts out of millimetres") {
    CHECK(formatLength(25.4, Unit::Inch, 3) == "1.000in");
    CHECK(formatLength(10.0, Unit::Centimetre, 1) == "1.0cm");
    CHECK(formatLength(1000.0, Unit::Metre, 0) == "1m");
    CHECK(formatLength(0.0, Unit::Millimetre, 2) == "0.00mm");
}

TEST_CASE("negative zero is never displayed") {
    // A rounding artefact, never a value the user meant.
    CHECK(formatLength(-0.0001, Unit::Millimetre, 2) == "0.00mm");
    CHECK(formatLength(-1.0, Unit::Millimetre, 2) == "-1.00mm");
}

TEST_CASE("suffix tokens round-trip") {
    for(Unit u : {Unit::Millimetre, Unit::Centimetre, Unit::Metre, Unit::Inch, Unit::Point}) {
        CHECK(unitFromSuffix(paroculus::unitSuffix(u)) == u);
    }
    CHECK_FALSE(unitFromSuffix("furlong").has_value());
}
