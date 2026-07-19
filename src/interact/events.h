// The interact layer's input boundary. Nothing here knows about Qt: the layer
// consumes abstract events and emits document commands, which is what makes
// gesture scripts runnable headlessly in CI — the property the whole
// feel-invariant strategy depends on. The shell translates QEvents into these.
//
// Events carry their position in both spaces because the two regimes are kept
// apart deliberately: hit radii, snap radii and drag thresholds are pixel
// quantities, while everything the document stores is absolute. Converting once
// at the boundary is what stops the two leaking into each other.
#pragma once

#include "core/geom.h"

namespace paroculus {

enum class Button { None, Left, Middle, Right };

enum class Modifier : unsigned {
    None = 0,
    Shift = 1u << 0,
    Control = 1u << 1,
    Alt = 1u << 2,
};

constexpr Modifier operator|(Modifier a, Modifier b) {
    return static_cast<Modifier>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
constexpr Modifier &operator|=(Modifier &a, Modifier b) { return a = a | b; }

// m: a modifier set. q: the modifier to test. Returns true when q is present.
// Modifier::None is present in every set, including the empty one.
constexpr bool has(Modifier m, Modifier q) {
    return (static_cast<unsigned>(m) & static_cast<unsigned>(q)) == static_cast<unsigned>(q);
}

enum class PointerAction { Move, Press, Release };

struct PointerEvent {
    PointerAction action = PointerAction::Move;
    Button button = Button::None;
    Modifier modifiers = Modifier::None;
    Eigen::Vector2d screen = Eigen::Vector2d::Zero();  // pixels
    Point document;                                    // document units

    // Fills both spaces from a screen position, so the two can never disagree.
    // screen: viewport pixels. view: the transform in force at event time.
    static PointerEvent at(PointerAction action, const Eigen::Vector2d &screen,
                           const ViewTransform &view, Button button = Button::None,
                           Modifier modifiers = Modifier::None);
};

}  // namespace paroculus
