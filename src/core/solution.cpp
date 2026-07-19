#include "core/solution.h"

namespace paroculus {

const char *statusName(SolveStatus s) {
    switch(s) {
        case SolveStatus::Okay:            return "okay";
        case SolveStatus::Inconsistent:    return "inconsistent";
        case SolveStatus::DidNotConverge:  return "did not converge";
        case SolveStatus::TooManyUnknowns: return "too many unknowns";
        case SolveStatus::RedundantOkay:   return "redundant but okay";
        case SolveStatus::Unsolved:        break;
    }
    return "unsolved";
}

}  // namespace paroculus
