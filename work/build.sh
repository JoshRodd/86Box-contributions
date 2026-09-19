#!/bin/zsh
#
# 86Box   A hypervisor and IBM PC system emulator that specializes in
#         running old operating systems and software designed for IBM
#         PC systems and compatibles from 1981 through fairly recent
#         system designs based on the PCI bus.
#
#         Local macOS build script (zsh). This is a developer convenience
#         wrapper around the normal CMake build and is not part of the
#         upstream build system.
#
# It can be run from any directory; it changes to the directory containing
# it and then to the repository root.
#
# It drives the two local MacPorts installations:
#
#   /opt/local   arm64   Apple Silicon, macOS 11.0 deployment target
#   /opt/intel   x86_64  Intel,         macOS 10.13 deployment target
#
# Usage: build.sh [--build] [--run] [--fat] [--gdb] [--intel|--arm]
#                  [--dry-run] [--background|-g] [--check] [--version]
#                  [--help] [-- args passed to 86Box...]
#
# Actions (one is required unless the script's own name supplies it: a script
# named run.sh defaults to --run, one named build.sh to --build):
#
#   --build   build 86Box. Always builds both the standard and the GDB stub
#             variant, into separate artifacts.
#   --run     run the built binary. If nothing has been built for the target
#             yet, it is built first (with --intel or --fat, that builds the
#             Intel or universal build). Pass --build to build unconditionally.
#   --check   only verify that MacPorts is installed properly for the
#             requested architecture(s), then exit.
#
# Build options:
#
#   --fat     also build the Intel slice and merge a universal (fat) app.
#             Without it only the Apple Silicon release is built.
#   --gdb     select the GDB stub variant. Both variants are built anyway;
#             this decides which one is selected for --run and printed last.
#   --intel   build and use the Intel (x86_64) build instead of arm64.
#             With --fat, run the Intel side of the universal app.
#   --arm     build and use the Apple Silicon build (the default).
#   --terminal
#             build the terminal-only (tigt) frontend instead of the Qt GUI,
#             into its own build and artifact directories (slice-<arch>-terminal
#             and -terminal-gdb), so the GUI and terminal builds can coexist and
#             be compared. Both variants are built, as with the GUI. Needs a
#             tree with tigt support; --fat does not apply.
#   --tty     implies --terminal, and runs that frontend with
#             TIGT_PRESENTATION=glass (the glass-TTY presentation streamed to
#             stdout). The terminal frontend has no VM manager, so a run needs
#             a VM name or --config.
#
# Other:
#
#   --dry-run print the commands that would be run instead of running them:
#             the build commands wrapped in pushd/popd when building, the
#             launch command when running.
#   --background, -g
#             launch with "open -g", so the app does not steal focus.
#             Implies --run. Not valid with --intel --fat, because "open"
#             always launches the native architecture.
#   --version print the version and exit.
#   --help    print usage and exit.
#   --        everything after this is passed to the 86Box executable.
#
# The script always hands these to 86Box, computing the defaults itself; pass
# them before the -- to override what it computes:
#
#   --rompath path,   -R   ROM directory   (default ~/src/86Box/roms)
#   --assetpath path, -A   asset directory (default ~/src/86Box/assets)
#   --vmpath path,    -P   VM directory root
#   --config file,    -C   configuration file
#   --logfile path,   -L   log file        (default /dev/null, so that 86Box
#                          does not spam the console; '' logs to the console)
#   --settings,       -S   start with the settings dialog only
#
# Any other bare argument is taken as a VM name: without --vmpath, the script
# then uses ~/Library/Application Support/86Box/Virtual Machines as the VM root
# and starts the 86Box.cfg inside <name> there.
#
# When building, the last line written to stdout is the selected variant's
# executable path and the line above is the other variant's; with --dry-run
# the build commands are printed instead, ending in popd. When running, the
# launch command is printed before the binary is started. The exit status is
# 0 on success and non-zero otherwise (the failing step's status where
# applicable).
#
# Environment overrides for non-standard MacPorts prefixes:
#   MP_ARM_PREFIX   (default: /opt/local)   arm64 MacPorts prefix
#   MP_X86_PREFIX   (default: /opt/intel)   x86_64 MacPorts prefix
#

set -u

# This is a zsh script. It can be run from any directory: move to the
# directory holding it (normally work/), then up to the repository root, so
# that everything below resolves inside the tree regardless of the caller's
# working directory.
cd -- "${0:A:h}" || exit 1
SCRIPT_DIR=$PWD
cd .. || exit 1
ROOT=$PWD

# If the script itself sits in the source tree rather than in work/, the ".."
# above overshoots; step back into the tree.
if [ ! -f "$ROOT/CMakeLists.txt" ] && [ -f "$SCRIPT_DIR/CMakeLists.txt" ]; then
    cd -- "$SCRIPT_DIR" || exit 1
    ROOT=$PWD
