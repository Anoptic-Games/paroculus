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
    src/core/document.cpp
    src/core/document.h
    src/core/geom.h
    src/core/glyphs.h
    src/core/ids.h
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
    src/core/table.h
    src/core/taxonomy.cpp
    src/core/taxonomy.h
    src/core/topology.cpp
    src/core/topology.h
    src/core/undo.cpp
    src/core/undo.h
    src/core/units.cpp
    src/core/units.h
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
    src/solve/solve.cpp
    src/solve/solve.h
)
# mimalloc is PRIVATE for the same reason the solver is: every solve context
# owns a heap, and no layer above needs to know what it is made of.
target_link_libraries(paroculus-solve
    PUBLIC paroculus-core
    PRIVATE paroculus-solver PkgConfig::MIMALLOC)

# render — the only target that may see Skia. Its headers speak in raw buffers,
# so the QQuickPaintedItem shortcut can become the Ganesh/RHI path (track R)
# without anything above this layer noticing.
add_library(paroculus-render STATIC
    src/render/view.cpp
    src/render/view.h
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
    src/interact/session.h
    src/interact/tools.cpp
    src/interact/tools.h
)
target_link_libraries(paroculus-interact PUBLIC paroculus-core paroculus-solve)

#-----------------------------------------------------------------------------
# paroculus — the Qt shell and entry point, the only Qt-aware compilation.
#-----------------------------------------------------------------------------
qt_add_executable(paroculus
    src/app/main.cpp
    src/app/scriptplay.cpp
    src/app/scriptplay.h
    src/app/selftest.cpp
    src/app/selftest.h
    src/shell/sketchview.cpp
    src/shell/sketchview.h
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
    QML_FILES src/shell/qml/Main.qml
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
        tests/unit/core_document.cpp
        tests/unit/core_geom.cpp
        tests/unit/core_ids.cpp
        tests/unit/core_parameters.cpp
        tests/unit/core_persist.cpp
        tests/unit/core_slots.cpp
        tests/unit/core_solution.cpp
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
        tests/gestures/corpus.cpp
        tests/gestures/inference.cpp
        tests/unit/solve_context.cpp
        tests/unit/solve_demosketch.cpp
        tests/semantics/catalogue.cpp
        tests/semantics/branches.cpp
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
    add_test(NAME unit COMMAND paroculus-tests)

    # Offscreen QPA: the selftest constructs a QGuiApplication, so it needs a
    # platform plugin, but the sandbox has no display server.
    add_test(NAME selftest COMMAND paroculus --selftest)
    set_tests_properties(selftest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Benchmarks are run on demand, not by CTest: they are wall-clock numbers on
    # a shared machine, and a timing gate in the sandbox would be a flake
    # generator rather than a signal. Run from the work tree root so the
    # baseline path resolves:  ./build/Debug/paroculus-bench
    add_executable(paroculus-bench tests/bench/main.cpp)
    target_link_libraries(paroculus-bench PRIVATE paroculus-solve)

    # Link-boundary canaries. Each is a translation unit that links only
    # paroculus-core and then reaches for a header belonging to a higher layer.
    # They are excluded from the default build and compiled by CTest, which
    # expects them to fail — that expectation is the whole test. If a canary
    # ever compiles, a seam has been widened into a convention.
    foreach(canary slvs skia)
        add_executable(paroculus-canary-${canary} EXCLUDE_FROM_ALL
            tests/boundary/canary_${canary}.cpp)
        target_link_libraries(paroculus-canary-${canary} PRIVATE paroculus-core)
        add_test(NAME boundary-${canary}
                 COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                         --target paroculus-canary-${canary})
        set_tests_properties(boundary-${canary} PROPERTIES WILL_FAIL TRUE RUN_SERIAL TRUE)
    endforeach()
endif()
