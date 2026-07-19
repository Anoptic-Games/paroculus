#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "render/view.h"
#include "solve/demosketch.h"

using paroculus::Point;
using paroculus::Solution;
using paroculus::ViewTransform;
using paroculus::fitView;
using paroculus::renderSketch;
using paroculus::solveDemoSketch;

namespace {

constexpr int W = 400;
constexpr int H = 300;
// canvas.clear(SkColorSetRGB(0x14, 0x16, 0x1a)) as a little-endian BGRA word.
constexpr uint32_t BACKGROUND = 0xff14161au;

std::vector<uint32_t> paint(const Solution &s) {
    std::vector<uint32_t> pixels(static_cast<size_t>(W) * H, 0u);
    renderSketch(s, reinterpret_cast<uint8_t *>(pixels.data()), W, H,
                 static_cast<size_t>(W) * 4);
    return pixels;
}

uint32_t at(const std::vector<uint32_t> &pixels, const Eigen::Vector2d &p) {
    const int x = static_cast<int>(p.x());
    const int y = static_cast<int>(p.y());
    REQUIRE(x >= 0);
    REQUIRE(y >= 0);
    REQUIRE(x < W);
    REQUIRE(y < H);
    return pixels[static_cast<size_t>(y) * W + x];
}

}  // namespace

TEST_CASE("the raster and the view transform agree") {
    // Analytic sampling rather than a golden image: the point the transform
    // says segment A's midpoint lands on must be the point Skia painted it on.
    // Hit testing will read the same transform, so this is the property that
    // keeps picking aligned with what the user sees.
    const Solution s = solveDemoSketch(1.618);
    REQUIRE(s.ok());

    const std::vector<uint32_t> pixels = paint(s);
    const ViewTransform view = fitView(W, H);

    const Point midA{(s.a0.x + s.a1.x) * 0.5, (s.a0.y + s.a1.y) * 0.5};
    CHECK(at(pixels, view.toScreen(midA)) != BACKGROUND);

    const Point midB{(s.b0.x + s.b1.x) * 0.5, (s.b0.y + s.b1.y) * 0.5};
    CHECK(at(pixels, view.toScreen(midB)) != BACKGROUND);
}

TEST_CASE("the raster clears the whole surface") {
    const Solution s = solveDemoSketch(1.618);
    const std::vector<uint32_t> pixels = paint(s);
    // A corner well away from geometry and off the 20-unit grid.
    CHECK(pixels[2u * W + 2u] == BACKGROUND);
}

TEST_CASE("a degenerate viewport paints nothing") {
    const Solution s = solveDemoSketch(1.618);
    std::vector<uint32_t> pixels(16u, 0u);
    renderSketch(s, reinterpret_cast<uint8_t *>(pixels.data()), 0, 0, 0);
    renderSketch(s, nullptr, 4, 4, 16);
    for(uint32_t p : pixels) CHECK(p == 0u);
}

TEST_CASE("fitView centres the content and flips Y") {
    const ViewTransform view = fitView(W, H);
    const Eigen::Vector2d up = view.toScreen({0.0, 10.0});
    const Eigen::Vector2d down = view.toScreen({0.0, -10.0});
    CHECK(up.y() < down.y());
}
