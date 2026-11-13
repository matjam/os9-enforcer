# os9-protect

Test machine and toolchain for adding crash containment to Mac OS 9.2.2: a
crashed or hung application should not take the rest of the system down.

The Mac software built here, Enforcer with its protection engine, write
hooks and test programs, is in its own repository, `os9-enforcer`, next to
this folder (`~/src/os9-enforcer`). Its `build.sh` uses this folder's
toolchain, and its README shows how to run its programs on this machine.

## Test machine

QEMU's `mac99` machine (PowerPC G4, 512 MB) running Mac OS 9.2.2. It boots the
real Mac OS ROM file and NanoKernel from the guest's System Folder, so a
modified NanoKernel can be tested by replacing that file.

Requirements: `brew install qemu hfsutils`, and the universal 9.2.2 install
image linked at `media/macos-922-uni.iso`.

To install Mac OS 9.2.2 on a fresh disk, run `./run.sh install`. In the guest,
open Utilities > Drive Setup, initialize the 8 GB disk as Mac OS Extended, then
run Install Mac OS 9.2.2 onto it. Shut the guest down when the installer
finishes.

To boot the installed system, run `./run.sh`. The CD drive starts empty; to
start with a disc in it, set `CD=path/to/image`.

To use the mouse in the guest, click inside the window; QEMU captures the
pointer. To release it, press Ctrl+Option+G. To change the zoom, drag the
window edge: with the local QEMU build the window snaps to whole-number zoom
levels and starts at the largest that fits the screen. View > Integer Scaling
turns this off. To change the guest's own resolution, use the Monitors control
panel.

To restart the guest, use Special > Restart as usual. QEMU's mac99 firmware
hangs on a black screen after a guest restart, so `run.sh` makes QEMU exit on
a restart and starts it again from the hard disk. The window closes and
reopens.

To debug the guest with gdb, run `./run.sh debug`. QEMU starts paused with a
gdb stub on `localhost:1234`.

To run without a window, set `HEADLESS=1`. To throw away every disk write when
the machine stops, set `SCRATCH=1`.

To boot another disk image, set `DISK=path/to/image` (default
`disks/os9.qcow2`). `run.sh` changes to this folder first, so the path is
relative to it.

### Disk images

- `disks/os9.qcow2`: the installed system.
- `disks/os9-novm.qcow2`: an overlay on `os9.qcow2` with virtual memory
  turned off in the Memory control panel. The write hooks and the
  protection engine need virtual memory off, so that no page is ever paged
  out under a hook.
- `disks/os9-enforcer.qcow2`: an overlay on `os9-novm.qcow2` with Enforcer
  installed, for tests that must keep their changes across a restart.

An overlay records only its own changes, so changing the image under it
corrupts the overlay. To make `os9-novm.qcow2` again:

```bash
qemu-img create -f qcow2 -b os9.qcow2 -F qcow2 disks/os9-novm.qcow2
```

Then boot it with `DISK=disks/os9-novm.qcow2 ./run.sh`, turn Virtual Memory
off in the Memory control panel and restart.

To make `os9-enforcer.qcow2` again (which removes Enforcer and everything
else done on it):

```bash
qemu-img create -f qcow2 -b os9-novm.qcow2 -F qcow2 disks/os9-enforcer.qcow2
```

## Networking

The guest has a Sun GEM network card (the G4's built-in Ethernet, which Mac OS
9.2.2 drives without extra software) on QEMU's user-mode network. TCP/IP is set
to Ethernet, Using DHCP Server, and gets 10.0.2.15. The guest reaches the
internet through the host, and reaches the host itself at 10.0.2.2.

## Moving files into the guest

To copy files from the host into the running guest, run
`tools/push.sh [-n "Volume Name"] file...`. It builds an HFS disc image with
the files and inserts it into the guest's CD drive, where it mounts on the
desktop. `.bin` files are decoded as MacBinary (resource forks, type and
creator kept), `.hqx` as BinHex, `.txt` as text with Mac line endings, and
anything else is copied as raw data. File paths are relative to the current
folder, so it works from another folder too.

Mac OS locks the CD tray while a disc is mounted and does not notice a disc
changed underneath it, so `push.sh` first ejects a disc it pushed earlier from
inside the guest: it clicks the desktop near the bottom left corner, selects the
disc by typing its name, and presses Cmd-E. If a file on that disc is open, the
eject fails and `push.sh` says so.

## Controlling the guest

`tools/qmp.py` talks to the running machine over QMP:

```
tools/qmp.py screenshot shot.png
tools/qmp.py pos                 # cursor position, guest pixels
tools/qmp.py click 22 9          # Apple menu
tools/qmp.py dclick X Y
tools/qmp.py drag X1 Y1 X2 Y2
tools/qmp.py key cmd-q
tools/qmp.py type 'Hello'
tools/qmp.py hmp 'info registers'
tools/qmp.py quit
```

The guest mouse is a relative USB mouse and Mac OS accelerates it. To place the
cursor exactly, `move` (and the commands built on it) reads the cursor position
from the guest's low memory (`RawMouse`, 0x82C), moves by the remaining
distance, and repeats until it matches. A move takes about two seconds.

