# Developing Enforcer

This file says how to build Enforcer and run it. `CLAUDE.md` is the full
development guide: the source layout, the test machine and how to drive
it, the tests, how each part works, the kernel work, and the known holes.
`CONTRIBUTING.md` says how to report a bug and send a fix.

## Build

Enforcer builds with Retro68's PowerPC toolchain. The build needs no
Apple files.

To install the packages the toolchain needs on macOS, run:

```bash
brew install boost cmake ninja gmp mpfr libmpc bison texinfo
```

On Debian or Ubuntu, run:

```bash
sudo apt-get install cmake ninja-build g++ libgmp-dev libmpfr-dev libmpc-dev libboost-all-dev bison flex texinfo ruby python3
```

To build the toolchain, run this once. It installs Retro68 in
`~/.local/opt/retro68`:

```bash
toolchain/build.sh
```

To use a Retro68 toolchain you already have, set `RETRO68` to its folder
(the one that holds `bin/`).

To build the programs, run:

```bash
./build.sh
```

To collect Enforcer Installer, the Enforcer Extension and the Enforcer
control panel in `dist/`, run:

```bash
tools/make-dist.sh
```

`dist/` holds each program as a MacBinary `.bin` file, and all three on
the HFS disk image `Enforcer.dsk`, for an emulator.

To make the file a release carries (`dist/Enforcer-VERSION.sit.hqx`),
run:

```bash
tools/make-release.py
```

GitHub Actions builds every push and keeps `dist/` as the artifact
"Enforcer".

## Versions

`version.txt` holds the version of the last release; release-please
changes it. Each build puts its version in the programs: in a `vers`
resource (the Finder's Get Info) and in the title bars of the control
panel and the installer. A build of a release commit has the plain
version (`0.1.0`). Any other build adds the commit (`0.1.0+1a2b3c4`), so
a report says exactly which code ran.

To have a change listed in the next release, start its commit subject
with `fix:` or `feat:` (`CLAUDE.md`, "Conventions" and "Releases").

## Run it in QEMU

The test machine is Mac OS 9.2.2 on QEMU's `mac99`. Setting it up needs a
Mac OS 9.2.2 install CD image, which is not in this repository;
`CLAUDE.md` ("Setup" and "Test machine") has the steps.

To install the patched QEMU the test machine uses, run:

```bash
qemu/build.sh
```

To start your own copy of the test machine with Enforcer installed, run:

```bash
./try.sh
```

To start it with virtual memory on, run:

```bash
./try.sh --vm
```

## Test a change

- Test with virtual memory on and with it off. Bugs here have shown up
  in only one of the two.
- Scribble is the test program (`CLAUDE.md`, "Scribble"): each key runs
  one test, and most print PASS or FAIL. ScribbleBig is the same program
  with a size that gets it an on-demand partition.
- After a change to the kernel patch (`kernel/nanokernel.s`), run:

```bash
tools/make-kpatch.py && tools/rompatch-test.sh && ./build.sh installer
```

- To read Enforcer's state and its log of writes from the running guest,
  run:

```bash
tools/xlog-read.py
```
