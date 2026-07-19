// Builders shared by the core test suites.
//
// These construct documents through commands, never by reaching into tables,
// so a fixture can never set up a state the command layer would have refused.
#pragma once

#include <string>
#include <vector>

#include "core/document.h"

namespace paroculus::test {

// A deterministic generator. Seeded explicitly and never reading a clock, so
// a property failure reproduces from its seed on any machine.
class Rng {
public:
    explicit Rng(uint64_t seed) : state_(seed | 1u) {}

    uint32_t next() {
        // xorshift64*, chosen for being short and reproducible rather than
        // statistically excellent; property coverage does not need more.
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return static_cast<uint32_t>((state_ * 0x2545F4914F6CDD1DULL) >> 32);
    }

    // Returns a value in [0, bound). Returns 0 when bound is 0.
    uint32_t below(uint32_t bound) { return bound == 0 ? 0 : next() % bound; }
    bool chance(uint32_t oneIn) { return below(oneIn) == 0; }
    double real(double lo, double hi) {
        return lo + (hi - lo) * (static_cast<double>(next()) / 4294967295.0);
    }

private:
    uint64_t state_;
};

// Adds a free point at (x, y) and returns its ID, or a null ID on refusal.
EntityId addPoint(Document &doc, double x, double y);

// Adds a segment between two existing points.
EntityId addSegment(Document &doc, EntityId a, EntityId b);

// Adds a constraint with the given operands and value.
ConstraintId addConstraint(Document &doc, ConstraintKind kind,
                           std::vector<EntityId> operands, Slot value = Slot());

}  // namespace paroculus::test
