// What a placement creates, in the vocabulary inference binds to.
//
// A snap candidate names its subject as a role — the placed point, the placed
// segment — because it is generated before the placement has any ids. This is
// the other half of that: which entities, or for a preview merely which kinds
// of entity, the committing click will actually produce.
//
// It lives in its own header because four things must agree on it and none of
// them may own it: tools fill it in, snap resolves candidates against it,
// glyphs asks it what a ghost would promise, and session carries it between
// them. A tool that forgot a role would silently drop the relations bound to
// it, which is precisely the class of bug this vocabulary exists to make
// impossible to write by omission.
#pragma once

#include "core/ids.h"

namespace paroculus {

// Which roles a placement fills, without naming entities. What a tool can say
// before its ids exist, and therefore what a preview can promise.
struct PlacementRoles {
    bool point = false;    // a point lands at the placement position
    bool segment = false;  // a segment is completed by this click
    bool curve = false;    // a circle or an arc is completed by this click

    friend bool operator==(const PlacementRoles &a, const PlacementRoles &b) {
        return a.point == b.point && a.segment == b.segment && a.curve == b.curve;
    }
    friend bool operator!=(const PlacementRoles &a, const PlacementRoles &b) {
        return !(a == b);
    }
};

// The same roles, once the entities filling them have ids. A role the placement
// did not fill stays null, and a candidate bound to it declares nothing.
struct PlacementSubjects {
    EntityId point;
    EntityId segment;
    EntityId curve;

    PlacementRoles roles() const {
        return PlacementRoles{point.valid(), segment.valid(), curve.valid()};
    }
};

}  // namespace paroculus
