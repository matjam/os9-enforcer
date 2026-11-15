# Enforcer

Enforcer adds three things to Mac OS 9.2.2 on NewWorld Macs:

- **Crash containment.** When an application writes into another
  application's memory, Enforcer blocks the write and stops the
  application that made it. The other applications and the system keep
  running, and you do not need to restart.
- **Preemption.** An application that computes for a long time without
  giving up the processor is made to share it.
- **Memory on demand.** The sizes in an application's Get Info window
  stop being a hard limit. An application gets memory as it uses it.

Enforcer has been tested only on QEMU's `mac99` machine, not on real
hardware.

## Requirements

- A NewWorld Mac (the first iMac and every Mac after it) running Mac OS
  9.2.2. OldWorld Macs are not supported.
- The Mac OS ROM file from Mac OS 9.2.2 (Mac OS ROM 9.2.1, NanoKernel
  2.28). Enforcer Installer checks the file and refuses any other.
- Virtual memory can be on or off.

## Install

1. Open Enforcer Installer and click Install.
2. Restart.

Keep Enforcer Installer, Enforcer Extension and the Enforcer control
panel in one folder: the installer copies the other two from its own
folder.

The installer patches the kernel in your Mac OS ROM file, moves the
original file to the folder "Enforcer Backup" at the top of the startup
disk, and copies the extension and the control panel into the System
Folder.

To update, open the new Enforcer Installer and click Install.

## Remove

1. Open Enforcer Installer and click Remove.
2. Restart.

The installer puts the original Mac OS ROM file back and moves Enforcer's
files to the Trash.

If the Mac does not start up after installing, start up from a Mac OS 9
CD and drag "Mac OS ROM" from "Enforcer Backup" into the System Folder,
replacing the patched one.

## Use

To change what Enforcer does, open the Enforcer control panel.

| Setting | What it does |
|---|---|
| Enforce (the default) | Blocks writes into other applications' memory and stops the application that made them. |
| Log only | Records the writes and allows them. |
| Off | Memory works as without Enforcer. |
| Also log writes into the system heap | Records applications' writes into the system heap. Enforcer never blocks those: applications and extensions write there legitimately. |
| Give applications more memory than Get Info sets (on by default) | Turns memory on demand on. To have the Get Info sizes apply as without Enforcer, turn it off; applications already running keep what they have. |

When Enforcer stops an application, a notification names the
application, the one whose memory it tried to write into, and the
address. When the write came from code that cannot be stopped safely
(interrupt-time code, for example), Enforcer blocks the write and lets
the application run, and the notification says so.

To stop an application that does not respond, use Force Quit
(Command-Option-Escape), as without Enforcer.

If the Mac OS ROM file is not patched (for example after reinstalling
Mac OS), the extension does not start and the control panel says so. To
fix it, open Enforcer Installer and click Install.

## What each feature does

### Crash containment

The patched kernel keeps an owner for each 4 KB page of memory: the
system, or one application. Each application runs with every other
application's pages read-only. A write by an application's code into
another application's page is denied, the target's memory is not
changed, and the application that wrote is stopped as if it had quit.

Temporary memory is protected in the same way: each application's
temporary memory comes from blocks that the application owns.

### Preemption

An application whose own PowerPC code runs for 20 ms without calling
`WaitNextEvent` is made to call it, when that is safe. Other applications
get time, and the application then goes on where it was. Preemption is on
with Enforce and Log only.

### Memory on demand

- An application that asks for 640 KB or more is launched with a 29 MB
  partition. The partition uses RAM only where the application touches
  it, and gives the RAM back when the application quits.
- A smaller application is launched with up to four times its preferred
  size.
- When an application's partition is full, a new block comes from
  temporary memory that the application owns.

## Limits

Crash containment:
- An overrun inside an application's own memory is not caught. It can
  only damage that application.
- Writes into the system heap are logged, not blocked.
- Writes by device DMA do not go through the processor's memory
  protection and are not seen.
- For a short time around an application's launch, quit, or the first
  application switch after startup, a write into another application can
  go uncaught.
- With virtual memory on, code in an application's own shared libraries
  and plug-ins counts as the system's.
- A program that calls the patched kernel on purpose can turn the check
  off. A stray write cannot.

Preemption:
- Only PowerPC code is preempted. A 68k application runs until it calls
  `WaitNextEvent` itself.
- An application inside a long Toolbox call, or in a routine the Toolbox
  called back, is not preempted.
- Bringing another application to the front waits until the preempted
  one calls `WaitNextEvent` itself.
- `WaitNextEvent` can move memory. An application that keeps a pointer
  into an unlocked handle across a long computation can find the block
  moved after it was preempted.

Memory on demand:
- With virtual memory on, the RAM behind a 29 MB partition is not paged
  out to disk.
- About This Computer, `FreeMem` and `MaxMem` report the partition's
  size, not the RAM in use behind it.
- Memory an application frees inside its partition goes back to the Mac
  only when the application quits.
- If an application touches new memory faster than Enforcer can supply
  it and the supply runs out, the application fails as on a Mac without
  enough memory.
- At most 16 applications get a 29 MB partition at once. The Finder and
  applications launched during startup keep ordinary partitions.
- The setting is one for every application: on or off.

Compatibility:
- Programs that write-protect pages themselves (debuggers, garbage
  collectors) do not see faults on pages Enforcer opens for them.
- A large temporary handle can fail to grow with `SetHandleSize` where
  Mac OS would have grown it.

`CLAUDE.md` has the full list of known holes and how each part works.

## Building

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

To try Enforcer by hand in QEMU, set up the test machine as `CLAUDE.md`
describes (it needs a Mac OS 9.2.2 install CD), then run:

```bash
./try.sh
```

To try it with virtual memory on, run:

```bash
./try.sh --vm
```

GitHub Actions builds every push and keeps `dist/` as the artifact
"Enforcer". `CLAUDE.md` describes the source layout, the test machine,
the tests and the kernel work.

## License

Enforcer is under the MIT License; see `LICENSE`. Retro68, QEMU and the
reference sources in `refs/` have their own licenses.