fi
ARM_PREFIX="${MP_ARM_PREFIX:-/opt/local}"
X86_PREFIX="${MP_X86_PREFIX:-/opt/intel}"
ARM_TARGET=11.0
X86_TARGET=10.13

BUILD_DIR="$ROOT/build"
LOG_DIR="$ROOT/work/logs"
HOST_PREFIXES="/opt/homebrew;/usr/local"

# Version reported by --version.
VERSION="7.0.0"

# Whether this tree can build the terminal frontend. Without the TERMINAL
# option, -DTERMINAL=ON would be ignored and the build would quietly produce
# the SDL frontend instead, so the two frontends could not be told apart.
# Defined up here because the argument validation below uses it.
supports_terminal() {
    [ -f "$ROOT/CMakeLists.txt" ] && grep -q 'option(TERMINAL' "$ROOT/CMakeLists.txt"
}

usage() {
    cat << 'EOF'
Usage: build.sh [--build] [--run] [--fat] [--gdb] [--intel|--arm]
                [--dry-run] [--background|-g] [--check] [--version]
                [--help] [-- arguments passed to 86Box...]

Actions (one is required unless the script's own name supplies it: a script
named run.sh defaults to --run, one named build.sh to --build):

  --build        build 86Box. Both the standard and the GDB stub variant are
                 always built, into separate artifacts.
  --run          run the built binary. If nothing has been built for the
                 requested target yet, it builds that first (so --intel or
                 --fat runs build the Intel or universal build as needed).
  --check        only verify that MacPorts is installed properly for the
                 requested architecture(s), then exit.

Build options:

  --fat          also build the Intel slice and merge a universal (fat) app.
                 Without it only the Apple Silicon release is built.
  --gdb          select the GDB stub variant. Both variants are built anyway;
                 this decides which is selected for --run and printed last.
  --intel        build and use the Intel (x86_64) build instead of arm64.
                 With --fat, run the Intel side of the universal app.
  --arm          build and use the Apple Silicon build (the default).
  --terminal     build the terminal-only (tigt) frontend instead of the Qt
                 GUI, in its own build and artifact directories so both can
                 coexist. Both variants are built, and --fat does not apply.
  --tty          implies --terminal, and runs that frontend with
                 TIGT_PRESENTATION=glass (glass-TTY output on stdout). A run
                 needs a VM name or --config: there is no VM manager in
                 terminal mode.

  --tigt <mode>  implies --terminal, and chooses the tigt presentation for the
                 run: glass (the same as --tty), adaptive or
                 adaptive-reversible (the full-screen presenter), or
                 fullscreen (the classic full-screen terminal UI, which is what
                 an unset presentation selects). The classic path also honours
                 TIGT_GRAPHICS from the environment.

Other:

  --dry-run      print the commands that would be run instead of running
                 them: the build commands wrapped in pushd/popd when
                 building, the launch command when running.
  --background, -g
                 launch with "open -g", so the app does not steal focus.
                 Implies --run, and is not valid with --intel --fat.
  --version      print the 86Box version and exit.
  --help         print this help and exit.
  --             everything after this is passed to the 86Box executable.

86Box is always started with these, computed by this script; pass them before
the -- to override the computed defaults:

  --rompath path,   -R   ROM directory   (default ~/src/86Box/roms)
  --assetpath path, -A   asset directory (default ~/src/86Box/assets)
  --vmpath path,    -P   VM root         (computed when a VM name is given)
  --config file,    -C   configuration file
  --logfile path,   -L   log file        (default /dev/null; pass '' to log to
                         the console instead)
  --settings,       -S   start with the settings dialog only

Anything else for 86Box goes after the --.

Any other bare argument is a VM name: without --vmpath the VM root becomes
~/Library/Application Support/86Box/Virtual Machines, and the 86Box.cfg inside
<name> there is started.

Environment overrides for non-standard MacPorts prefixes:
  MP_ARM_PREFIX  arm64 MacPorts prefix  (default: /opt/local)
  MP_X86_PREFIX  x86_64 MacPorts prefix (default: /opt/intel)

On success the last line of stdout is the selected variant's executable path
(building) or the launch command (running, with --dry-run).
EOF
}

fat=0
build=0
gdb=0
intel=0
arm=0
terminal=0
tty=0
tigt=
run=0
dry_run=0
background=0
check_only=0
settings=0
rompath=
assetpath=
vmpath=
config=
vm_arg=
exec_args=()
options_done=0

