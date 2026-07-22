cmake_minimum_required(VERSION 3.21)
project(paroculus VERSION 0.0.1 LANGUAGES CXX)

# Qt shell is C++20; the vendored solver stays at its own standard (below).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(PAROCULUS_TESTS "Build the doctest runner and register tests with CTest" OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml)
find_package(Eigen3 3.4 REQUIRED NO_MODULE)
pkg_check_modules(SKIA REQUIRED IMPORTED_TARGET skia)
pkg_check_modules(MIMALLOC REQUIRED IMPORTED_TARGET mimalloc)

qt_standard_project_setup(REQUIRES 6.5)

#-----------------------------------------------------------------------------
# paroculus-solver — SolveSpace's geometric constraint solver, nothing else.
#
# Upstream's `slvs-solver` INTERFACE target is these six translation units plus
# src/slvs/lib.cpp for the C wrapper; the header chain below them reaches only
# libstdc++, Eigen, and mimalloc. Compiling them directly sidesteps SolveSpace's
# top-level CMake and all nine of its extlib submodules (cairo, gtkmm, freetype,
# libpng, zlib, angle, pixman, libdxfrw), none of which the solver touches.
#
# LIBRARY: stubs out the GUI message-box path in util.cpp.
# STATIC_LIB: neutralises slvs.h's __declspec on Windows. No-op elsewhere.
# Invariant: paroculus links this through slvs.h's C API only. Nothing above
# this target includes a SolveSpace C++ header, so the C++ standard, allocator,
# and exception behaviour on either side stay independent.
#-----------------------------------------------------------------------------
set(SLVS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/solvespace")
if(NOT EXISTS "${SLVS_DIR}/src/slvs/lib.cpp")
    message(FATAL_ERROR
        "external/solvespace is empty — run 'git submodule update --init external/solvespace'")
endif()

add_library(paroculus-solver STATIC
    "${SLVS_DIR}/src/constrainteq.cpp"
    "${SLVS_DIR}/src/entity.cpp"
    "${SLVS_DIR}/src/expr.cpp"
    "${SLVS_DIR}/src/platform/platformbase.cpp"
    "${SLVS_DIR}/src/system.cpp"
    "${SLVS_DIR}/src/util.cpp"
    "${SLVS_DIR}/src/slvs/lib.cpp"
)
target_compile_definitions(paroculus-solver PRIVATE LIBRARY STATIC_LIB)
target_include_directories(paroculus-solver
    PRIVATE "${SLVS_DIR}/src"
    PUBLIC  "${SLVS_DIR}/include")
target_link_libraries(paroculus-solver PRIVATE Eigen3::Eigen PkgConfig::MIMALLOC)
set_target_properties(paroculus-solver PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)
# Upstream is C++11-era and warns freely; its noise is not ours to fix.
target_compile_options(paroculus-solver PRIVATE -w)

#-----------------------------------------------------------------------------
# The layer targets. Five seams, each crossable downward only:
#
#   shell + app   Qt: SketchView, input translation, QML, entry point
#   interact      selection, hit, snap, tools, registry, policies
#   render        tessellation, compositor, adorners            [Skia]
#   solve         slvs translation, contexts, seeds, diagnosis  [slvs.h]
#   core          ids, taxonomy, slots, units, document, geom   [Eigen]
#
# The boundaries are link boundaries, not conventions: Skia reaches only
# paroculus-render and slvs.h only paroculus-solve, both PRIVATE, so a forbidden
# include from any other layer is a build error. tests/boundary/ exercises that
# mechanism directly.
#-----------------------------------------------------------------------------

# core — the bottom of the stack. Everything above reaches it; it reaches Eigen.
# The PUBLIC src include directory is what gives every layer its "core/geom.h"
# style include, so it is declared once here and inherited by linkage.
add_library(paroculus-core STATIC
    src/core/bake.cpp
    src/core/bake.h
    src/core/composition.cpp
    src/core/composition.h
    src/core/compound.cpp
    src/core/compound.h
    src/core/copy.cpp
    src/core/copy.h
    src/core/document.cpp
    src/core/document.h
    src/core/geom.h
    src/core/glyphs.cpp
    src/core/glyphs.h
    src/core/ids.h
    src/core/measure.cpp
    src/core/measure.h
    src/core/parameters.cpp
    src/core/parameters.h
    src/core/persist.cpp
    src/core/pose.cpp
    src/core/pose.h
    src/core/persist.h
    src/core/records.cpp
    src/core/records.h
    src/core/slots.cpp
    src/core/slots.h
    src/core/solution.cpp
    src/core/solution.h
    src/core/svg.cpp
    src/core/svg.h
    src/core/table.h
    src/core/tags.cpp
    src/core/tags.h
    src/core/taxonomy.cpp
    src/core/taxonomy.h
    src/core/topology.cpp
    src/core/topology.h
    src/core/transform.cpp
    src/core/transform.h
    src/core/undo.cpp
    src/core/undo.h
    src/core/units.cpp
    src/core/units.h
    src/core/usage.cpp
    src/core/usage.h
)
target_include_directories(paroculus-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(paroculus-core PUBLIC Eigen3::Eigen)

# solve — the only target that may see slvs.h. PRIVATE linkage keeps the
# solver's include directory off every consumer's command line while still
# carrying its objects through to the final link.
add_library(paroculus-solve STATIC
    src/solve/arena.cpp
    src/solve/arena.h
    src/solve/context.cpp
    src/solve/context.h
    src/solve/demosketch.cpp
    src/solve/demosketch.h
    src/solve/diagnose.cpp
    src/solve/diagnose.h
    src/solve/scheduler.cpp
    src/solve/scheduler.h
    src/solve/solve.cpp
    src/solve/solve.h
    src/solve/spsc.h
)
# mimalloc is PRIVATE for the same reason the solver is: every solve context
# owns a heap, and no layer above needs to know what it is made of. Threads is
# PUBLIC: the async scheduler's workers are std::threads, and a consumer that
# links this static library has to carry pthread through to the final link.
target_link_libraries(paroculus-solve
    PUBLIC paroculus-core Threads::Threads
    PRIVATE paroculus-solver PkgConfig::MIMALLOC)

# The bundled typeface, compiled in rather than looked up.
#
# A pinned build input, embedded as bytes at configure time. Dimension text has
# to render identically in the sandbox, in a raster assertion and on a user's
# machine, and a font resolved through fontconfig at runtime makes all three
# answer differently. Refused outright when absent: an application whose
# dimensions are silently blank is worse than one that will not configure.
set(PAROCULUS_FONT "$ENV{PAROCULUS_FONT}" CACHE FILEPATH
    "TTF embedded as the bundled typeface for dimension text")
if(NOT PAROCULUS_FONT OR NOT EXISTS "${PAROCULUS_FONT}")
    message(FATAL_ERROR
        "PAROCULUS_FONT is unset or missing (${PAROCULUS_FONT}).\n"
        "The nix build and dev shell both supply it; a hand-driven cmake needs "
        "-DPAROCULUS_FONT=/path/to/a.ttf")
endif()
set(PAROCULUS_TYPEFACE_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/typeface.cpp")
# At configure time, not build time: the font is a fixed input, so regenerating
# it per build would be work with no possible new answer.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            "-DFONT=${PAROCULUS_FONT}" "-DOUT=${PAROCULUS_TYPEFACE_CPP}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_font.cmake"
    RESULT_VARIABLE embed_result)
if(NOT embed_result EQUAL 0)
    message(FATAL_ERROR "could not embed ${PAROCULUS_FONT}")
endif()

# render — the only target that may see Skia. Its headers speak in raw buffers,
# so the QQuickPaintedItem shortcut can become the Ganesh/RHI path (track R)
# without anything above this layer noticing.
add_library(paroculus-render STATIC
    src/render/view.cpp
    src/render/view.h
    src/render/typeface.h
    "${PAROCULUS_TYPEFACE_CPP}"
)
target_link_libraries(paroculus-render PUBLIC paroculus-core PRIVATE PkgConfig::SKIA)

# interact — deliberately toolkit-free and raster-free: it consumes abstract
# input events and emits document commands, which is what makes gesture scripts
# runnable headlessly in CI.
add_library(paroculus-interact STATIC
    src/interact/drag.cpp
    src/interact/drag.h
    src/interact/events.cpp
    src/interact/events.h
    src/interact/glyphs.cpp
    src/interact/glyphs.h
    src/interact/hit.cpp
    src/interact/impose.cpp
    src/interact/impose.h
    src/interact/hit.h
    src/interact/loops.cpp
    src/interact/loops.h
    src/interact/numeric.cpp
    src/interact/numeric.h
    src/interact/policies.cpp
    src/interact/placement.h
    src/interact/policies.h
    src/interact/registry.cpp
    src/interact/registry.h
    src/interact/script.cpp
    src/interact/script.h
    src/interact/selection.cpp
    src/interact/selection.h
    src/interact/snap.cpp
    src/interact/snap.h
    src/interact/session.cpp
    src/interact/surface.cpp
    src/interact/surface.h
    src/interact/session.h
    src/interact/tools.cpp
    src/interact/tools.h
)
target_link_libraries(paroculus-interact PUBLIC paroculus-core paroculus-solve)

#-----------------------------------------------------------------------------
# paroculus — the Qt shell and entry point, the only Qt-aware compilation.
#
# The non-entry-point shell sources are shared between the app and its offscreen
# test runner, so both compile the same Workspace, models and settings the QML
# projects. Listed once here; sketchview.cpp stays listed per target because the
# QML_ELEMENT registration is the app target's alone.
#-----------------------------------------------------------------------------
set(PAROCULUS_SHELL_SOURCES
    src/shell/sidecar.cpp
    src/shell/sidecar.h
    src/shell/settings.cpp
    src/shell/settings.h
    src/shell/theme.cpp
    src/shell/theme.h
    src/shell/models/reports.cpp
    src/shell/models/reports.h
    src/shell/workspace.cpp
    src/shell/workspace.h
    src/shell/workspaces.cpp
    src/shell/workspaces.h
)

qt_add_executable(paroculus
    src/app/main.cpp
    src/app/scriptplay.cpp
    src/app/scriptplay.h
    src/app/selftest.cpp
    src/app/selftest.h
    src/shell/sketchview.cpp
    src/shell/sketchview.h
    ${PAROCULUS_SHELL_SOURCES}
)
target_link_libraries(paroculus PRIVATE
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml
    paroculus-interact
    paroculus-render
    # Stage-0 residue: SketchView solves directly because the interact layer
    # that will own that dispatch arrives in stage 3. Drop this line then.
    paroculus-solve
)
# qt_add_qml_module generates a registration TU that includes QML_ELEMENT
# headers by basename (<sketchview.h>), so the shell directory has to be
# reachable directly. Shell sources still use the layer-qualified form.
target_include_directories(paroculus PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/shell")

# Without the alias the QML file would resolve at qrc:/qt/qml/Paroculus/src/
# shell/qml/Main.qml, which loadFromModule("Paroculus", "Main") will not find.
set_source_files_properties(src/shell/qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
qt_add_qml_module(paroculus
    URI Paroculus
    VERSION 1.0
    QML_FILES
        src/shell/qml/Main.qml
        src/shell/qml/Panel.qml
        src/shell/qml/ProjectedMenu.qml
        src/shell/qml/LeftDock.qml
        src/shell/qml/RightDock.qml
        src/shell/qml/StripView.qml
        src/shell/qml/CommandPalette.qml
        src/shell/qml/Toast.qml
        src/shell/qml/StatusBarView.qml
)

install(TARGETS paroculus BUNDLE DESTINATION . RUNTIME DESTINATION bin)

#-----------------------------------------------------------------------------
# Tests. paroculus-tests is the doctest runner over every layer below shell;
# the shell is covered by --selftest, which needs a QGuiApplication.
#-----------------------------------------------------------------------------
if(PAROCULUS_TESTS)
    enable_testing()
    find_package(doctest REQUIRED)

    add_executable(paroculus-tests
        tests/main.cpp
        tests/support/build.cpp
        tests/support/build.h
        tests/unit/core_composition.cpp
        tests/unit/core_document.cpp
        tests/unit/core_geom.cpp
        tests/unit/core_ids.cpp
        tests/unit/core_parameters.cpp
        tests/unit/core_persist.cpp
        tests/unit/core_slots.cpp
        tests/unit/core_solution.cpp
        tests/unit/core_svg.cpp
        tests/unit/core_table.cpp
        tests/unit/core_taxonomy.cpp
        tests/unit/core_topology.cpp
        tests/unit/core_undo.cpp
        tests/unit/core_units.cpp
        tests/unit/interact_events.cpp
        tests/unit/render_view.cpp
        tests/unit/core_pose.cpp
        tests/unit/interact_hit.cpp
        tests/unit/interact_selection.cpp
        tests/unit/interact_script.cpp
        tests/unit/interact_tools.cpp
        tests/unit/interact_snap.cpp
        tests/unit/interact_glyphs.cpp
        tests/unit/interact_shapes.cpp
        tests/unit/interact_numeric.cpp
        tests/unit/interact_loops.cpp
        tests/unit/interact_registry.cpp
        tests/unit/interact_impose.cpp
        tests/unit/interact_composition.cpp
        tests/unit/interact_regions.cpp
        tests/raster/composition.cpp
        tests/raster/regions.cpp
        tests/raster/dimensions.cpp
        tests/gestures/async.cpp
        tests/gestures/corpus.cpp
        tests/gestures/composition.cpp
        tests/gestures/flagship.cpp
        tests/gestures/inference.cpp
        tests/gestures/structure.cpp
        tests/unit/solve_context.cpp
        tests/unit/solve_demosketch.cpp
        tests/unit/solve_scheduler.cpp
        tests/semantics/applicability.cpp
        tests/semantics/catalogue.cpp
        tests/semantics/branches.cpp
        tests/semantics/structure.cpp
    )
    target_link_libraries(paroculus-tests PRIVATE
        doctest::doctest
        paroculus-core
        paroculus-interact
        paroculus-render
        paroculus-solve
    )
    # Test-only: fixtures include "support/build.h" the same way sources include
    # their layer-qualified headers.
    target_include_directories(paroculus-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    # Corpus scripts are checked-in artefacts, not build outputs: a test that
    # asserts one has to find it in the source tree whatever directory CTest
    # runs from. Passed as a definition rather than resolved from the working
    # directory, because the working directory is the one thing a sandbox is
    # free to change.
    target_compile_definitions(paroculus-tests PRIVATE
        PAROCULUS_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus")
    add_test(NAME unit COMMAND paroculus-tests)

    # The shell's QEvent-to-interact translation, on its own Qt-linked runner.
    # paroculus-tests links no Qt by design — that headlessness is what makes the
    # gesture corpus runnable — so the translation seam in sketchview.cpp went
    # untested (REVIEW.md finding 12). This target compiles that seam and pins
    # the pure helpers exposed for it in sketchview.h. scriptplay.cpp comes along
    # because sketchview.cpp's constructor references pendingScript; it is
    # otherwise an app source rather than a library, so it is not on any layer
    # target to link. Qt components match the paroculus target's for the same
    # reason: compiling the whole TU pulls QQuickPaintedItem in.
    qt_add_executable(paroculus-shell-tests
        tests/shell/translation.cpp
        tests/shell/sidecar.cpp
        tests/shell/workspace.cpp
        src/shell/sketchview.cpp
        src/shell/sketchview.h
        src/app/scriptplay.cpp
        src/app/scriptplay.h
        ${PAROCULUS_SHELL_SOURCES}
    )
    target_link_libraries(paroculus-shell-tests PRIVATE
        doctest::doctest
        Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml
        paroculus-interact
        paroculus-render
        paroculus-solve
    )
    # The layer-qualified includes ("shell/...", "core/...") and the QML_ELEMENT
    # header lookup want both the src root and the shell directory reachable
    # directly, exactly as the paroculus target arranges.
    target_include_directories(paroculus-shell-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/shell")
    # Offscreen QPA for the same reason the selftest needs it: QKeyEvent's device
    # default and QGuiApplication both resolve through a platform plugin, and the
    # sandbox has no display server. The checkPhase already exports QT_PLUGIN_PATH.
    add_test(NAME shell-translation COMMAND paroculus-shell-tests)
    set_tests_properties(shell-translation PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Offscreen QPA: the selftest constructs a QGuiApplication, so it needs a
    # platform plugin, but the sandbox has no display server.
    add_test(NAME selftest COMMAND paroculus --selftest)
    set_tests_properties(selftest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # The timed benchmarks are run on demand, not by CTest: they are wall-clock
    # numbers on a shared machine, and a timing gate in the sandbox would be a
    # flake generator rather than a signal:  ./build/Debug/paroculus-bench
    #
    # The arena-bytes half is deterministic — a function of the document, not the
    # clock — so its --check-arena mode is registered below. The baseline path is
    # compiled in from the source tree rather than resolved from the working
    # directory, which is the one thing a sandbox is free to change.
    add_executable(paroculus-bench tests/bench/main.cpp)
    target_link_libraries(paroculus-bench PRIVATE paroculus-solve)
    target_compile_definitions(paroculus-bench PRIVATE
        PAROCULUS_BENCH_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/tests/bench/baseline.txt")
    add_test(NAME bench-arena COMMAND paroculus-bench --check-arena)

    # CLI dispatch. The headless entry points and the argument guards. --export
    # and --import run before any QGuiApplication, and the mode-conflict and
    # bad-script guards return before it too, so none of these needs a display —
    # they run under the same offscreen QPA the selftest does, for the same
    # reason and in case a path ever grows a Qt dependency.
    add_test(NAME cli-export
             COMMAND paroculus --export "${CMAKE_CURRENT_BINARY_DIR}/cli-export.svg")
    add_test(NAME cli-import
             COMMAND paroculus --import "${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/import.svg")
    add_test(NAME cli-conflicting-flags
             COMMAND paroculus --selftest --export "${CMAKE_CURRENT_BINARY_DIR}/cli-unused.svg")
    add_test(NAME cli-bad-script
             COMMAND paroculus --script "${CMAKE_CURRENT_BINARY_DIR}/no-such-file.paro")
    set_tests_properties(cli-export cli-import cli-conflicting-flags cli-bad-script
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(cli-conflicting-flags cli-bad-script PROPERTIES WILL_FAIL TRUE)

    # Link-boundary canaries. Each is a translation unit that links one layer
    # target and then reaches for a header belonging to a layer that target must
    # not expose. EXCLUDE_FROM_ALL and compiled by CTest, which expects them to
    # fail — that expectation is the whole test. If a canary ever compiles, a
    # seam has been widened into a convention.
    #
    #   slvs / skia on core  — core sees neither the solver nor Skia.
    #   solve_consumer       — a consumer of paroculus-solve still cannot see
    #                          slvs.h; catches paroculus-solver flipping to PUBLIC
    #                          and leaking the solver upward into interact/shell.
    #   render_consumer      — a consumer of paroculus-render still cannot see
    #                          Skia; catches the Skia link flipping to PUBLIC.
    #   interact_qt          — the interact layer stays toolkit-free; catches a
    #                          Qt dependency reaching it, which would make gesture
    #                          scripts un-runnable headlessly.
    #
    # Each links only its layer target and NOT the forbidden library, so the
    # include resolves exactly when the seam widens — never because the canary
    # dragged the header in itself.
    function(paroculus_canary name)
        add_executable(paroculus-canary-${name} EXCLUDE_FROM_ALL
            tests/boundary/canary_${name}.cpp)
        target_link_libraries(paroculus-canary-${name} PRIVATE ${ARGN})
        add_test(NAME boundary-${name}
                 COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                         --target paroculus-canary-${name})
        set_tests_properties(boundary-${name} PROPERTIES WILL_FAIL TRUE RUN_SERIAL TRUE)
    endfunction()

    paroculus_canary(slvs paroculus-core)
    paroculus_canary(skia paroculus-core)
    paroculus_canary(solve_consumer paroculus-solve)
    paroculus_canary(render_consumer paroculus-render)
    paroculus_canary(interact_qt paroculus-interact)
endif()