To open an icon, double-click it with `dclick`, aimed at a solid part of the
icon. The Finder hit-tests the icon's mask, so a click in the hole of a CD
icon lands on the desktop and deselects everything.

To switch applications, press Cmd-Tab (`tools/qmp.py key cmd-tab`).

To save ranges of the guest's logical memory from one consistent moment, use
`tools/vdump.py ADDR LENGTH OUT.bin ...`. It pauses the guest and saves only
while the CPU uses the blue task's translation. os9-enforcer's host scripts
read the guest through it.

## Snapshots

`clean-install` is Mac OS 9.2.2 straight after the installer, before its
first boot and the Setup Assistant.

To save the disk state while the machine is off, run
`qemu-img snapshot -c <name> disks/os9.qcow2`.

To return to a saved state, run `qemu-img snapshot -a <name> disks/os9.qcow2`.

To list saved states, run `qemu-img snapshot -l disks/os9.qcow2`.

## Local QEMU build

`run.sh` uses `qemu/build/qemu-system-ppc` when it exists, otherwise Homebrew's
`qemu-system-ppc`. The local build is QEMU 11.1.3 with
`qemu/patches/0001-cocoa-zoom-integer.patch`, which adds the Cocoa
`zoom-integer=on|off` option.

The repository holds only the patch, not QEMU's source. To build it:

```bash
qemu/build.sh
```

The script downloads the QEMU 11.1.3 release tarball (141 MB, kept as
`qemu/qemu-11.1.3.tar.xz`) and checks its SHA-256. It unpacks it into
`qemu/src` as a git repository whose one commit is the unmodified release, and
applies each `qemu/patches/*.patch` in name order. It then configures
`qemu/build` (`--target-list=ppc-softmmu --enable-cocoa --disable-docs
--disable-werror`) and builds it at `nice -n 19`. Running it again skips
patches that are already applied and only rebuilds what changed.

To change the patch, run `qemu/build.sh src` (fetch and patch only), edit the
files in `qemu/src`, then regenerate the patch:

```bash
git -C qemu/src diff > qemu/patches/0001-cocoa-zoom-integer.patch
```

## Toolchain

Retro68 (GCC 16 for classic 68k and PowerPC Mac OS) is built in
`toolchain/build/toolchain`, using the open-source Multiversal Interfaces
(Apple's Universal Interfaces are not installed, so Open Transport,
Navigation Services and other post-System 7 APIs have no headers).

To build a CMake project for 68k, configure it with
`-DCMAKE_TOOLCHAIN_FILE=toolchain/build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake`.
For PowerPC, use
`toolchain/build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake`.
Each application builds as a `.bin` (MacBinary) that `tools/push.sh` can copy
into the guest. Retro68's samples are already built in
`toolchain/build/build-target/Samples` (68k) and
`toolchain/build/build-target-ppc/Samples` (PowerPC).

To rebuild the toolchain, run:

```
mkdir -p toolchain/build && cd toolchain/build
nice -n 19 ../Retro68/build-toolchain.bash --no-carbon --ninja
```

Dependencies: `brew install boost cmake gmp mpfr libmpc bison texinfo`.

A Retro68 CMake project must declare both C and CXX (`project(Name C CXX)`),
because `add_application` links with the C++ driver.

## Reference sources

These clones are not in this repository (`.gitignore` excludes `refs/` and
`toolchain/`). To recreate them:

```bash
git clone https://github.com/elliotnunn/NanoKernel.git refs/NanoKernel
```

```bash
git clone https://git.applefritter.com/Macintosh-SW/powermac-rom.git refs/powermac-rom
```

```bash
git clone --recursive https://github.com/autc04/Retro68.git toolchain/Retro68
```

- `refs/NanoKernel` (commit b2b5613): Elliot Nunn's reconstruction of the
  v1 NanoKernel.
- `refs/powermac-rom` (commit 4de6504): the v2 NanoKernel sources, in
  `NanoKernel/NK*.s`.
- `toolchain/Retro68` (commit a4a389c41): to build it, run
  `../Retro68/build-toolchain.bash --no-carbon --ninja` from
  `toolchain/build`.

## Store measurement