# Value of an option that takes one argument ($1 = option, rest = remaining).
opt_value() {
    option=$1
    shift
    if [ $# -eq 0 ]; then
        printf '[!] %s needs a value\n' "$option" >&2
        exit 1
    fi
    printf '%s' "$1"
}

while [ $# -gt 0 ]; do
    arg=$1
    shift
    if [ $options_done -eq 1 ]; then
        exec_args+=("$arg")
        continue
    fi
    case $arg in
        --)              options_done=1 ;;
        --build)         build=1 ;;
        --fat)           fat=1 ;;
        --gdb)           gdb=1 ;;
        --intel)         intel=1 ;;
        --arm)           arm=1 ;;
        --terminal)      terminal=1 ;;
        --tty)           tty=1; terminal=1; tigt=glass ;;
        --tigt)          tigt=$(opt_value "$arg" "$@"); shift; terminal=1 ;;
        --run)           run=1 ;;
        --dry-run)       dry_run=1 ;;
        --background|-g) background=1; run=1 ;;
        --check)         check_only=1 ;;
        --rompath|-R)    rompath=$(opt_value "$arg" "$@"); shift ;;
        --assetpath|-A)  assetpath=$(opt_value "$arg" "$@"); shift ;;
        --vmpath|-P)     vmpath=$(opt_value "$arg" "$@"); shift ;;
        --config|-C)     config=$(opt_value "$arg" "$@"); shift ;;
        --logfile|-L)    logfile=$(opt_value "$arg" "$@"); shift ;;
        --settings|-S)   settings=1 ;;
        --version)       printf '%s\n' "$VERSION"; exit 0 ;;
        -h|--help)       usage; exit 0 ;;
        -*)
            printf '[!] unknown option: %s\n\n' "$arg" >&2
            usage >&2
            exit 1
            ;;
        *)
            # A bare argument is the name of a VM to start.
            if [ -n "$vm_arg" ]; then
                printf '[!] only one VM name may be given (got "%s" and "%s")\n' "$vm_arg" "$arg" >&2
                exit 1
            fi
            vm_arg=$arg
            ;;
    esac
done

# The script's own name can supply the default action: run.sh implies --run,
# build.sh implies --build.
if [ $build -eq 0 ] && [ $run -eq 0 ]; then
    case ${${0:t}%.sh} in
        run)   run=1 ;;
        build) build=1 ;;
    esac
fi

if [ $build -eq 0 ] && [ $run -eq 0 ] && [ $check_only -eq 0 ]; then
    printf '[!] no action given: pass --build, --run or --check\n' >&2
    printf '    (a script named build.sh or run.sh selects one from its name)\n' >&2
    exit 1
fi

if [ $intel -eq 1 ] && [ $arm -eq 1 ]; then
    printf '[!] --intel and --arm are mutually exclusive\n' >&2
    exit 1
fi
if [ $terminal -eq 1 ]; then
    if [ $fat -eq 1 ]; then
        printf '[!] --fat does not apply to --terminal: the terminal frontend builds a plain\n' >&2
        printf '    executable per architecture, with no bundle to merge.\n' >&2
        exit 1
    fi
    if ! supports_terminal; then
        printf '[!] this source tree has no TERMINAL option, so it cannot build the terminal\n' >&2
        printf '    frontend (that needs tigt support; see work/docs/TERMINAL-ONLY-BUILD.md).\n' >&2
        exit 1
    fi
fi
if [ -n "$tigt" ]; then
    case $tigt in
        glass|adaptive|adaptive-reversible|fullscreen) ;;
        *)
            printf '[!] unknown --tigt mode: %s\n' "$tigt" >&2
            printf '    choose glass, adaptive, adaptive-reversible or fullscreen.\n' >&2
            exit 1
            ;;
    esac
fi
if [ $tty -eq 1 ] && [ $background -eq 1 ]; then
    printf '[!] --background launches an app bundle; --tty runs the terminal executable in the\n' >&2
    printf '    caller'"'"'s terminal instead.\n' >&2
    exit 1
fi
if [ $background -eq 1 ] && [ $intel -eq 1 ] && [ $fat -eq 1 ]; then
    printf '[!] --background cannot force the Intel side of a universal app: "open" always\n' >&2
    printf '    launches the native architecture. Use --intel without --fat instead.\n' >&2
    exit 1
fi
if [ -n "$config" ] && [ -n "$vm_arg" ]; then
    printf '[!] a VM name and --config are mutually exclusive (the VM name selects %s)\n' "the 86Box.cfg inside that VM" >&2
    exit 1
fi

# Defaults for the paths handed to 86Box. A VM name implies the standard VM
# root, and the 86Box.cfg inside that VM is started.
: ${rompath:=$HOME/src/86Box/roms}
: ${assetpath:=$HOME/src/86Box/assets}
# Logging goes to a file by default so that 86Box does not spam the console.
# The default applies only when --logfile was not given at all, so an explicit
# empty value (--logfile '') leaves the path unset for 86Box, which then logs
# to the console instead.
if [ -z "${logfile+set}" ]; then
    logfile=/dev/null
fi
if [ -z "$vmpath" ] && [ -n "$vm_arg" ]; then
    vmpath="$HOME/Library/Application Support/86Box/Virtual Machines"
fi
# 86Box joins a relative config path onto the VM root before adding its own
# trailing separator, so the root has to end in one for that join to work.
if [ -n "$vm_arg" ]; then
    case $vmpath in
        */) ;;
        *)  vmpath="$vmpath/" ;;
    esac
