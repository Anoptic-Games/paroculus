#include "interact/events.h"

namespace paroculus {

PointerEvent PointerEvent::at(PointerAction action, const Eigen::Vector2d &screen,
                              const ViewTransform &view, Button button,
                              Modifier modifiers, int clicks) {
    PointerEvent e;
    e.action = action;
    e.button = button;
    e.modifiers = modifiers;
    e.clicks = clicks;
    e.screen = screen;
    e.document = view.toDocument(screen);
    return e;
}

}  // namespace paroculus
