#include <doctest/doctest.h>

#include "interact/events.h"

using paroculus::Button;
using paroculus::Modifier;
using paroculus::PointerAction;
using paroculus::PointerEvent;
using paroculus::ViewTransform;
using paroculus::has;

namespace {

ViewTransform makeView() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(100.0, 50.0));
    m.scale(Eigen::Vector2d(2.0, -2.0));
    return ViewTransform(m);
}

}  // namespace

TEST_CASE("modifier sets compose and test") {
    const Modifier m = Modifier::Shift | Modifier::Alt;
    CHECK(has(m, Modifier::Shift));
    CHECK(has(m, Modifier::Alt));
    CHECK_FALSE(has(m, Modifier::Control));
    CHECK(has(m, Modifier::Shift | Modifier::Alt));
    CHECK_FALSE(has(m, Modifier::Shift | Modifier::Control));
    // The empty modifier is present in every set, including the empty one.
    CHECK(has(Modifier::None, Modifier::None));
    CHECK(has(m, Modifier::None));
}

TEST_CASE("a pointer event carries both spaces consistently") {
    // Screen and document can never disagree, because only one constructor
    // fills them. Pixel-space tolerances and document-space geometry stay in
    // their own regimes with a single named conversion between.
    const ViewTransform view = makeView();
    const Eigen::Vector2d screen(140.0, 30.0);

    const PointerEvent e =
        PointerEvent::at(PointerAction::Press, screen, view, Button::Left, Modifier::Shift);

    CHECK(e.action == PointerAction::Press);
    CHECK(e.button == Button::Left);
    CHECK(has(e.modifiers, Modifier::Shift));
    CHECK(e.screen == screen);
    CHECK(e.document.x == doctest::Approx(20.0));
    CHECK(e.document.y == doctest::Approx(10.0));

    const Eigen::Vector2d back = view.toScreen(e.document);
    CHECK(back.x() == doctest::Approx(e.screen.x()));
    CHECK(back.y() == doctest::Approx(e.screen.y()));
}

TEST_CASE("a default pointer event is an unmodified move") {
    const PointerEvent e;
    CHECK(e.action == PointerAction::Move);
    CHECK(e.button == Button::None);
    CHECK(e.modifiers == Modifier::None);
}