fi

game_args=(--rompath "$rompath" --assetpath "$assetpath")
[ -n "$vmpath" ] && game_args+=(--vmpath "$vmpath")
[ -n "$config" ] && game_args+=(--config "$config")
[ $settings -eq 1 ] && game_args+=(--settings)
game_args+=(--logfile "$logfile")
game_args+=("${exec_args[@]}")
# 86Box accepts one positional argument, and it has to be the last one.
[ -n "$vm_arg" ] && game_args+=("$vm_arg/86box.cfg")

# Everything below needs the source tree and macOS.
if [ ! -f "$ROOT/CMakeLists.txt" ]; then
    printf '[!] cannot locate the 86Box source tree from [%s]\n' "$SCRIPT_DIR" >&2
    exit 1
fi

if [ "$(uname -s)" != "Darwin" ]; then
    printf '[!] this script only supports macOS\n' >&2
    exit 1
fi

JOBS=$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

# --- helpers ---------------------------------------------------------------

# Minimum deployment target recorded in a Mach-O file ($1 = file, $2 = arch).
min_os() {
    otool -l -arch "$2" "$1" 2>/dev/null | awk '
        /cmd LC_BUILD_VERSION|cmd LC_VERSION_MIN_MACOSX/ { grab = 1; next }
        grab && /minos/   { print $2; exit }
        grab && /version/ { print $2; exit }
    '
}

# True when dotted version $1 <= $2.
ver_le() {
    [ "$(awk -v a="$1" -v b="$2" 'BEGIN {
            split(a, x, "."); split(b, y, ".");
            for (i = 1; i <= 3; i++) {
                if (x[i] + 0 < y[i] + 0) { print "yes"; exit }
                if (x[i] + 0 > y[i] + 0) { print "no"; exit }
            }
            print "yes"
        }')" = "yes" ]
}

prefix_for() { [ "$1" = "arm64" ] && printf '%s' "$ARM_PREFIX" || printf '%s' "$X86_PREFIX"; }
target_for() { [ "$1" = "arm64" ] && printf '%s' "$ARM_TARGET" || printf '%s' "$X86_TARGET"; }

PROBLEMS=
NOTES=
problem() { PROBLEMS="$PROBLEMS
          - $1"; }
note()    { NOTES="$NOTES
          note: $1"; }

# $1 = file that must exist, $2 = what it provides
need_file() {
    [ -e "$1" ] || problem "missing $2: $1"
}

# --- checking --------------------------------------------------------------

# Verify that the MacPorts installation for $1 can build 86Box for $2 (target),
# leaving any findings in $PROBLEMS / $NOTES.
check_arch() {
    arch=$1
    prefix=$2
    target=$3
    PROBLEMS=
    NOTES=

    if [ ! -d "$prefix" ]; then
        problem "no MacPorts installation at $prefix (directory does not exist)"
        return 1
    elif [ ! -x "$prefix/bin/port" ]; then
        problem "no MacPorts installation at $prefix ($prefix/bin/port is missing)"
        return 1
    else
        if ! "$prefix/bin/port" version > /dev/null 2>&1; then
            problem "MacPorts at $prefix is installed but not runnable"
        fi
        mpconf="$prefix/etc/macports/macports.conf"
        build_arch=$(awk -F'[[:space:]]+' '$1 == "build_arch" { print $2; exit }' "$mpconf" 2>/dev/null)
        if [ -n "$build_arch" ] && [ "$build_arch" != "$arch" ]; then
            problem "MacPorts at $prefix builds for $build_arch, expected $arch"
        fi
        dep_target=$(awk -F'[[:space:]]+' '$1 == "macosx_deployment_target" { print $2; exit }' "$mpconf" 2>/dev/null)
        if [ -z "$dep_target" ]; then
            note "macosx_deployment_target is unset in $mpconf, so ports default to the host macOS version"
        elif [ "$dep_target" != "$target" ]; then
            note "macosx_deployment_target is $dep_target in $mpconf, expected $target"
        fi
    fi

    # Everything CMake is told to find in this prefix.
    need_file "$prefix/lib/cmake/Qt5/Qt5Config.cmake"                "Qt5 (Core/Widgets/Network/OpenGL/Gui)"
    need_file "$prefix/lib/cmake/Qt5LinguistTools/Qt5LinguistToolsConfig.cmake" "Qt5 LinguistTools"
    need_file "$prefix/libexec/qt5/bin/lconvert"                     "Qt5 lconvert tool"
    need_file "$prefix/libexec/qt5/plugins/platforms/libqcocoa.dylib" "Qt5 Cocoa platform plugin"
    need_file "$prefix/libexec/qt5/plugins/styles/libqmacstyle.dylib" "Qt5 macOS style plugin"
    need_file "$prefix/libexec/qt5/plugins/imageformats/libqico.dylib"  "Qt5 ICO image plugin"
    need_file "$prefix/libexec/qt5/plugins/imageformats/libqicns.dylib" "Qt5 ICNS image plugin"
    need_file "$prefix/include/vulkan/vulkan.h"                      "Vulkan headers (used by src/qt/qt_osd.cpp)"
    if [ "$arch" = "arm64" ]; then
        need_file "$prefix/lib/cmake/SDL3/SDL3Config.cmake" "SDL3"
    else
        need_file "$prefix/lib/cmake/SDL2/sdl2-config.cmake" "SDL2"
    fi

    # pkg-config modules that the build requires.
    if [ -x "$prefix/bin/pkg-config" ]; then
        for mod in slirp sndfile freetype2 rtmidi fluidsynth libserialport; do
            "$prefix/bin/pkg-config" --exists "$mod" 2>/dev/null || \
                problem "pkg-config module '$mod' not found via $prefix/bin/pkg-config"
        done
    else
        problem "no pkg-config at $prefix/bin/pkg-config"
    fi

    # Qt5 has to be usable for this architecture and this deployment target.
    qtcore="$prefix/libexec/qt5/lib/QtCore.framework/Versions/5/QtCore"
    if [ -e "$qtcore" ]; then
        case " $(lipo -archs "$qtcore" 2>/dev/null) " in
            *" $arch "*) ;;
            *) problem "Qt5 at $prefix is not built for $arch (QtCore: $(lipo -archs "$qtcore" 2>/dev/null))" ;;
        esac
        got=$(min_os "$qtcore" "$arch")
        if [ -n "$got" ] && ! ver_le "$got" "$target"; then
            problem "Qt5 at $prefix requires macOS $got, newer than the $target deployment target (rebuild with macosx_deployment_target $target)"
        fi
    else
        problem "missing Qt5 runtime: $qtcore"
    fi

    # Libraries that end up inside the app bundle: report any that are newer
    # than the deployment target, since the result could not run on it.
    for pattern in 'libfreetype*.dylib' 'libpng16*.dylib' 'libslirp*.dylib' \
                   'libvdeplug*.dylib' 'libopenal*.dylib' 'librtmidi*.dylib' \
                   'libfluidsynth*.dylib' 'libsndfile*.dylib' 'libzstd*.dylib' \
                   'libserialport*.dylib' 'libSDL*.dylib'; do
        # find matches the pattern itself; zsh does not glob variable contents.
        lib=$(find "$prefix/lib" -maxdepth 1 -name "$pattern" 2> /dev/null | head -1)
        [ -n "$lib" ] || continue
        got=$(min_os "$lib" "$arch")
        [ -n "$got" ] && ! ver_le "$got" "$target" && \
            note "$(basename "$lib") requires macOS $got, newer than $target"
    done

    [ -z "$PROBLEMS" ]
}

