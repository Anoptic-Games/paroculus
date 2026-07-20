#include "core/glyphs.h"

#include <cmath>

namespace paroculus {

std::vector<Eigen::Vector2d> layOutGlyphs(std::span<const GlyphMark> marks,
                                          const ViewTransform &view,
                                          const GlyphLayout &layout) {
    std::vector<Eigen::Vector2d> out;
    out.reserve(marks.size());

    // How many marks have already landed on each anchor. A linear scan rather
    // than a map: the list is a per-frame budget in the low hundreds, and a
    // hash map here would make placement depend on iteration order, which
    // determinism forbids.
    std::vector<std::pair<Eigen::Vector2d, int>> occupied;

    for(const GlyphMark &mark : marks) {
        const Eigen::Vector2d base = view.toScreen(mark.anchor);

        int index = 0;
        bool seen = false;
        for(auto &[at, count] : occupied) {
            // Half a pixel: two marks this close are on the same joint as far
            // as the eye and the cursor are concerned, whatever the last bits
            // of the solve say.
            if((at - base).norm() < 0.5) {
                index = ++count;
                seen = true;
                break;
            }
        }
        if(!seen) occupied.emplace_back(base, 0);

        const double angle = layout.fanStart + static_cast<double>(index) * layout.fanStep;
        out.push_back(base + layout.offset * Eigen::Vector2d(std::cos(angle), std::sin(angle)));
    }
    return out;
}

}  // namespace paroculus
