# Building and running 86Box on macOS

`work/build.sh` (zsh) is a local convenience wrapper around the normal CMake build. It
builds against the two MacPorts installations on this machine and can be invoked from any
directory. `work/run.sh` is a symlink to it, so the script name selects the default action.

| MacPorts prefix | Architecture | Deployment target |
| --------------- | ------------ | ----------------- |
| `/opt/local`    | arm64 (Apple Silicon) | macOS 11.0 (Big Sur) |
| `/opt/intel`    | x86_64 (Intel)        | macOS 10.13 (High Sierra) |

## Actions

One action is required. The script's own name supplies it when no action flag is given:
a script named `run.sh` defaults to `--run`, one named `build.sh` to `--build`. Any other
name with no action flag reports that an imperative command is required.

| Action | Effect |
| ------ | ------ |
| `--build` | Build 86Box. Both variants (standard and GDB stub) of each requested architecture are always built, into separate directories. |
| `--run` | Run the already-built binary. If nothing has been built for the requested target yet, it builds that first — so `--intel` or `--fat` runs build the Intel or universal build as needed. Pass `--build` to build unconditionally. |
| `--check` | Verify MacPorts per requested architecture; build nothing. |

## Options

| Option | Effect |
| ------ | ------ |
| `--fat` | Also build the Intel slice and merge a universal (`x86_64` + `arm64`) app. Without it only the Apple Silicon release is built. |
| `--gdb` | Select the GDB stub variant for `--run` and for the path printed last. Both variants are built anyway. |
| `--intel` | Build and use the Intel (`x86_64`) build instead of arm64. With `--fat`, run the Intel side of the universal app. |
| `--arm` | Build and use the Apple Silicon build (the default). |
| `--dry-run` | Print the commands that would be run instead of running them — the build commands (between `pushd` and `popd`) when building, the launch command when running. |
| `--background`, `-g` | Launch with `open -g`, so the app does not steal focus. Implies `--run`. Not valid with `--intel --fat`: `open` always launches the native architecture. |
| `--version` | Print the version (`7.0.0`). |
| `--help` | Print usage. |
| `--` | Everything after this is passed to the 86Box executable untouched. |

`--intel` and `--arm` are mutually exclusive.

## Options the script passes to 86Box

86Box is always started with these arguments. This script computes them and passes them;
pass them before the `--` to override what it computes. Anything else for 86Box
(`--logfile`, `--fullscreen`, `--vmname`, …) goes after the `--`, where it is passed
through untouched.

| Option | What the script passes |
| ------ | ---------------------- |
| `--rompath path`, `-R` | always; default `~/src/86Box/roms` |
| `--assetpath path`, `-A` | always; default `~/src/86Box/assets` |
| `--vmpath path`, `-P` | computed as `~/Library/Application Support/86Box/Virtual Machines/` whenever a VM name is given; otherwise not passed |
| `--config file`, `-C` | passed when given; mutually exclusive with a VM name |
| `--logfile path`, `-L` | always; default `/dev/null`, so a run does not spam the console |
| `--settings`, `-S` | passed when given (starts 86Box with the settings dialog only) |

An overridden option provided after the `--` works too, since 86Box takes the last
occurrence of each of these options. To see 86Box's own logging on the console, pass
`-L ''` or `-L /dev/stdout`.

## Running a VM

A bare argument before the `--` is taken as a VM name. With no `--vmpath`, the VM root is
assumed to be `~/Library/Application Support/86Box/Virtual Machines/`, and the `86box.cfg`
inside the VM's directory there is started:

    $ work/run.sh -g 486
    [-] Running: open -g /…/artifacts-arm64/86Box.app --args \
        --rompath /Users/josh/src/86Box/roms --assetpath /Users/josh/src/86Box/assets \
        --vmpath /Users/josh/Library/Application\ Support/86Box/Virtual\ Machines/ 486/86box.cfg

With no VM name, 86Box starts with its VM manager, and only the ROM and asset paths are
supplied. The VM root is passed with a trailing slash because 86Box joins the relative
config path onto it before adding its own separator.

The VM name has to come before the `--`: after it, a bare argument is handed to 86Box
as-is, which would interpret it relative to the current directory instead.

## Examples

    work/build.sh                                 # build: Apple Silicon, standard + GDB stub
    work/build.sh --fat                           # build: universal, standard + GDB stub
    work/build.sh --intel                         # build: Intel only
    work/build.sh --dry-run --fat                 # print the build command sequence
    work/run.sh                                   # 86Box GUI, VM manager
    work/run.sh -g 486                            # start the "486" VM, in the background
    work/run.sh --gdb Convertible                 # start a VM with the GDB stub build
    work/run.sh -R ~/other-roms --settings        # settings dialog, alternate ROMs
    work/run.sh --fat --intel -- --fullscreen     # Intel side of the universal app
    work/build.sh --check --fat                   # are both MacPorts trees usable?

## What gets built

Two variants are built for each requested architecture, into separate directories, so a GDB
build and a normal build can be kept and run side by side:

| Variant | `GDBSTUB` | Dynamic recompiler | arm64 slice dir | x86_64 slice dir |
| ------- | --------- | ------------------ | --------------- | ---------------- |
| standard | `OFF` | `ON` | `build/slice-arm64` | `build/slice-x86_64` |
| GDB stub | `ON` | `OFF` (forced when `GDBSTUB=ON`) | `build/slice-arm64-gdb` | `build/slice-x86_64-gdb` |

Resulting apps:

| Variant | Apple Silicon only | Intel only (`--intel`) | with `--fat` |
| ------- | ------------------ | ---------------------- | ------------ |
| standard | `build/artifacts-arm64/86Box.app` | `build/artifacts-x86_64/86Box.app` | `build/universal/86Box.app` |
| GDB stub | `build/artifacts-arm64-gdb/86Box.app` | `build/artifacts-x86_64-gdb/86Box.app` | `build/universal-gdb/86Box.app` |

## Output contract

Building finishes with the selected variant's executable path on the last line and the
other variant's on the line above it:

    $ work/build.sh --fat
    [-] Configuring 86Box for arm64, standard (macOS 11.0, /opt/local)
    ...
    [-] GDB build: /…/build/universal-gdb/86Box.app/Contents/MacOS/86Box
    /…/build/universal/86Box.app/Contents/MacOS/86Box

`--dry-run --build` prints the build instead — a `pushd` to the repository root, the
`cmake` configure/build/install commands for each slice, a merge note, the `codesign`
commands, then `popd`:

    $ work/build.sh --dry-run
    pushd /Users/josh/src/86Box/86Box-7690
    cmake -G Ninja -S . -B build/slice-arm64 -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=arm64 -DARCH=arm64 -DNEW_DYNAREC=ON …
    cmake --build build/slice-arm64 -j 18
    cmake --install build/slice-arm64
    codesign --force --deep --sign - --options runtime --entitlements src/mac/entitlements.plist /…/build/artifacts-arm64/86Box.app
    …
    popd

`--run` prints the launch command and then starts the binary in the foreground (so its
output and exit status are yours); `--run --dry-run` prints the command only.

On failure the exit status is non-zero — 3 configure, 4 build, 5 install, 6 merge,
7 sign — and the message names the log holding the details.

## --check

For each requested architecture it verifies that MacPorts is present and runnable, that
its `build_arch` matches, that Qt5 (including the Cocoa/macStyle/ICO/ICNS plugins and
`lconvert`), SDL2 (Intel) or SDL3 (ARM), `vulkan/vulkan.h` and the required `pkg-config`
modules (`slirp`, `sndfile`, `freetype2`, `rtmidi`, `fluidsynth`, `libserialport`) are
all there, and that Qt5 is the right architecture and no newer than the deployment target.

    $ work/build.sh --check --fat
    [check] arm64  /opt/local  macOS 11.0  OK
    [check] x86_64 /opt/intel  macOS 10.13 OK

A failed check prints the reasons and returns 1:

    [check] x86_64 /nonexistent macOS 10.13 NOT BUILDABLE
              - no MacPorts installation at /nonexistent (directory does not exist)

Bundled libraries that require a newer macOS than the deployment target are reported as
`note:` lines without failing the check. The same pre-flight also runs before every build,
so an unusable tree is reported in seconds instead of after a long compile.

## Details

- Slice directories are reused, so repeat builds are incremental. A slice generated for a
  different source path (a moved or copied tree) is detected and recreated.
- `--run` only builds when the artifact for the requested target is missing; otherwise it
  starts the existing one immediately. `--dry-run` never builds — it just prints what
  would run.
- Merging lipo's the matching files from both bundles together, copies files that exist in
  only one of them (for example `libSDL3.0.dylib`, which is ARM-only here), and preserves
  Qt's framework symlinks.
- Each app is signed ad-hoc with `src/mac/entitlements.plist`. This is required: `lipo`
  invalidates the arm64 code signature, and macOS will not run an arm64 binary whose
  signature is broken.
- `--intel --fat` moves the universal app to its Intel side with `arch -x86_64`, so it runs
  under Rosetta. macOS warns that it is an Intel app; that warning is expected. A
  thin `--intel` build needs no `arch`, since Rosetta picks it up on its own.
- `open` re-activates an already-running 86Box instead of starting a second copy, so quit
  any running instance before launching a VM from here.
- The default `--logfile /dev/null` silences 86Box's own log stream, which is the bulk of
  what a run prints. A few Qt and VM-manager lines (Qt version, translations, the config
  scan) still reach the console; they are not part of the log stream.
- Logs are per variant: `work/logs/arm64.log`, `arm64-gdb.log`, `x86_64.log`,
  `x86_64-gdb.log`, `sign.log`, `sign-gdb.log`.
- A full four-slice build (`--fat`) occupies around 1.6 GB under `build/`.
- Non-standard MacPorts prefixes: `MP_ARM_PREFIX=... MP_X86_PREFIX=... work/build.sh`.