# --- steps -----------------------------------------------------------------

if [ $fat -eq 1 ]; then
    arches=(arm64 x86_64)
elif [ $intel -eq 1 ]; then
    arches=(x86_64)
else
    arches=(arm64)
fi

if [ $check_only -eq 1 ]; then
    rc=0
    for arch in $arches; do
        prefix=$(prefix_for "$arch")
        target=$(target_for "$arch")
        printf '[check] %-6s %-11s macOS %-5s ' "$arch" "$prefix" "$target"
        if check_arch "$arch" "$prefix" "$target"; then
            printf 'OK\n'
        else
            printf 'NOT BUILDABLE\n'
            rc=1
        fi
        [ -n "$PROBLEMS" ] && printf '%s\n' "$PROBLEMS"
        [ -n "$NOTES" ] && printf '%s\n' "$NOTES"
    done
    exit $rc
fi

# Refuse to start a long build against an unusable MacPorts tree.
preflight() {
    for arch in $arches; do
        prefix=$(prefix_for "$arch")
        target=$(target_for "$arch")
        if ! check_arch "$arch" "$prefix" "$target"; then
            printf '[check] %-6s %-11s macOS %-5s NOT BUILDABLE\n' "$arch" "$prefix" "$target"
            printf '%s\n' "$PROBLEMS"
            printf '[!] cannot build 86Box for %s; fix the above or run with --check for details\n' "$arch" >&2
            return 1
        fi
        [ -n "$NOTES" ] && printf '[check] %-6s %s\n' "$arch" "$(printf '%s' "$NOTES" | sed 's/^ *//;s/^note: /note: /' | tr '\n' ' ')"
    done
    return 0
}

# Print a command line, shell-quoted so it can be pasted back into a shell.
print_command() {
    line=
    for word in "$@"; do
        [ -n "$line" ] && line+=' '
        line+="${(q)word}"
    done
    printf '%s\n' "$line"
}

