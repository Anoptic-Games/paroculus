#pragma once

namespace paroculus {

// Solves the demo sketch, checks the solved geometry against the constraints
// that were declared, rasterises it, and reports on stdout.
// Returns 0 on success, 1 on the first failed check.
// Requires a live QGuiApplication (it builds a QImage) but no display server.
int selftest();

}  // namespace paroculus
