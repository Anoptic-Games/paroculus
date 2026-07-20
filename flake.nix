{
  # Pure side — artifacts in ./result, every dep pinned, working-tree state irrelevant:
  #   nix build                            the app, Release, this platform
  #   nix build .#debug                    the app, Debug
  #   nix build .#<type>-<platform>-<arch>   any permutation:
  #     release-linux-x64   debug-linux-x64   (same set with -aarch64 on ARM Linux;
  #                                            -macos-aarch64 on Apple Silicon)
  #     release-linux-x64-anygpu (alias: .#anygpu)   bundled mesa ICDs
  #   nix run [.#play]                     launch on this machine's GPU: host drivers,
  #                                        store mesa ICDs (additive), nixglhost for host
  #                                        NVIDIA. WSL refused — no in-guest render target.
  #   nix build .#tests                    build + run the headless selftest in the sandbox
  #                                        (solver convergence, constraint residuals, Skia
  #                                        raster coverage). Fails = red.
  #   nix flake check --no-build --all-systems   eval-only sweep; plain flake check BUILDS
  #                                              and runs the selftest.
  #
  # Impure side — your working tree, output in ./build/<label>/:
  #   nix run .#dev                        halts if the submodule gitlink disagrees with the
  #                                        flake pin, auto-inits if absent, configures and
  #                                        builds, then launches through the GPU bridge
  #   nix develop                          the same env, you drive
  #
  # Platform status: Linux is verified. macOS evaluates but is unbuilt here — Skia's GN
  # build and the Qt/MoltenVK path both want a real check before they are claimed.
  # Windows is deliberately absent: Qt 6 and Skia both cross-compile poorly under MinGW,
  # and a package output that has never built is worse than no output at all.
  #
  # git flakes see tracked files only: `git add` new files or nix will not. Submodule
  # contents are NOT visible to a git flake, which is why solvespace arrives as a pinned
  # flake input and is injected in postUnpack rather than read from the work tree.
  description = "paroculus — parametric vector layout, design and animation suite";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/b5aa0fbd538984f6e3d201be0005b4463d8b09f8";

    # Pinned submodule source, rev matches .gitmodules / the recorded gitlink.
    # Only src/{constrainteq,entity,expr,system,util,platform/platformbase}.cpp and
    # src/slvs/lib.cpp are compiled — see CMakeLists.txt. SolveSpace's own nine extlib
    # submodules are never fetched because the solver's header chain does not reach them.
    solvespace-src = {
      url = "github:solvespace/solvespace/27b6a080c8b669421bd4d444650c3b8eddec5687";
      flake = false;
    };

    # Host NVIDIA userspace bridge for non-NixOS hosts.
    nix-gl-host = {
      url = "github:numtide/nix-gl-host";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      solvespace-src,
      nix-gl-host,
    }:
    let
      lib = nixpkgs.lib;
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
      forAllSystems = f: lib.genAttrs systems f;

      # Feed each attr its own name in as the variant.
      mkVariants = builder: lib.mapAttrs (variant: args: builder (args // { inherit variant; }));

      # Store mesa Vulkan ICDs, colon-joined: hardware first, lvp last. Qt Quick's RHI
      # takes the Vulkan backend when QSG_RHI_BACKEND=vulkan; OpenGL is the default and
      # rides the same driver closure.
      mesaVkIcdPaths =
        pkgs: host:
        lib.concatStringsSep ":" (
          map (d: "${pkgs.mesa}/share/vulkan/icd.d/${d}_icd.${host.parsed.cpu.name}.json") [
            "radeon"
            "intel"
            "nouveau"
            "lvp"
          ]
        );

      # Copy a pinned submodule source into the unpacked tree.
      injectSubmodule = name: src: ''
        rm -rf "$sourceRoot/external/${name}"
        cp -r ${src} "$sourceRoot/external/${name}"
        chmod -R u+w "$sourceRoot/external/${name}"
      '';

      # path=rev pairs shared by the shell warning and the nix-run fatal gate.
      pinList = "external/solvespace=${solvespace-src.rev}";

      # Shell-entry warning when recorded gitlinks disagree with the flake pins.
      submodulePinWarn = ''
        if git rev-parse --git-dir >/dev/null 2>&1; then
          for pair in ${pinList}; do
            p="''${pair%%=*}" want="''${pair#*=}"
            rec="$(git ls-tree HEAD "$p" 2>/dev/null | awk '{ print $3 }')"
            if [ -n "$rec" ] && [ "$rec" != "$want" ]; then
              echo "[paroculus] WARNING: $p gitlink $rec != flake pin $want — stale submodule commit. Run 'git submodule update --init' and commit the corrected pointer." >&2
            fi
          done
        fi
      '';

      # One app derivation for every permutation.
      # pkgs/stdenv: host package set + compiler.
      # variant: qualified attr name, becomes the pname suffix.
      # buildType: Release | Debug.
      # tests: PAROCULUS_TESTS + ctest in checkPhase.
      # anyGpu: mesa ICDs as a VK_ADD_DRIVER_FILES default. Opt out: VK_ADD_DRIVER_FILES="".
      # Invariant: install ships bin/paroculus, wrapped with its Qt plugin and QML paths.
      mkApp =
        {
          pkgs,
          stdenv,
          variant,
          buildType,
          tests ? false,
          anyGpu ? false,
        }:
        let
          host = stdenv.hostPlatform;
          isDebug = buildType == "Debug";
          qt = pkgs.qt6;
          # Additive to the host driver scan; wrapQtAppsHook consumes qtWrapperArgs.
          wrapperArgs = lib.optionals (host.isLinux && anyGpu) [
            "--set-default"
            "VK_ADD_DRIVER_FILES"
            (mesaVkIcdPaths pkgs host)
          ];
        in
        stdenv.mkDerivation {
          pname = "paroculus-${variant}";
          version = "0.0.1";
          src = self;

          nativeBuildInputs = (
            with pkgs.buildPackages;
            [
              cmake
              ninja
              pkg-config
            ]
          )
          ++ [
            qt.wrapQtAppsHook
            # qsb, invoked by qt_add_qml_module for any shader the Quick scene graph needs.
            qt.qtshadertools
          ];

          buildInputs = [
            qt.qtbase
            # Quick, Qml, and the Controls.Basic style Main.qml imports.
            qt.qtdeclarative
            pkgs.skia
            pkgs.eigen
            # platformbase.cpp uses the heap-arena API (mi_heap_new / _destroy / _zalloc),
            # not just the malloc override, so this is load-bearing rather than a swap.
            pkgs.mimalloc
            # The bundled typeface. Dimension text has to render identically on
            # every machine and inside the sandbox, so the font is a pinned
            # build input rather than whatever fontconfig finds at runtime — a
            # raster assertion against a host font is an assertion about the
            # host. DejaVu because it is already in the pinned nixpkgs, is
            # freely redistributable, and is a workhorse rather than a style
            # statement, which is what a dimension label wants to be.
            pkgs.dejavu_fonts
          ]
          # Header-only, test-only: paroculus-tests is the doctest runner over
          # every layer below the shell. Only configured in when tests are on.
          ++ lib.optional tests pkgs.doctest;

          postUnpack = injectSubmodule "solvespace" solvespace-src;

          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=${buildType}"
            "-DPAROCULUS_FONT=${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSans.ttf"
          ]
          ++ lib.optional tests "-DPAROCULUS_TESTS=ON";

          # Debug builds need -O off, which fortify refuses.
          hardeningDisable = lib.optionals isDebug [
            "fortify"
            "fortify3"
          ];

          doCheck = tests;
          # checkPhase runs before fixupPhase, so the binary is not yet wrapped and finds
          # no Qt plugins on its own. The selftest never loads QML — it only needs the
          # offscreen QPA plugin, since the sandbox has no display server.
          checkPhase = ''
            runHook preCheck
            export QT_QPA_PLATFORM=offscreen
            export QT_PLUGIN_PATH="${qt.qtbase}/lib/qt-6/plugins"
            export HOME="$TMPDIR/paroculus-home"
            mkdir -p "$HOME"
            ctest --output-on-failure
            runHook postCheck
          '';

          qtWrapperArgs = wrapperArgs;

          meta = {
            description = "paroculus — ${variant}";
            mainProgram = "paroculus";
            # SolveSpace's solver is linked in, so the distributed binary is GPL-3+.
            license = lib.licenses.gpl3Plus;
            platforms = lib.platforms.linux ++ lib.platforms.darwin;
          };
        };

      perSystem =
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          host = pkgs.stdenv.hostPlatform;
          isLinux = host.isLinux;
          archTag = if host.isx86_64 then "x64" else "aarch64";
          hostTag = (if isLinux then "linux" else "macos") + "-" + archTag;

          # Skia ships as a C++ shared library built by the same nixpkgs stdenv; matching
          # its toolchain keeps one libstdc++ and one set of C++ ABI rules in the closure.
          appStdenv = pkgs.stdenv;

          mkHost =
            args:
            mkApp (
              {
                inherit pkgs;
                stdenv = appStdenv;
              }
              // args
            );

          native = mkVariants mkHost (
            {
              "release-${hostTag}" = {
                buildType = "Release";
              };
              "debug-${hostTag}" = {
                buildType = "Debug";
              };
            }
            // lib.optionalAttrs isLinux {
              "release-${hostTag}-anygpu" = {
                buildType = "Release";
                anyGpu = true;
              };
            }
          );

          # Host-resolved short names.
          aliases = {
            default = native."release-${hostTag}";
            release = native."release-${hostTag}";
            debug = native."debug-${hostTag}";
          }
          // lib.optionalAttrs isLinux {
            anygpu = native."release-${hostTag}-anygpu";
          };

          # Sandbox test suite. Building it runs it.
          checks = mkVariants mkHost {
            tests = {
              buildType = "Debug";
              tests = true;
            };
          };

          # nixglhost: harvests the host GL/Vulkan userspace at runtime (non-NixOS).
          nixglhost = if isLinux then nix-gl-host.packages.${system}.default else null;

          # nix run [.#play]: the pure Release app on whatever GPU this machine has.
          # Pessimistic: stack every rung and let the loader pick. Host drivers load
          # natively where /run/opengl-driver exists; store mesa ICDs are staged additively
          # (VK_ADD_DRIVER_FILES="" opts out); a host NVIDIA kernel module on a foreign
          # distro takes the nixglhost harvest. Runs from a writable per-user dir so
          # stray CWD writes never target the store.
          playLauncher =
            let
              app = native."release-${hostTag}";
            in
            pkgs.writeShellApplication {
              name = "paroculus-play";
              runtimeInputs = [
                pkgs.coreutils
                pkgs.gnugrep
              ];
              text =
                (
                  if isLinux then
                    ''
                      if [ -r /proc/version ] && grep -qi microsoft /proc/version; then
                        echo "[paroculus] WSL has no in-guest render target for Qt Quick." >&2
                        exit 1
                      fi
                      dir="''${XDG_DATA_HOME:-$HOME/.local/share}/paroculus"
                    ''
                  else
                    ''
                      dir="$HOME/Library/Application Support/paroculus"
                    ''
                )
                + ''
                  mkdir -p "$dir"
                  cd "$dir" || exit 1
                ''
                + lib.optionalString isLinux ''
                  export VK_ADD_DRIVER_FILES="''${VK_ADD_DRIVER_FILES-${mesaVkIcdPaths pkgs host}}"
                  if [ ! -e /run/opengl-driver ] && [ -e /proc/driver/nvidia/version ]; then
                    echo "[paroculus] NVIDIA kernel module detected — bridging via nixglhost." >&2
                    exec ${nixglhost}/bin/nixglhost ${lib.getExe app} "$@"
                  fi
                ''
                + ''
                  exec ${lib.getExe app} "$@"
                '';
            };

          # nix run .#dev: the impure entry. Fatal pin check, submodule supply, configure,
          # build, then launch through the same GPU bridge as .#play.
          runWrapper = pkgs.writeShellApplication {
            name = "paroculus-dev";
            runtimeInputs = [
              pkgs.git
              pkgs.coreutils
            ];
            text = ''
              root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
              if [ -z "$root" ] || [ ! -f "$root/CMakeLists.txt" ]; then
                echo "[paroculus] not inside the paroculus work tree." >&2
                exit 1
              fi
              cd "$root" || exit 1

              fail=0
              # pinList holds one path=rev pair today and is meant to grow.
              # shellcheck disable=SC2043
              for pair in ${pinList}; do
                p="''${pair%%=*}" want="''${pair#*=}"
                rec="$(git ls-tree HEAD "$p" 2>/dev/null | awk '{ print $3 }')"
                if [ -n "$rec" ] && [ "$rec" != "$want" ]; then
                  echo "[paroculus] FATAL: $p is at $rec but flake.nix pins $want." >&2
                  fail=1
                fi
              done
              if [ "$fail" -ne 0 ]; then
                echo "[paroculus] run 'git submodule update --init --recursive' and commit the corrected pointer, or update the flake pin if the bump is intentional." >&2
                exit 1
              fi

              # shellcheck disable=SC2043
              for pair in ${pinList}; do
                p="''${pair%%=*}"
                if [ -z "$(ls -A "$p" 2>/dev/null)" ]; then
                  echo "[paroculus] fetching submodules..."
                  git submodule update --init --recursive
                  break
                fi
              done

              nix develop "$root" --command cmake -G Ninja -S . -B build/Debug \
                -DCMAKE_BUILD_TYPE=Debug -DPAROCULUS_TESTS=ON \
                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
              nix develop "$root" --command cmake --build build/Debug
              ln -sf build/Debug/compile_commands.json compile_commands.json

              bin="$root/build/Debug/paroculus"
              echo "[paroculus] launching $bin"
              ${lib.optionalString isLinux ''
                if [ ! -e /run/opengl-driver ]; then
                  export VK_ADD_DRIVER_FILES="''${VK_ADD_DRIVER_FILES-${mesaVkIcdPaths pkgs host}}"
                  if [ -e /proc/driver/nvidia/version ]; then
                    echo "[paroculus] NVIDIA kernel module detected — bridging via nixglhost."
                    exec nix develop "$root" --command ${nixglhost}/bin/nixglhost "$bin" "$@"
                  fi
                fi
              ''}
              exec nix develop "$root" --command "$bin" "$@"
            '';
          };

          shellTools = with pkgs; [
            cmake
            ninja
            pkg-config
            git
            clang-tools
          ];

          devShells.default = pkgs.mkShell {
            name = "paroculus-${hostTag}";
            nativeBuildInputs = shellTools ++ [
              pkgs.qt6.qtshadertools
              pkgs.qt6.wrapQtAppsHook
            ];
            buildInputs = [
              pkgs.qt6.qtbase
              pkgs.qt6.qtdeclarative
              pkgs.skia
              pkgs.eigen
              pkgs.mimalloc
              pkgs.doctest
              pkgs.dejavu_fonts
            ];
            # A work-tree binary is never wrapped, so it inherits none of the
            # plugin paths the installed app gets from wrapQtAppsHook. Without
            # these, `nix run .#dev` finds no QPA platform plugin and no
            # QtQuick.Controls.Basic, and exits 1 before the window appears.
            shellHook = ''
              # The bundled typeface, for a work-tree configure. CMakeLists
              # falls back to this when -DPAROCULUS_FONT was not passed, so
              # `nix run .#dev` needs no extra flag of its own.
              export PAROCULUS_FONT="${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSans.ttf"
              export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/lib/qt-6/plugins''${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
              for v in QML_IMPORT_PATH QML2_IMPORT_PATH; do
                export "$v=${pkgs.qt6.qtdeclarative}/lib/qt-6/qml"
              done
              echo "[paroculus] ${hostTag} — $(cmake --version | head -1)"
              echo "[paroculus] configure: cmake -G Ninja -S . -B build/Debug -DPAROCULUS_TESTS=ON"
            ''
            + submodulePinWarn;
          };
        in
        {
          packages = native // aliases // checks // {
            play = playLauncher;
          };
          inherit checks devShells;
          apps = {
            default = {
              type = "app";
              program = lib.getExe playLauncher;
              meta.description = "paroculus — parametric vector layout, design and animation suite";
            };
            play = {
              type = "app";
              program = lib.getExe playLauncher;
              meta.description = "paroculus — launch on this machine's GPU";
            };
            dev = {
              type = "app";
              program = lib.getExe runWrapper;
              meta.description = "paroculus — configure, build and launch from the work tree";
            };
          };
        };
      # Evaluate each system once, then project the output types.
      perSys = forAllSystems perSystem;
    in
    {
      packages = lib.mapAttrs (_: s: s.packages) perSys;
      checks = lib.mapAttrs (_: s: s.checks) perSys;
      devShells = lib.mapAttrs (_: s: s.devShells) perSys;
      apps = lib.mapAttrs (_: s: s.apps) perSys;
      # nixfmt-tree: tree-mode `nix fmt`.
      formatter = forAllSystems (s: nixpkgs.legacyPackages.${s}.nixfmt-tree);
    };
}