# Ditto, but wrapping long commands with backslash continuations.
print_command_wrapped() {
    local line= word quoted
    for word in "$@"; do
        quoted="${(q)word}"
        if [ -n "$line" ] && [ $(( ${#line} + ${#quoted} + 1 )) -gt 92 ]; then
            printf '%s \\\n' "$line"
            line="    $quoted"
        elif [ -n "$line" ]; then
            line+=" $quoted"
        else
            line="$quoted"
        fi
    done
    printf '%s\n' "$line"
}

# Fill in the variables and the exact command lines for one slice, without
# running anything. $1 = arch (arm64/x86_64), $2 = variant suffix, which
# carries both axes: "" and "-gdb" are the Qt GUI frontend, "-terminal" and
# "-terminal-gdb" the terminal-only one.
slice_config() {
    arch=$1
    suffix=$2

    case $suffix in
        *gdb*)      gdbstub=ON;  dynarec=OFF; variant="GDB stub" ;;
        *)          gdbstub=OFF; dynarec=ON;  variant="standard" ;;
    esac
    case $suffix in
        *terminal*) terminal_slice=1; variant="terminal $variant" ;;
        *)          terminal_slice=0 ;;
    esac

    prefix=$(prefix_for "$arch")
    target=$(target_for "$arch")
    case $arch in
        arm64)
            sdl2=OFF
            new_dynarec=ON
            ignore="$HOST_PREFIXES"
            ;;
        x86_64)
            sdl2=ON
            new_dynarec=OFF
            ignore="$HOST_PREFIXES;$ARM_PREFIX"
            ;;
    esac

    # Commands are relative to the repository root, which the script already
    # changed to, so the printed form stays readable and can be pasted back.
    slice_rel="build/slice-$arch$suffix"
    slice_dir="$BUILD_DIR/slice-$arch$suffix"
    log="$LOG_DIR/$arch$suffix.log"

    configure_step=(cmake -G Ninja -S . -B "$slice_rel"
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_OSX_ARCHITECTURES="$arch"
        -DARCH="$arch"
        -DNEW_DYNAREC="$new_dynarec"
        -DDYNAREC="$dynarec"
        -DGDBSTUB="$gdbstub"
        -DCMAKE_PREFIX_PATH="$prefix"
        -DCMAKE_IGNORE_PREFIX_PATH="$ignore"
        -DCMAKE_INSTALL_PREFIX="$BUILD_DIR/artifacts-$arch$suffix")

    if [ $terminal_slice -eq 1 ]; then
        # Mirrors the option set of the tree's "terminal" CMake preset, in our
        # own build directory: no Qt, no SDL frontend.
        configure_step+=(-DTERMINAL=ON
            -DQT=OFF
            -DCMAKE_MACOSX_BUNDLE=OFF
            -DBUILD_TESTING=OFF
            -DBUILD_BENCHMARKS=OFF
            -DOPENAL=ON
            -DRTMIDI=OFF
            -DFLUIDSYNTH=OFF
            -DMUNT=OFF
            -DSOUNDCANVAS=OFF
            -DDISCORD=OFF
            -DVNC=OFF)
    else
        configure_step+=(-DQT=ON
            -DUSE_QT6=OFF
            -DMOLTENVK=OFF
            -DSDL2="$sdl2"
            -DQt5_DIR="$prefix/lib/cmake/Qt5"
            -DQt5LinguistTools_DIR="$prefix/lib/cmake/Qt5LinguistTools")
    fi

    build_step=(cmake --build "$slice_rel" -j "$JOBS")
    install_step=(cmake --install "$slice_rel")
}

# Build one slice, logging the details to work/logs.
build_slice() {
    slice_config "$1" "$2"

    # A build directory generated for a different source path (e.g. the tree
    # was moved or copied) cannot be reused; start it over.
    if [ -f "$slice_dir/CMakeCache.txt" ]; then
        cached_src=$(awk -F= '$1 == "CMAKE_HOME_DIRECTORY:INTERNAL" { print $2; exit }' "$slice_dir/CMakeCache.txt")
        if [ -n "$cached_src" ] && [ "$cached_src" != "$ROOT" ]; then
            printf '[-] Recreating build directory for %s/%s (it was generated from %s)\n' "$arch" "$variant" "$cached_src"
            rm -rf "$slice_dir"
        fi
    fi

    printf '[-] Configuring 86Box for %s, %s (macOS %s, %s)\n' "$arch" "$variant" "$target" "$prefix"
    if ! $configure_step >> "$log" 2>&1; then
        printf '[!] CMake configuration failed for %s, %s (see %s)\n' "$arch" "$variant" "$log" >&2
        return 3
    fi

    printf '[-] Building %s, %s\n' "$arch" "$variant"
    if ! $build_step >> "$log" 2>&1; then
        printf '[!] build failed for %s, %s (see %s)\n' "$arch" "$variant" "$log" >&2
        return 4
    fi

    printf '[-] Installing %s, %s\n' "$arch" "$variant"
    if ! $install_step >> "$log" 2>&1; then
        printf '[!] install failed for %s, %s (see %s)\n' "$arch" "$variant" "$log" >&2
        return 5
    fi
    return 0
}

