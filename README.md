(We need help in developing this emulator! See https://github.com/86Box/86Box/issues/7386 for details. Any and all help is appreciated.)

86Box
=====

[![Build Status](https://ci.86box.net/job/86Box/badge/icon)](https://ci.86box.net/job/86Box/)
[![License](https://img.shields.io/github/license/86Box/86Box)](COPYING)
[![Latest release](https://img.shields.io/github/release/86Box/86Box.svg)](https://github.com/86Box/86Box/releases)
[![Downloads](https://img.shields.io/github/downloads/86Box/86Box/total.svg)](https://github.com/86Box/86Box/releases)
[![Translation status](https://weblate.86box.net/widget/86box/86box/language-badge.svg)](https://weblate.86box.net/engage/86box/)

**86Box** is a low level x86 emulator that runs older operating systems and software designed for IBM PC systems and compatibles from 1981 through fairly recent system designs based on the PCI bus.

Features
--------

* Easy to use interface inspired by mainstream hypervisor software
* Low level emulation of 8086-based processors up to the Mendocino-era Celeron with focus on accuracy
* Great range of customizability of virtual machines
* Many available systems, such as the very first IBM PC 5150 from 1981, or the more obscure IBM PS/2 line of systems based on the Micro Channel Architecture
* Lots of supported peripherals including video adapters, sound cards, network adapters, hard disk controllers, and SCSI adapters
* MIDI output to Windows built-in MIDI support, FluidSynth, or emulated Roland synthesizers
* Supports running MS-DOS, older Windows versions, OS/2, many Linux distributions, or vintage systems such as BeOS or NEXTSTEP, and applications for these systems

Minimum system requirements and recommendations
-----------------------------------------------

* 64-bit Intel Core 2, AMD Athlon 64 or ARMv8 processor or newer
* 4 GB of RAM or higher
* **Windows version:** Windows 7 Service Pack 1 or newer on Intel/AMD systems; Windows 11 or newer on ARM systems
* **Linux version:** Ubuntu 16.04, Debian 9.0 or other distributions from 2016 onwards
* **macOS version:** macOS 10.14 Mojave or newer

Performance may vary depending on host and guest configuration. Most emulation logic is executed in a single thread. Therefore, systems with greater IPC (instructions per clock) capacity should be able to emulate higher clock speeds.

For easier handling of multiple virtual machines, use a manager application:

* [Avalonia 86](https://github.com/notBald/Avalonia86) by [notBald](https://github.com/notBald) (Windows and Linux)
* [86Box Manager](https://github.com/86Box/86BoxManager) by [Overdoze](https://github.com/daviunic) (Windows only)
* [86Box Manager X](https://github.com/RetBox/86BoxManagerX) by [xafero](https://github.com/xafero) (Cross platform Port of 86Box Manager using Avalonia)
* [sl86](https://github.com/DDXofficial/sl86) by [DDX](https://github.com/DDXofficial) (Command-line 86Box machine manager written in Python)
* [Linbox-qt5](https://github.com/Dungeonseeker/linbox-qt5) by [Dungeonseeker](https://github.com/Dungeonseeker/) (Linux focused, should work on Windows though untested)
* [MacBox for 86Box](https://github.com/Moonif/MacBox) by [Moonif](https://github.com/Moonif) (MacOS only)

To use 86Box on its own, use the `--vmpath`/`-P` command line option.

Getting started
---------------

See [our documentation](https://86box.readthedocs.io/en/latest/index.html) for an overview of the emulator's features and user interface.

Community
---------

We operate an IRC channel and a Discord server for discussing 86Box, its development, and anything related to retro computing. We look forward to hearing from you!

[![Visit our IRC channel](https://kiwiirc.com/buttons/irc.ringoflightning.net/86Box.png)](https://kiwiirc.com/client/irc.ringoflightning.net/?nick=86box|?#86Box)

[![Visit our Discord server](https://discordapp.com/api/guilds/262614059009048590/embed.png)](https://discord.gg/QXK9XTv)

[Forum: SoftHistory](https://forum.softhistory.org/)

[Wiki: SoftHistory](https://wiki.softhistory.org/)

[Twitter: @86BoxEmulator](https://twitter.com/86BoxEmulator)

[YouTube: 86Box](https://youtube.com/c/86Box)

Contributions
-------------

We welcome all contributions to the project, as long as the [contribution guidelines](CONTRIBUTING.md) are followed.

Building
---------
For instructions on how to build 86Box from source, see the [build guide](https://86box.readthedocs.io/en/latest/dev/buildguide.html).

### Terminal-only build (Unix)

The `terminal` preset builds the native tigt terminal frontend and C/Rust keyboard
mapper without Qt or SDL. It requires CMake 3.20 or newer, Ninja, C11 and C++17
compilers, Rust/Cargo with Rust 2024 edition support, and development packages for
curses (such as ncurses), OpenAL, FreeType, libpng, libsndfile, and libslirp
(including its GLib dependencies). Install pkg-config so CMake can discover the
native libraries. Initialize the repository submodules, including `submodules/tigt`
and `submodules/terminal-to-pc-keyboard`, before configuring.

```sh
git submodule update --init --recursive
cmake --preset terminal
cmake --build --preset terminal
```

The executable is `work/build-terminal-only/src/86Box`, including on macOS; it is
not an application bundle. OpenAL remains enabled for floppy audio. The preset
disables optional external synthesizers, MIDI, Discord, VNC, GUI rendering
integrations, tests, and benchmarks, without changing the GUI presets. Native
libraries outside the system search paths can be supplied through
`CMAKE_PREFIX_PATH` and `PKG_CONFIG_PATH`; no Qt or Homebrew-specific paths are
required by this preset.

On macOS with Homebrew, select OpenAL Soft rather than Apple's legacy OpenAL
framework:

```sh
cmake --preset terminal -DOpenAL_ROOT="$(brew --prefix openal-soft)"
```

This native audio dependency remains necessary without Qt. When changing an
existing build's OpenAL selection, clear its cached discovery results with
`-U 'OPENAL_*'` on the configure command.

For output-only text presentation, set `TIGT_PRESENTATION=glass` to stream
glass-TTY output to stdout (including redirected output), `adaptive` for one-way
full-screen fallback, or `adaptive-reversible` to return after a clear followed
by representable text. Adaptive modes require stdout to be a TTY, use the normal
screen rather than the alternate screen, and clip to smaller host windows.
These modes do not read stdin or enable raw input. Unset the variable for the
existing interactive curses frontend. An unrepresentable glass-TTY update is
confirmed for 100 ms before the frontend reports failure; adaptive modes instead
fall back to full-screen presentation. Use `--logfile` to keep emulator logs
separate from the guest stdout stream.

Licensing
---------

86Box is released under the [GNU General Public License, version 2](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html) or later. For more information, see the `COPYING` file in the root of the repository.

The emulator can also optionally make use of [munt](https://github.com/munt/munt), [FluidSynth](https://www.fluidsynth.org/), [Ghostscript](https://www.ghostscript.com/) and [Discord Game SDK](https://discord.com/developers/docs/game-sdk/sdk-starter-guide), which are distributed under their respective licenses.

Donations
---------

We do not charge you for the emulator but donations are still welcome:
<https://paypal.me/86Box>.
You can also support the project on Patreon:
<https://www.patreon.com/86box>.

Acknowledgments
---------------

### Powered by
[![JetBrains logo.](https://resources.jetbrains.com/storage/products/company/brand/logos/jetbrains.svg)](https://jb.gg/OpenSource)
