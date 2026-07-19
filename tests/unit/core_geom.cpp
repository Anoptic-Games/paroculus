#include <doctest/doctest.h>

#include "core/geom.h"

using paroculus::Point;
using paroculus::ViewTransform;

namespace {

// A representative view: half-scale, Y flipped, origin pushed to (100, 50).
ViewTransform makeView() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(100.0, 50.0));
    m.scale(Eigen::Vector2d(2.0, -2.0));
    return ViewTransform(m);
}

}  // namespace

TEST_CASE("ViewTransform maps document space to screen pixels") {
    const ViewTransform view = makeView();

    const Eigen::Vector2d origin = view.toScreen({0.0, 0.0});
    CHECK(origin.x() == doctest::Approx(100.0));
    CHECK(origin.y() == doctest::Approx(50.0));

    // Y-up document against Y-down screen: positive document Y goes up-screen.
    const Eigen::Vector2d up = view.toScreen({0.0, 10.0});
    CHECK(up.y() < origin.y());
}

TEST_CASE("ViewTransform round-trips through document space") {
    const ViewTransform view = makeView();
    const Point p{37.5, -12.25};

    const Point back = view.toDocument(view.toScreen(p));
    CHECK(back.x == doctest::Approx(p.x));
    CHECK(back.y == doctest::Approx(p.y));
}

TEST_CASE("pixel tolerances convert to document lengths") {
    const ViewTransform view = makeView();
    // The view doubles document units, so a 10px radius covers 5 document units.
    CHECK(view.toDocumentLength(10.0) == doctest::Approx(5.0));
}

TEST_CASE("a default ViewTransform is the identity") {
    const ViewTransform view;
    const Eigen::Vector2d v = view.toScreen({3.0, 4.0});
    CHECK(v.x() == doctest::Approx(3.0));
    CHECK(v.y() == doctest::Approx(4.0));
}