# Merge the two single-architecture bundles into one universal bundle:
# shared files are lipo'd together, files present in only one bundle are
# copied, and symlinks are preserved from whichever bundle has them.
merge_bundles() {
    arm_app=$1
    x86_app=$2
    out_app=$3

    rm -rf "$out_app"
    mkdir -p "$(dirname "$out_app")"
    if ! ditto "$arm_app" "$out_app"; then
        printf '[!] could not copy %s\n' "$arm_app" >&2
        return 6
    fi
    (
        cd "$x86_app" || exit 1
        find . -type f | while IFS= read -r f; do
            d="$out_app/${f#./}"
            if [ -f "$d" ]; then
                cmp -s "$f" "$d" && continue
                lipo -create -output "$d" "$d" "$f" 2> /dev/null || cp -p "$f" "$d"
            else
                mkdir -p "$(dirname "$d")"
                ditto "$f" "$d"
            fi
        done
        find . -type l | while IFS= read -r l; do
            d="$out_app/${l#./}"
            if [ ! -L "$d" ] && [ ! -e "$d" ]; then
                mkdir -p "$(dirname "$d")"
                ln -s "$(readlink "$l")" "$d"
            fi
        done
    ) || return 6
    return 0
}

sign_app() {
    app=$1
    suffix=$2
    name=$(basename "$app")
    log="$LOG_DIR/sign$suffix.log"
    [ -n "$suffix" ] && name="$name (gdb)"
    printf '[-] Signing %s\n' "$name"
    if ! codesign --force --deep --sign - --options runtime \
            --entitlements "$ROOT/src/mac/entitlements.plist" "$app" >> "$log" 2>&1; then
        printf '[!] code signing failed (see %s)\n' "$log" >&2
        return 7
    fi
    return 0
}

# --- terminal-only build ---------------------------------------------------
#
# The terminal frontend is built as its own slices (see slice_config), so the
# GUI and terminal builds can coexist.

# Executable of a terminal slice ($1 = arch, $2 = variant suffix). Terminal
# builds are plain executables, installed under bin/ rather than a bundle.
terminal_exe() {
    printf '%s/artifacts-%s%s/bin/86Box' "$BUILD_DIR" "$1" "$2"
}

# --- driver ----------------------------------------------------------------

# Bundle path for a variant suffix ("" or "-gdb"), whether or not it has been
# built in this run.
app_path() {
    if [ $fat -eq 1 ]; then
        printf '%s' "$BUILD_DIR/universal$1/86Box.app"
    elif [ $intel -eq 1 ]; then
        printf '%s' "$BUILD_DIR/artifacts-x86_64$1/86Box.app"
    else
        printf '%s' "$BUILD_DIR/artifacts-arm64$1/86Box.app"
    fi
}

# The variant suffixes to build: only the requested frontend is built, so the
# GUI and terminal builds can be produced separately and compared.
if [ $terminal -eq 1 ]; then
    build_suffixes=("-terminal" "-terminal-gdb")
else
    build_suffixes=("" "-gdb")
fi

# --dry-run --build: print the commands instead of running them.
print_build_steps() {
    printf 'pushd %s\n' "${(q)ROOT}"
    for suffix in "${build_suffixes[@]}"; do
        for arch in $arches; do
            slice_config "$arch" "$suffix"
            print_command_wrapped "${configure_step[@]}"
            print_command "${build_step[@]}"
            print_command "${install_step[@]}"
        done
        if [ $terminal -eq 0 ]; then
            if [ $fat -eq 1 ]; then
                printf '# merge the two bundles into %s\n' "build/universal$suffix/86Box.app"
            fi
            print_command codesign --force --deep --sign - --options runtime \
                          --entitlements src/mac/entitlements.plist "$(app_path "$suffix")"
        fi
    done
    printf 'popd\n'
}

# Build both variants of every requested architecture: standard first, then
# the GDB stub. GUI builds are merged (with --fat) and signed; terminal builds
# are plain executables.
build_all() {
    mkdir -p "$LOG_DIR"
    preflight || return 1

    for suffix in "${build_suffixes[@]}"; do
        for arch in $arches; do
            build_slice "$arch" "$suffix" || return $?
        done

        if [ $terminal -eq 1 ]; then
            exe="$(terminal_exe "${arches[1]}" "$suffix")"
            if [ ! -x "$exe" ]; then
                printf '[!] expected 86Box executable not found at %s\n' "$exe" >&2
                return 1
            fi
            continue
        fi

        app="$(app_path "$suffix")"
        if [ $fat -eq 1 ]; then
            if [ -n "$suffix" ]; then label="gdb"; else label="standard"; fi
            printf '[-] Merging arm64 and x86_64 bundles into a universal %s app\n' "$label"
            merge_bundles "$BUILD_DIR/artifacts-arm64$suffix/86Box.app" \
                          "$BUILD_DIR/artifacts-x86_64$suffix/86Box.app" "$app" || return $?
        fi

        sign_app "$app" "$suffix" || return $?

        if [ ! -x "$app/Contents/MacOS/86Box" ]; then
            printf '[!] expected 86Box executable not found at %s\n' "$app/Contents/MacOS/86Box" >&2
            return 1
        fi
    done
    return 0
}

