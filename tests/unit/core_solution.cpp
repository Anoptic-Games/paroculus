#include <doctest/doctest.h>

#include <string>

#include "core/solution.h"

using paroculus::SolveStatus;
using paroculus::Solution;
using paroculus::statusName;

TEST_CASE("a default Solution is unsolved") {
    const Solution s;
    CHECK(s.status == SolveStatus::Unsolved);
    CHECK(s.dof == -1);
    CHECK_FALSE(s.ok());
}

TEST_CASE("redundant-but-consistent is a solved state") {
    // Redundancy is caught at creation time, not by rejecting solved geometry.
    Solution s;
    s.status = SolveStatus::RedundantOkay;
    CHECK(s.ok());
}

TEST_CASE("failed solves are not ok") {
    for(SolveStatus bad : {SolveStatus::Inconsistent, SolveStatus::DidNotConverge,
                           SolveStatus::TooManyUnknowns, SolveStatus::Unsolved}) {
        Solution s;
        s.status = bad;
        CHECK_FALSE(s.ok());
    }
}

TEST_CASE("every status has a stable label") {
    CHECK(std::string(statusName(SolveStatus::Okay)) == "okay");
    CHECK(std::string(statusName(SolveStatus::Inconsistent)) == "inconsistent");
    CHECK(std::string(statusName(SolveStatus::DidNotConverge)) == "did not converge");
    CHECK(std::string(statusName(SolveStatus::TooManyUnknowns)) == "too many unknowns");
    CHECK(std::string(statusName(SolveStatus::RedundantOkay)) == "redundant but okay");
    CHECK(std::string(statusName(SolveStatus::Unsolved)) == "unsolved");
    // Out-of-range codes must not fall off the end of the table.
    CHECK(std::string(statusName(static_cast<SolveStatus>(99))) == "unsolved");
}