`tools/storewatch/` is a QEMU TCG plugin that counts every store the guest
makes into low memory (below 0x2800) and the system heap (0x2800 to
SysZone's end). For each store it records the code that made it: the native
PC, or for 68k code the 68k instruction after the store (as Enforcer's 68k
hook does). It marks a store "app" when that code lies in the current
application's partition, which it reads from the running engine's 68k hook
data, the same test as Enforcer's. It needs the local QEMU build.

To build it and measure an idle and a busy phase:

```bash
tools/storewatch/build.sh
SCRATCH=1 DISK=disks/os9-enforcer.qcow2 PLUGIN=tools/storewatch/build/libstorewatch.dylib,out=run/sw ./run.sh &
tools/storewatch/scenario.sh run/sw
tools/storewatch/report.py run/sw-use
```

The plugin runs on every store, so the guest slows down in proportion to
how many stores it makes: about 16 million instructions a second at the
desktop, against hundreds of millions in loops with few stores. To measure
something else, create `run/sw.reset` to clear the counts and `run/sw.dump`
to write them (see `storewatch.c`).

Results on 9.2.2 (2026-11-12, Enforcer running; the busy phase opens a
window, uses the Calculator and Stickies, and switches applications):

| | Idle | Busy |
|---|---|---|
| Instructions | 334 million | 1,610 million |
| Low memory stores per million instructions | 682 | 752 |
| System heap stores per million instructions | 5,614 | 3,758 |
| Low memory stores by app code | 0 | 16 |
| System heap stores by app code | 0 (see below) | 0 (see below) |
| Stores into the 68k vectors (0000-00FF) | 0 | 0 |

- System code writes low memory all the time: `MemErr` and `TheZone` on
  every Memory Manager call, the current port, `ResErr`, `CrsrVis`, the
  File Manager and VBL queues.
- The only low memory global written by app code was `ResReadOnly`
  (0A5C), by a 68k application.
- Every system heap store marked "app" (5,466 idle, 12,369 busy) came from
  one emulator instruction, 6806E34C. The emulator saves state into a
  544-byte frame, from a table near 0x46000, when 68k code calls native
  code (most traps). r24 still points at the application's trap instruction
  then, so the 68k-PC test counts the emulator's store as the application's.
  The plugin now counts stores by that emulator code (6806E300-6806E370) as
  system stores, and so does Enforcer's 68k hook.
- Nothing wrote the 68k exception vectors, where a write through a NULL
  pointer with a small offset lands.

What this means for protecting low memory and the system heap:
- Trapping every store is too slow: that would be hundreds to thousands of
  traps per million instructions. Low memory's first page holds both the
  vectors and the most written globals, so it cannot be protected alone.
- Applications' own code almost never writes either area, so a rule like
  Enforcer's (deny app code) would rarely stop a working application.

### Protecting the system heap page by page

Enforcer does not trap every store into another application's memory: it
protects each page, lets the first store by system code open it (one
trap), and protects it again at its next 16 ms tick. The plugin's `epoch=`
option replays that on every system heap page from 0x2000 up, and counts
the traps it would take and how many pages would be open at a time:

```bash
SCRATCH=1 DISK=disks/os9-enforcer.qcow2 PLUGIN=tools/storewatch/build/libstorewatch.dylib,out=run/pm,epoch=ticks:250000:1000000:4000000:16000000 ./run.sh &
tools/storewatch/scenario.sh run/pm
tools/storewatch/report.py run/pm-use
```

`ticks` re-protects at each tick of the guest's clock (1/60 s), the closest
to Enforcer's timer. The plugin slows the guest, so for work that keeps the
CPU busy each tick holds less work than it would; an interval of a fixed
number of instructions covers that case. Without the plugin, this guest
runs 13 to 25 million instructions a second at the desktop and up to about
900 million when busy (`mode=insns`), so 16 ms is 0.2 to 14 million
instructions.

One trap costs about 70 µs on this QEMU (Apple Silicon host), including
Enforcer putting the protection back; os9-enforcer's README ("What a trap
costs") describes the measurement.

Results (2026-11-12, same idle and busy phases as above, 5,300 system heap
pages; "open for good" leaves the pages with the most traps unprotected):

| Open for good | Traps per tick, idle / busy | CPU at 70 µs a trap | Pages protected |
|---|---|---|---|
| none | 51 / 45 | about 20% | 99.4% |
| 64 pages | 1.6 / 6.3 | 0.7% / 2.6% | 98.7% |
| 128 pages | 0.1 / 2.3 | 0.1% / 1.0% | 97.5% |
| 256 pages | 0 / 0.8 | 0.3% | 95.2% |

For a busy CPU with 16 million instructions per interval (16 ms at about
1,000 million a second), the busy phase takes 124 traps per interval with
no pages open for good (about 54% CPU), 69 with 64 (30%), 44 with 128
(19%) and 22 with 256 (10%).

- The pages with the most traps are the same in both phases and are
  written in nearly every tick: the emulator's call frames (0x44000 to
  0x46000) and a few dozen others.
- No application's code stored into the system heap in these phases, so a
  deny rule stopped nothing here; that is a small sample of applications.
- A protected share of 98.7% means a store to a random system heap
  address would be caught that often. A stale pointer into a structure the
  system uses all the time is more likely to hit one of the open pages.

So protecting the system heap the way Enforcer protects applications costs
about 20% of the CPU on this QEMU if every page is protected again at every
tick, and 1 to 3% in interactive use if the few dozen pages the system
writes all the time are left open, at the price of those pages. Busy work
costs more, because more pages get written in each tick.

Trying it showed a harder limit: a protected page holding a variable that
Mac OS updates with `lwarx`/`stwcx.` hangs the machine, because the
NanoKernel retries the faulting `stwcx.` itself and no write hook ever sees
the fault (see "Watching the system heap" in os9-enforcer's README).
