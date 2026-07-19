# paroculus

A parametric vector layout, design and animation suite.

Vector tooling has settled on a bezier- and line-centric model that assumes an
arts background: you place control points and tangents, and everything else is
downstream of that. paroculus inverts the priority. The things a non-arts user
actually reasons about — proportion, spacing, alignment, parallelism — are the
primitives, and the geometry is what falls out of them.

Design goals, in the order they constrain the architecture:

- Proportions, spacing and parallelism are first-class, declared and driven, not
  measured after the fact.
- A CAD-like parametric constraint system underneath, so a layout holds its
  relationships when any part of it moves.
- Curves limited initially to 3-point uniform. Bezier handles are a power tool
  for a later release, not the entry point.
- A layering system with alpha overwrite shapes, so occlusion and cut-outs are
  compositional rather than destructive.

Status: pre-alpha. The toolchain is wired end to end and verified on Linux; the
document model does not exist yet. What builds today is a proof that the solver,
the rasteriser and the UI shell link and cooperate.

## Dependencies

Everything except the constraint solver comes from the pinned nixpkgs revision
in `flake.nix`. Those pins are exact revisions, so vendoring them as submodules
would buy no reproducibility and cost hours of build time — Qt is roughly 5 GB
of source, and Skia builds through GN, which is hostile inside a Nix sandbox.

| Component | Version | Source | Consumed via |
|---|---|---|---|
| Qt 6 (Quick/QML) | 6.11.1 | nixpkgs | `find_package(Qt6)` |
| Skia | 144 | nixpkgs | pkg-config `skia` |
| Eigen | 3.4.1 | nixpkgs | `find_package(Eigen3)` |
| mimalloc | 3.3.2 | nixpkgs | pkg-config `mimalloc` |
| SolveSpace solver | v3.2 (`27b6a080`) | git submodule | `slvs.h` C API |

SolveSpace is the one genuine submodule. The nixpkgs `solvespace` package is the
GTK application and does not install `libslvs` headers, so the solver has to be
built here.

mimalloc is not an allocator preference. SolveSpace's `platformbase.cpp` uses the
heap-arena API (`mi_heap_new`, `mi_heap_destroy`, `mi_heap_zalloc`) for its
temporary arena, so the dependency is load-bearing.

### How little of SolveSpace is actually built

Upstream's `slvs-solver` target is six translation units, plus `src/slvs/lib.cpp`
for the C wrapper. The header chain below them reaches only libstdc++, Eigen and
mimalloc. `CMakeLists.txt` therefore compiles those seven files directly and
never invokes SolveSpace's top-level CMake. None of its nine `extlib` submodules
— zlib, libpng, freetype, libdxfrw, pixman, cairo, angle, and its own vendored
copies of mimalloc and Eigen — are fetched or built.

`paroculus` links the result strictly through `slvs.h`'s C API. Nothing above
that boundary includes a SolveSpace C++ header, so the solver stays at C++17
while the application is C++20, and the two sides keep independent C++ standard,
allocator and exception behaviour.

### Licence

Linking the SolveSpace solver makes the distributed binary GPL-3.0-or-later, and
`LICENSE` reflects that. This replaced the Unlicense the repository started with.
Anyone wanting a permissive paroculus would need to replace the solver — Eigen is
MPL-2.0 and a 2D-only Newton-Raphson or Levenberg-Marquardt residual solver over
these primitives is tractable, since SolveSpace's is 3D and gets constrained to a
workplane here anyway.

## Building

The flake carries the whole toolchain; no system dependencies are assumed beyond
Nix itself.

Pure builds, output in `./result`, working-tree state irrelevant:

```
nix build                # Release, this platform
nix build .#debug        # Debug
nix build .#anygpu       # Release with mesa ICDs bundled as a default
nix build .#tests        # build and run the headless selftest; failure is red
nix flake check          # the above, as a check output
```

Running:

```
nix run                  # or .#play — launch on this machine's GPU
nix run .#dev            # build the work tree and launch it
nix develop              # the same environment, driven by hand
```

`nix run .#dev` gates on the submodule gitlink matching the flake pin and
refuses to proceed if they have diverged, auto-initialises the submodule if it
is absent, writes `compile_commands.json` for clangd, then launches through the
same GPU bridge as `.#play`.

Both launchers stage store mesa ICDs additively and bridge a host NVIDIA driver
through nixglhost when `/run/opengl-driver` is missing, which is what makes the
application run on foreign distributions. `VK_ADD_DRIVER_FILES=""` opts out.

Git flakes see tracked files only, so `git add` new files or Nix will not see
them. Submodule contents are invisible to a git flake, which is why SolveSpace
arrives as a pinned flake input and is injected during `postUnpack` rather than
read from the work tree. `flake.nix` and `.gitmodules` must therefore agree on
the revision; the dev shell warns when they do not.

### Platform status

Linux is verified. macOS evaluates but has not been built here — Skia's GN build
and the Qt/MoltenVK path both need a real check before that is claimed. Windows
is deliberately absent rather than merely untested: Qt 6 and Skia both
cross-compile poorly under MinGW, and a package output that has never built is
worse than no output. `pkgsCross.ucrt64.qt6.qtbase` does evaluate, so the path
exists; it needs work.

## Layout

```
CMakeLists.txt        two targets: paroculus-solver, paroculus
flake.nix             toolchain, variants, launchers, dev shell
external/solvespace   submodule, pinned; seven files of it are compiled
src/sketch.{h,cpp}    solver and Skia raster. Links no Qt.
src/sketchview.{h,cpp}  QQuickPaintedItem. The only Qt-aware translation unit.
src/main.cpp          application entry and --selftest
src/Main.qml          the shell
```

The seam between `sketch.cpp` and `sketchview.cpp` is deliberate. The document
model and its rendering must not acquire a dependency on the UI toolkit, both so
the render backend can change without touching geometry and so the solver can be
exercised headlessly.

## Verifying

`paroculus --selftest` solves a sketch and rasterises it with no display server,
and is what `nix build .#tests` runs under an offscreen QPA platform.

It checks solved geometry against the constraints that were declared, rather
than trusting the solver's own status code. Seeds are deliberately off-constraint
— segment A starts non-horizontal, B starts non-parallel — so a solver that
no-opped and echoed its input back would report success and still fail every
assertion:

```
solver result=0 dof=0
A=(0.0000,0.0000)->(120.0000,0.0000) |A|=120.000000
B=(0.0000,-60.0000)->(74.1656,-60.0000) |B|=74.165637
  ok: |A| == 120
  ok: A horizontal
  ok: B parallel to A
  ok: len(A)/len(B) == ratio
skia: background=ff14161a painted=7907 of 120000 px
selftest OK — solvespace, eigen, skia and qt all live
```

The demo sketch holds segment B parallel to A with `len(A)/len(B)` pinned to a
ratio driven from the UI, which is the thesis of the project in miniature:
proportion and parallelism as the control surface.

The selftest does not exercise QML. That path is checked separately by launching
the application and confirming the window and its registered type appear.

## Known shortcuts

`SketchView::paint` routes Skia through `QQuickPaintedItem`, costing a CPU raster
and a texture upload per frame. That is adequate for proving the toolchain and
inadequate for a real canvas; the replacement is Skia's Ganesh backend sharing
Qt's RHI context, or a `QQuickRhiItem`. The `sketch` / `sketchview` seam exists
so that swap does not reach the document model.

The sketch itself is hardcoded in `solveDemoSketch`. There is no document, no
persistence, no layer system, and no curve support yet.
