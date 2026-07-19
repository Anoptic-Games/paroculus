cmake_minimum_required(VERSION 3.21)
project(paroculus VERSION 0.0.1 LANGUAGES CXX)

# Qt shell is C++20; the vendored solver stays at its own standard (below).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(PAROCULUS_TESTS "Register the headless selftest with CTest" OFF)

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
# paroculus — Qt Quick shell over a Qt-free sketch/render core.
#
# sketch.cpp holds the solver and Skia calls and links no Qt; sketchview.cpp is
# the only file that knows about Qt. Keeping that seam now means the document
# model never grows a dependency on the UI toolkit.
#-----------------------------------------------------------------------------
qt_add_executable(paroculus
    src/main.cpp
    src/sketch.cpp
    src/sketch.h
    src/sketchview.cpp
    src/sketchview.h
)
target_link_libraries(paroculus PRIVATE
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml
    paroculus-solver
    PkgConfig::SKIA
    Eigen3::Eigen
)
target_include_directories(paroculus PRIVATE src)

qt_add_qml_module(paroculus
    URI Paroculus
    VERSION 1.0
    QML_FILES src/Main.qml
)

install(TARGETS paroculus BUNDLE DESTINATION . RUNTIME DESTINATION bin)

if(PAROCULUS_TESTS)
    enable_testing()
    # Offscreen QPA: the selftest constructs a QGuiApplication, so it needs a
    # platform plugin, but the sandbox has no display server.
    add_test(NAME selftest COMMAND paroculus --selftest)
    set_tests_properties(selftest PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