# The selected artifact, whether or not it gets built in this run.
if [ $terminal -eq 1 ]; then
    if [ $gdb -eq 1 ]; then
        primary_executable="$(terminal_exe "${arches[1]}" "-terminal-gdb")"
        other_executable="$(terminal_exe "${arches[1]}" "-terminal")"
        other_label="terminal standard"
    else
        primary_executable="$(terminal_exe "${arches[1]}" "-terminal")"
        other_executable="$(terminal_exe "${arches[1]}" "-terminal-gdb")"
        other_label="terminal GDB stub"
    fi
elif [ $gdb -eq 1 ]; then
    primary_executable="$(app_path "-gdb")/Contents/MacOS/86Box"
    other_executable="$(app_path "")/Contents/MacOS/86Box"
    other_label="standard"
else
    primary_executable="$(app_path "")/Contents/MacOS/86Box"
    other_executable="$(app_path "-gdb")/Contents/MacOS/86Box"
    other_label="GDB"
fi

if [ $build -eq 1 ]; then
    if [ $dry_run -eq 1 ]; then
        print_build_steps
    else
        build_all || exit $?
        printf '[-] %s build: %s\n' "$other_label" "$other_executable"
        printf '%s\n' "$primary_executable"
    fi
fi

if [ $run -eq 1 ]; then
    app="${primary_executable%/Contents/MacOS/86Box}"

    # The terminal frontend has no VM manager: without a config to start, 86Box
    # enters VM-manager mode (only the GUI implements it) and crashes.
    if [ $terminal -eq 1 ] && [ -z "$vm_arg" ] && [ -z "$config" ]; then
        printf '[!] the terminal frontend has no VM manager: give a VM name or --config.\n' >&2
        exit 1
    fi

    # Nothing built for this target yet: build exactly what running it needs
    # (the architecture and variant follow the same options, so a --intel or
    # --fat run builds those). --dry-run only reports the command.
    if [ ! -x "$primary_executable" ] && [ $dry_run -eq 0 ]; then
        if [ $terminal -eq 1 ]; then
            printf '[-] No terminal-only build yet; building it first\n'
        elif [ $fat -eq 1 ]; then
            printf '[-] No universal build yet; building it first\n'
        elif [ $intel -eq 1 ]; then
            printf '[-] No Intel build yet; building it first\n'
        elif [ $gdb -eq 1 ]; then
            printf '[-] No Apple Silicon GDB stub build yet; building it first\n'
        else
            printf '[-] No Apple Silicon build yet; building it first\n'
        fi
        build_all || exit $?
    fi

    if [ ! -x "$primary_executable" ]; then
        if [ $dry_run -eq 1 ]; then
            # Reporting only: the command is still what a build would produce.
            printf '[-] note: %s does not exist yet; a real run would build it first\n' \
                   "$primary_executable" >&2
        else
            printf '[!] no 86Box executable at %s\n' "$primary_executable" >&2
            exit 1
        fi
    fi

    launch_cmd=()
    if [ $terminal -eq 1 ]; then
        # A plain executable: no bundle to launch and no architecture to force.
        # --tty is the glass-TTY presentation streamed to stdout; --tigt picks
        # any of them, and "fullscreen" leaves the presentation unset, which is
        # the classic full-screen terminal UI.
        launch_cmd=()
        if [ -n "$tigt" ] && [ "$tigt" != "fullscreen" ]; then
            launch_cmd=(env "TIGT_PRESENTATION=$tigt")
        fi
        launch_cmd+=("$primary_executable")
    elif [ $background -eq 1 ]; then
        # "open -g" launches without stealing focus, but it always picks the
        # native architecture, so --intel --fat is rejected during parsing.
        launch_cmd=(open -g "$app")
        [ ${#game_args} -gt 0 ] && launch_cmd+=(--args)
    else
        # A thin x86_64 build runs under Rosetta by itself; a universal app
        # has to be told which slice to use.
        [ $intel -eq 1 ] && [ $fat -eq 1 ] && launch_cmd=(arch -x86_64)
        launch_cmd+=("$primary_executable")
    fi
    launch_cmd+=("${game_args[@]}")

    if [ $dry_run -eq 1 ]; then
        print_command "${launch_cmd[@]}"
    elif [ $background -eq 1 ]; then
        # Announcement goes to stderr: with --tty, stdout is the guest's glass
        # stream, and it belongs to 86Box either way.
        printf '[-] Running: %s\n' "$(print_command "${launch_cmd[@]}")" >&2
        "${launch_cmd[@]}"
        exit $?
    else
        printf '[-] Running: %s\n' "$(print_command "${launch_cmd[@]}")" >&2
        exec "${launch_cmd[@]}"
    fi
fi
exit 0
