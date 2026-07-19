// The doctest runner over every layer below shell. Stage 0 carries one case per
// layer, proving the harness compiles and links against each seam; stages 1-2
// grow this into tests/{unit,property,semantics}, and stage 3 adds the gesture
// corpus. Nothing here may construct a QGuiApplication — the shell is covered
// by `paroculus --selftest` instead.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
