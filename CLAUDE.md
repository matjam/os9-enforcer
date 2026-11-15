# os9-enforcer: development notes

`README.md` describes Enforcer for its users. Keep it short. This file
holds what you need to build, test and change Enforcer, and what was
learned on the way.

## Conventions

- Everything Enforcer needs is in this repository: the test machine
  (`run.sh`, `disks/`, the tools that drive the guest), the reference
  sources (submodules in `refs/`), and the scripts that install Retro68
  and the patched QEMU in `~/.local/opt/retro68` and
  `~/.local/opt/qemu-os9` (`toolchain/build.sh`, `qemu/build.sh`). Nothing
  here is for another project, and other projects keep their own.
- Commit each working step to `main`. The remote, `origin`, is the
  private GitHub repository matjam/os9-enforcer.
- Commit only our own sources, scripts and patches. Apple's files stay out
  of git: `kernel/stock/` and `kernel/build/` are ignored, and the
  installer carries only the bytes that differ from the stock kernel
  (`installer/kpatch.h`).
- Write docs instruction first ("To X, use Y"), in plain technical prose.
- When a hole is fixed or found, update both the Known holes list here and
  the Limits section of `README.md`.

## Setup

To set up a new clone on macOS, install the packages:

```bash
brew install boost cmake ninja gmp mpfr libmpc bison texinfo hfsutils glib pixman libslirp pkg-config
```

Install the toolchain and QEMU, and make tbxi's Python environment. The
install scripts fetch the source they build, and do nothing when the
install is already there:

```bash
toolchain/build.sh
```

```bash
qemu/build.sh
```

```bash
tools/tbxi-setup.sh
```

To fetch the kernel reference sources, run:

```bash
git submodule update --init refs/NanoKernel refs/powermac-rom
```

The disk images and the Mac OS 9.2.2 install image are not in git. To make
the disks, see "Disks" below.

## Build

To build every program, run `./build.sh`. To build some, name their
directories: `./build.sh enforcer scribble`. Each builds a MacBinary `.bin`
in its `build/` folder. The `enforcer/` build also needs Python 3.

`common/hooks.cmake` (`hooks_embed`) assembles the write hooks into each
engine program and links DriverServicesLib.

### Toolchain

The toolchain is Retro68's PowerPC toolchain (GCC 16), with the
open-source Multiversal Interfaces: Apple's Universal Interfaces are not
installed, so Open Transport, Navigation Services and other post-System 7
APIs have no headers. `toolchain/build.sh` pins the Retro68 commit
(a4a389c41), fetches that commit and its multiversal submodule into
`toolchain/Retro68`, builds in `toolchain/build`, PowerPC only, and
installs in `~/.local/opt/retro68` (or `$RETRO68`). Both folders are
git-ignored and can be removed after the install. `toolchain/find.sh`
gives the install folder to `build.sh`, `kernel/build.sh`,
`kernel/extract.sh` and `tools/make-dist.sh`. A fixed install folder
matters: Retro68 writes its absolute path into the toolchain's files.

A finished install records the commit in `REVISION`, and
`toolchain/build.sh` does nothing when it is the pinned one;
`toolchain/build.sh src` only fetches. To move to another commit, change
`commit` in the script, empty `~/.local/opt/retro68`, and run it.
`./build.sh` starts a program's `build/` folder again when its CMake cache
names another toolchain (`RETRO68_ROOT`), since the cache keeps the tools
it found first.

To build a CMake project with it, configure it with
`-DCMAKE_TOOLCHAIN_FILE=$HOME/.local/opt/retro68/powerpc-apple-macos/cmake/retroppc.toolchain.cmake`.
A Retro68 project must declare both C and CXX (`project(Name C CXX)`),
because `add_application` links with the C++ driver.

Toolchains built separately do not build byte-identical programs; our own
code comes out the same, the C library linked in does not:
- `toolchain/build.sh` on macOS, against a toolchain built by hand from
  the same commit: the extension and the control panel match. In the
  installer, newlib's `dtoa` read-only data is 64 bytes longer, which
  moves the data after it. Against the build in `~/.local/opt` (Retro68
  a4a389c41 again, 2026-11-14), 113 bytes of the installer's newlib
  `dtoa` and `mprec` read-only data differ, at the same length.
- GitHub Actions (Linux): the resource forks match. Code differs only in
  newlib's stdio and `_exit` at the end of each code section.
Both builds were installed in the guest: the installer recognized the
installed kernel, and the CI-built extension registered its control block
and stopped Scribble's `n` store.

The first CI run took 24 minutes to build the toolchain; the cached
toolchain is 354 MB.

`tools/make-dist.sh` puts the three programs users install, and an HFS
image holding them, in `dist/`. `.github/workflows/build.yml` builds the
toolchain, every program and `dist/` on each push, and keeps `dist/` as
the artifact "Enforcer". It runs `toolchain/build.sh` and caches
`~/.local/opt/retro68` under the hash of the script, which holds the
pinned commit; changing the script rebuilds it.

### QEMU

`qemu/build.sh` pins the QEMU release (v11.1.3), clones its tag shallow
into `qemu/src`, and applies each `qemu/patches/*.patch` to the checkout.
It configures `qemu/build` for the install folder
(`--target-list=ppc-softmmu --enable-cocoa --enable-plugins --disable-docs
--disable-werror`), builds it at `nice -n 19`, and installs it in
`~/.local/opt/qemu-os9` (or `$QEMU_PREFIX`). `qemu/src` and `qemu/build`
are git-ignored. Running it again skips patches already applied and
rebuilds only what changed; it configures again when the install folder
changes (`qemu/build/.prefix`). The install records the release and the
patches' SHA-1 in `REVISION`; with `qemu/build` gone and `REVISION`
matching, the script does nothing.

`run.sh` uses `~/.local/opt/qemu-os9/bin/qemu-system-ppc` when it exists,
otherwise Homebrew's `qemu-system-ppc`. The patch,
`0001-cocoa-zoom-integer.patch`, adds the Cocoa `zoom-integer=on|off`
option. To change it, run `qemu/build.sh src` (fetch and patch only), edit
the files in `qemu/src`, then regenerate it:

```bash
git -C qemu/src diff > qemu/patches/0001-cocoa-zoom-integer.patch
```

To move to another QEMU release, change `version` in `qemu/build.sh`, run
`qemu/build.sh src` to check that the patches apply, and run
`qemu/build.sh`.

### Reference sources

The submodules under `refs/` are for reading; nothing builds from them
but tbxi.
- `refs/NanoKernel` (b2b5613): Elliot Nunn's reconstruction of the v1
  NanoKernel.
- `refs/powermac-rom` (4de6504): Elliot Nunn's reconstruction of the v2
  NanoKernel (version 2.28, the one in this 9.2.2 install), in
  `NanoKernel/NK*.s`. It is disassembled source with many routines still
  unnamed, not Apple's original.
- `refs/tbxi` (69839ea): Elliot Nunn's tool that unpacks a Mac OS ROM file
  into editable parts (`tbxi dump`) and packs it again (`tbxi build`).
  `tools/tbxi-setup.sh` installs it in `refs/tbxi-venv`, with
  `macresources` from PyPI; `kernel/extract.sh`, `kernel/pack.sh` and
  `tools/rompatch-test.sh` use it.

tbxi's setup leaves out its C compressor when it fails to build, and then
packs a ROM file in pure Python, which takes minutes instead of a second.
With a pyenv Python the link fails (the linker cannot read the SDK's
library stubs), so `tbxi-setup.sh` compiles it with Xcode's clang when it
is missing. tbxi's BinHex output (`tbxi build -o NAME.hqx`) runs for
minutes without finishing even then; `kernel/pack.sh` builds the data fork
only and copies the resource fork from the stock file.

## Test machine

QEMU's `mac99` machine (PowerPC G4, 512 MB) running Mac OS 9.2.2. It
boots the real Mac OS ROM file and NanoKernel from the guest's System
Folder, so a modified kernel is tested by replacing that file.

To boot a disk, run:

```bash
DISK=disks/os9-vmoff.qcow2 ./run.sh &
```

To test with more RAM, set `MEM` (megabytes, default 512; Mac OS 9 uses
up to 1536). `xlog-read.py` cannot read the engine's state with virtual
memory on; use the control panel there.

`DISK` defaults to `disks/os9.qcow2`; a name ending in `.qcow2` opens as
qcow2, any other as raw, relative to the repository's top folder.

To try Enforcer by hand, run `./try.sh` (`--vm` for virtual memory on,
`--fresh` to start over). It boots your own copy of `os9-vmoff.qcow2` (or
`os9-pool.qcow2`), made on first use as a whole image in `disks/try.qcow2`
(`try-vm.qcow2`), so tests that change the test disks do not touch it.
Its QMP socket is `run/try.sock` (`run/try-vm.sock`), so it runs beside a
test machine; to drive it, set `QMP_SOCK=run/try.sock` for `tools/qmp.py`,
`tools/push.sh` and the tools that read the guest. `run.sh` takes
`QMP_SOCK` too, and `push.sh` keeps its files per socket.

`run.sh` starts QEMU again from the hard disk whenever the guest restarts:
QEMU's mac99 firmware hangs on a black screen after a guest restart, so
QEMU exits on a restart instead, and the window closes and reopens.
`SCRATCH=1` discards disk writes, so it also loses an install at the
installer's restart; to keep one, use an overlay disk instead.
`HEADLESS=1` runs without a window, and `CD=IMAGE` starts with a disc in
the CD drive (it is empty otherwise). To debug the guest with gdb, run
`./run.sh debug`: QEMU starts paused with a gdb stub on `localhost:1234`.

To use the mouse in the guest, click inside the window; QEMU captures the
pointer. To release it, press Ctrl+Option+G. The window snaps to
whole-number zoom levels and starts at the largest that fits the screen;
View > Integer Scaling turns this off. To change the guest's resolution,
use the Monitors control panel.

The guest has a Sun GEM network card (the G4's built-in Ethernet) on
QEMU's user-mode network. TCP/IP is set to Ethernet with DHCP and gets
10.0.2.15; the guest reaches the internet through the host, and the host
itself at 10.0.2.2.

### Disks

| Disk (in `disks/`) | What it is |
|---|---|
| `os9.qcow2` | The base install of Mac OS 9.2.2 |
| `os9-novm.qcow2` | Overlay on `os9.qcow2`, virtual memory off |
| `os9-enforcer.qcow2` | Overlay on `os9-novm.qcow2`, an early Enforcer install (the store measurements below) |
| `os9-installer.qcow2` | Overlay on `os9-novm.qcow2`, stock ROM file, for installer tests |
| `os9-pool.qcow2` | Overlay on `os9-installer.qcow2`, virtual memory on (990 MB, 512 MB of RAM) |
| `os9-vmoff.qcow2` | Overlay on `os9-installer.qcow2`, virtual memory off |
| `os9-kernel.img` | Raw image for kernel builds (`kernel/install.sh`) |
| `os9-kernel-enforcer.img` | Copy of `os9-kernel.img` with the extension and panel |

`media/macos-922-uni.iso` links to the universal Mac OS 9.2.2 install
image. To install Mac OS 9.2.2 on a fresh disk, run `./run.sh install`.
In the guest, open Utilities > Drive Setup, initialize the 8 GB disk as
Mac OS Extended, run Install Mac OS 9.2.2 onto it, and shut the guest
down when the installer finishes.

An overlay records only its own changes, so changing the image under it
corrupts the overlay. To make an overlay again, create it on its base
(the base's name is relative to the overlay's folder), then boot it and
set it up; for `os9-novm.qcow2`, turn Virtual Memory off in the Memory
control panel and restart. `qemu-img` is in the `bin/` of the QEMU
install, or Homebrew's:

```bash
qemu-img create -f qcow2 -b os9.qcow2 -F qcow2 disks/os9-novm.qcow2
```

`os9-kernel.img` is raw so that macOS can attach it and write the ROM file
into its System Folder while the machine is off; it takes about 330 MB on
disk (8 GB sparse). To make it again, with the stock Mac OS ROM file:

```bash
qemu-img convert -O raw disks/os9-novm.qcow2 disks/os9-kernel.img
```

`os9.qcow2` holds the snapshot `clean-install`: Mac OS 9.2.2 straight
after the installer, before its first boot and the Setup Assistant. To
save, return to or list snapshots while the machine is off, use
`qemu-img snapshot -c NAME`, `-a NAME` or `-l` on the image. Returning
`os9.qcow2` to a snapshot changes the image under every overlay.

### Driving the guest

To copy files into the running guest, use `tools/push.sh [-n "Volume
Name"] FILE...`. It builds an HFS image with the files and inserts it as
the CD. `.bin` files are decoded as MacBinary, `.hqx` as BinHex, `.txt` as
text with Mac line endings, and anything else is copied as raw data:

```bash
tools/push.sh -n Test enforcer/build/"Enforcer Extension.bin" scribble/build/Scribble.bin
```

Mac OS locks the CD tray while a disc is mounted, so `push.sh` first
ejects a disc it pushed earlier from inside the guest: it clicks the
desktop near the bottom left corner, types the disc's name and presses
Command-E. If a file on that disc is open, the eject fails and `push.sh`
says so.

To drive the guest, use `tools/qmp.py`: `screenshot FILE`, `pos`,
`click X Y`, `dclick X Y`, `drag X1 Y1 X2 Y2`, `key NAME [MS]`,
`type TEXT`, `hmp CMD`, `raw JSON`, `quit`. The guest mouse is a relative
USB mouse that Mac OS accelerates, so `move` (and the commands built on
it) reads the cursor position from low memory (`RawMouse`, 0x82C), moves
by the distance left, and repeats until it matches; a move takes about
two seconds. Aim `dclick` at a solid part of an icon: the Finder
hit-tests the icon's mask. To switch applications, use `key cmd-tab`.

To save ranges of logical memory from one consistent moment, use
`tools/vdump.py ADDR LENGTH OUT.bin ...`. It pauses the guest and saves
only while the CPU uses the blue task's translation (see "Reading guest
memory").

To install or update Enforcer in the guest, push Enforcer Installer, the
Enforcer Extension and Enforcer (the control panel) together, open the
installer, click Install, then Restart. The Finder cannot replace a running
extension; the installer moves it to the Trash first.

To read the engine's state, counts, domains and offense log while it runs,
use `tools/xlog-read.py`.

### Scribble

Scribble (`scribble/`) makes the stores the engine has to judge. Type a
letter and Return. Each store writes the value already there, so an
allowed store changes nothing. The build makes it twice: Scribble asks
for 512 KB and keeps an ordinary partition (the overflow tests need a
small one); ScribbleBig asks for 1 MB and, with virtual memory off, is
launched into a sparse partition, where `o`, `Z`, `n` and `E` were run.

| Key | Store | Expected in enforce mode |
|---|---|---|
| `n` | native code into the Finder | denied, Scribble stopped cleanly |
| `6` | 68k code into the Finder | denied, Scribble stopped cleanly |
| `b` | `BlockMoveData` (system code) into the Finder | allowed |
| `o` | into its own heap | allowed |
| `s`, `S` | native, 68k into the system heap | logged, allowed |
| `t` | time 2 s of trapped writes | prints the cost of one trap |
| `L` | keep 64 MB of temporary memory, every page written | in a chunk of Scribble's |
| `R` | after `L`: time one write into each of its pages right after a switch back, then again | prints the cost of the faults |
| `F` | time a read of each page of the Finder's partition right after a switch back, then again | prints the PTEs rebuilt (application VSIDs) and their cost |
| `m`, `M` | 64 KB kept, 1 MB freed, of temporary memory | in Scribble's own chunks |
| `T` | native code into the Finder's temporary memory | denied, Scribble stopped |
| `P` | write and read back 600 MB of temporary memory | pages out other applications (VM on) |
| `Y` | an MP task into the Finder | dropped by the kernel, Scribble goes on |
| `V` | a Time Manager task into a background-only application, 5 s later; make the Finder current | dropped by the kernel |
| `D` | a deferred task into the Finder, Scribble current | skipped by the engine, Scribble goes on |
| `W` | native code into the Finder right after a switch back, while the engine still has another process current | denied, Scribble stopped cleanly |
| `K` | native and 68k stores into the control block and the owner table | dropped by the kernel (6 counted in `CB_TABLES`), Scribble goes on |
| `X` | make the Enforcer Extension call `ExitToShell` without `engine_stop` | engine stops itself, control block unregistered |
| `Z` | run 2.5 s twice in its own code without `WaitNextEvent` | preempted: gaps while other applications run, Process Manager switches, `WaitNextEvent` calls by the stub; the two sums equal |
| `C` | the same loop in a callback Mixed Mode calls from native code | preempted (`CallUniversalProc` from PowerPC to PowerPC leaves no frame: nothing is in progress) |
| `U` | the same loop in a callback called from 68k code | not preempted: every check "chain not ending at the base" |
| `H` | `U` for 20 s; press Command-Option-Escape during it | Force Quit comes up, and Cancel or Force Quit works before the 20 s end |
| `G` | `TempNewHandle` in system mode (`BeginSystemMode`) | PASS: the handle is the Process Manager's, not in a chunk |
| `A` | draw an Appearance scroll bar with a thumb for 3 s; first in a fresh boot, then again after `q`, a launch and `m` | the first passes 1 request on in system mode (the theme's thumb image); the second thumb is the same as the first |
| `O` | 6 MB of handles and 2 MB of pointers in 256 KB blocks (the file asks for a 1 MB partition), written, read back and disposed | PASS: every block, those the partition cannot hold in chunks, the chunks freed after disposing |
| `J` | sparse memory (kernel patch 7, VM off; once a boot: it leaves 450 KB in the system heap): a 64 MB area with a pool of 64 pages, 48 pages touched, freed, one touched again | PASS: each page zero at first touch and holding its data, the pool's counts right, the area deleted. SKIP when the engine has the sparse range |
| `I` | I/O into sparse memory: a 64 KB read of its own file into touched, untouched and locked pages of a block in a sparse chunk | PASS: three reads without error, the same bytes |
| `E` | in ScribbleBig (a sparse partition): a 1 MB handle grown to 20 MB with `SetHandleSize`, written and read back, and a 4 MB pointer | PASS: no error, in the partition, no fault with the pool empty. SKIP in an ordinary partition |

`Y`, `V` and `D` flip the word, read it back and restore it, and print
whether the store landed.

XLog, Guard, SysLog and SysGuard (`xlog/`) run the engine in a console
window: log, deny, log the system heap, deny with the system heap logged.

## Design

### The kernel's owner check (`kernel/nanokernel.s`, `common/kcb.h`)

- Patch 1, `stwcx.` on a protected page: the kernel runs a faulting store
  itself, and for `stwcx.` that store failed without a fault (the
  reservation is gone), so the caller's loop retried forever. When the
  kernel's `stwcx.` fails, it now stores the word's current value back
  under a new reservation, which faults on a read-only page and reaches
  the blue task.
- Patch 2: `MPSetAreaAccess` skips the CPU plugin's pages when a call would
  remove write access. The plugin (interrupt routing, caches, sleep) lives
  in the system heap and runs in SIGP mode, where a write fault makes the
  kernel abandon the call (`IntReturnFromSIGP`).
- With no control block registered, every store faults as in the stock
  kernel.
- Patch 3, in pass mode (the engine is off or turning off): a store by a
  task other than the blue task or the CPU plugin into a page the engine
  owns sets the page's PTE writable and runs the store again (in
  `MemRetryDSI`), and is counted in `task_log`; a page with no owner (the
  ROM image, another program's protection) faults as in the stock kernel.
  The PLE stays read-only, so the page is protected again when its PTE
  leaves the HTAB or `MPSetAreaAccess` runs.
- Patch 4, the owner check: the engine registers a control block (16 KB,
  physically contiguous) and an owner table (one byte per 4 KB page, up to
  128 table pages: 2 GB) with MP call 109, which the stock kernel leaves
  unused (it returns −4 there). The kernel takes only `CB_VERSION` 5 and
  returns paramErr for another, so a mismatched extension and kernel
  protect nothing. Owner bytes: 0 none, 1 the system heap, 2 the control
  block and the owner table, 3 + slot for an application's domain. An
  application's byte with `OWNER_CODE` (0x80, `kOwnerCode`) marks a page
  where its code lies but which is not its memory (the partly used first
  page of a partition, code outside it): the kernel's `code_of` gives the
  application, for a store's code and for `ApplZone`, and `owner_of`
  gives 0, so the page is nobody's as a target.
- The control block and the owner table are read-only to Mac OS. The
  engine gives their pages owner 2 and write-protects them, and the
  kernel drops every store into a page with owner 2, in any mode, and
  counts it (`CB_TABLES`). The engine changes them through MP call 109
  with an operation in r3 instead of a block address: `OP_OWNERS` (a run
  of owner bytes, optionally only where the byte is a given value),
  `OP_MODE` and `OP_CURRENT` (`kcb_owners` and `kcb_set` in
  `common/kcb.h`). `OP_OWNERS` with "any byte" leaves owner 2 alone, so
  marking a range of the system heap keeps the tables protected. The MP
  call reaches the arguments past r5 in the caller's context block (r6 at
  +0x134, r7 at +0x13C).
- On a store to a read-only page that has an owner, in any task, the kernel
  looks up the owner of the code doing the store: the PC, or for the 68k
  emulator the 68k PC (r24 − 2). Code nobody owns is the system's. A store
  by an application's code into a page that is not its own is an offense.
  In enforce mode (`task_write`):
  - into the current application's page: allowed and logged (it can be a
    callback the current application called);
  - by the current application in the blue task, with the 68k interrupt
    mask at 0: denied. The denied store reaches the hooks, which deny it
    too and stop the application. The kernel records the last denial of
    each kind (`CB_DENY_NATIVE`, `CB_DENY_68K`: the page and the event's
    number); `HookData.kdeny` points the hook at its kind's pair, and the
    hook denies a store to that page once for each number (`kseen`),
    since it cannot write the block;
  - any other: dropped. The kernel moves SRR0 past its own emulated store
    and returns to its emulation; memory is not changed and the writer
    goes on (`kEvfSkipped`). `stwcx_probe`'s store is allowed instead, or
    the caller's `lwarx`/`stwcx.` loop would never end.
- The interrupt mask is in the emulator's r25 (bits 29-31): the task's own
  r25 when the emulator faulted, or the one saved in the emulator's
  context block (KDP + 0x658, then + 0x1CC) when native code did. Stopping
  an application from a Time Manager task leaves the Time Manager's
  dispatch unfinished: no timer runs again and the machine hangs.
- Every allowed store makes the page's PTE writable, runs the store again
  and records it in the block's ring (488 entries). The engine's timer
  takes the ring every 16 ms and protects the page again.
- Patch 5, application VSIDs (`OP_SPACES`, `OP_SWITCH`, `OP_FLUSH`,
  `OP_PATCH`). Every
  application runs in the blue task, in the system address space. With
  `OP_SPACES` on, that space's segment registers for the segments in
  `vsid_mask` hold the current application's own VSIDs: segment << 20 |
  0xFFF00 + its owner byte (`space_switch` writes them into the
  `AddressSpace` and loads them; the scheduler loads them from there after
  another space ran). Segments 6 and up (kernel, emulator, I/O, ROM) keep
  the system's.
  - One PTE per page, as the kernel's VM and area code need: at
    `PagingFunc1`'s "PLE already in the HTAB" exits, `vsid_fix` drops a PTE
    built for another VSID (`InvalPTE` keeps R and C in the PLE) and lets
    it build one for this VSID (`CB_REBUILT`).
  - At PTE build, `pp_owner` sets PP 11 when another application owns the
    page; otherwise PP comes from the PLE. So an application's own pages
    are writable in its VSIDs and its PTEs stay valid across a switch.
  - `task_write` takes a store into a page read-only by owner
    (`ro_by_owner`) like one into a write-protected page. At a blue-task
    store whose segment's VSID is not `ApplZone`'s application, it loads
    the right VSIDs and runs the store again (`CB_LAZY`).
  - A denied store into a page read-only only by owner gets a read-only
    PLE: with virtual memory on, the 68k emulator asks the VM Manager about
    a write fault first, and it retries a store into a page whose PLE is
    writable, forever (Scribble `6` hung). The engine opens the PLE again
    when it takes the denied event.
  - `OP_OWNERS` drops the PTE of each page whose byte changes, and
    `OP_SPACES` drops every application page's PTE, so pages are built
    again for their new owner.
  - The lazy switch records its store's PC in `CB_LAZY_PC`.
  - `OP_PATCH` (`kcb_patch`) replaces one word of code through its
    physical address: `phys_of` finds the page's PTE under the HTAB lock,
    the old word must match, and the kernel writes the new one, then
    `dcbst`/`icbi`. The write goes around the PTE, so the page's changed
    bit stays clear and the Virtual Memory Manager never writes a
    file-mapped page back with it. The page must have a PTE and should be
    held, or a page read in again loses the change.

### The engine (`common/engine.c`, `common/engine.h`)

- One domain per application: its partition's whole pages, its temporary
  memory chunks, and with virtual memory on its main code section.
- Application VSIDs (kernel patch 5) protect the domains: `mark` adds the
  segments of each partition and chunk to `vsid_mask` (`spaces_cover`,
  segments 0-5), and `engine_set_mode` turns them on with protection
  (`spaces_set`). Nothing is protected page by page in those segments
  (`by_vsid`): after a store the kernel allowed, the engine drops the
  page's PTE (`OP_FLUSH`) instead of protecting it. A range past segment 5
  is protected page by page as before.
- The VSID switch: a hook in the Process Manager's native context switch
  (`pm_hook`), at the `blr` after it loads the next process's registers
  and stack (B in `engine.c`; `ApplZone` is already set), branches to a
  stub in the system heap that calls `OP_SWITCH` with `ApplZone`. The
  low-memory reload (S, after the `ApplZone` store) is the fallback site;
  there the Process Manager still stores into the previous process's
  stack, about two allowed faults a switch. Both are found by instruction
  pattern in the system heap. Launches and quits set `ApplZone` outside
  the context switch, so a few switches stay lazy with the hook (14 of
  553 in a VM-off boot). The timer's `check_switch` calls `OP_SWITCH`
  too, as a backstop.
- The VSID switch with virtual memory on: the Process Manager's code is
  file-mapped above logical RAM, not in the system heap, and its
  container exports nothing. Until a hook is in, the kernel switches
  lazily at the first store that faults, which is the Process Manager's
  store into the new process's stack, and records its PC. The timer
  then searches for B within 0x24000 bytes of that PC (`pm_hook_mapped`)
  and writes `b` (or `ba`) to the stub there with `OP_PATCH`, on a held
  page. The search reads only pages in the PC's kernel area that a
  `HoldMemory` probe can bring in (`page_readable`): a large file mapping
  has unbacked pages, and reading one stopped the extension with a bus
  error. The stub is allocated at start whether or not a site was found,
  so it is in branch reach. The engine searches again from each new lazy
  PC, at most 8 times. `pm_unhook` puts the `blr` back with `OP_PATCH`;
  if that fails, it leaves the stub in place.
- Current is the Process Manager's current process (`ApplZone`), the one
  whose code runs in the blue task between `WaitNextEvent` calls, in the
  foreground or the background.
  - The kernel reads `ApplZone` at each store it decides, through its
    physical address in `CB_APPLZONE` (`applzone_physical`; with virtual
    memory on, `GetPhysical` needs low memory locked), and stores the
    owner of the page it points into in `CB_CURRENT`.
  - The engine reads it on each tick (`check_switch`) to move the hooks'
    current range (and, for ranges not covered by VSIDs, to protect the
    previous application's pages). While `ApplZone` is outside that range,
    the hooks count nothing as the current application's, and the engine
    decides from each event's zone (`event_app_code`).
- The span is 2 GB (`kSpanHi`): with 1.5 GB of RAM the Process Manager
  places applications at about 0x5E000000, from the top of RAM down. The
  owner table covers logical RAM, plus 256 MB with virtual memory on.
- The partly used first page of a partition, where the heap and often the
  code start, gets the application's owner byte with `kOwnerCode`, so it
  stays unprotected (`own_first_page`): the Process Manager's block before
  the partition shares the page, and under VSIDs a plain owner byte made
  its stores fault at every switch. Without the byte, code there was the
  system's. Code owned outside the partition (`own_code`) gets the same.
- The Enforcer Extension runs only with the owner check registered; with a
  stock or other-release kernel it stops the engine and publishes
  `kEnforcerNoKernel` or `kEnforcerOtherKernel` for the control panel.
- Off mode turns the temporary memory pools off (`PoolStubs.enabled`).
- Crash cleanup: the program running the engine patches `_ExitToShell`
  for itself with `NSetTrapAddress` (`self_exit`), so the Process Manager
  installs it whenever that program is current, and `engine_stop` runs
  when it quits without stopping the engine. The pool stub's own check
  (`quit_hook`) is not enough: right after a switch the trap table can
  hold another routine until the timer's next tick.
- Write hooks (`common/hnat.s`, `common/h68k.s`) sit in front of the
  Exception Manager's entry in the native context block and the 68k
  emulator's handler. They take write violations on managed pages, record
  them in a ring, and deny a store by the current application's code at or
  above `deny_from`. A fallback Exception Manager handler takes the rest.
- Clean stop: for a denied native store, the fault handler resumes the
  application at `ExitToShell`, in its own context. For 68k code, a stub in
  front of the bus error vector calls `_ExitToShell` when the frame's
  interrupt mask is 0.
- Above task level (`CurrentExecutionLevel` is not `kTaskLevel`: a deferred
  task, a secondary interrupt handler), the fault handler skips a denied
  native store instead (`skip_store`): PC + 4, a `stwcx.` reports success,
  and the store is logged as blocked. Deferred tasks run with the 68k
  interrupt mask at 0, so the kernel cannot tell them apart.
- Offenses: `kOffenseStopped`, `kOffenseBlocked` (dropped or skipped),
  `kOffenseAllowed` (log mode). The extension notifies for each stop, and
  for a blocked writer and target at most once a minute.
- Temporary memory pools: the engine's stub on `_OSDispatch` selector
  0x1D (`TempNewHandle`) allocates in a chunk of the calling
  application's own: temporary memory from the Process Manager, moved high,
  locked, with a zone on the whole pages inside it after a 16-byte
  signature. The first 128 KB chunk is kept; any other is freed with its
  last handle. When no chunk can be made, the request goes on to the
  Process Manager and the block has no owner.
- System mode: system software makes temporary memory that must outlive
  the current application between `BeginSystemMode` and `EndSystemMode`
  (`_OSDispatch` selectors 0x40 and 0x41, not in InterfaceLib; the
  selectors are in `MFPrivate.p`). The Process Manager then puts the
  handle on its system list, not the current process's
  (`p_systemModeLevel` in `c_TempNewHandle`, ProcessMgr `Memory.c` in the
  System 7.1 source), and an application's quit leaves it alone. The stub
  asks `GetSystemClientProcess` (selector 0x3F) first: when it names the
  system (PSN 0.1), the request goes on to the Process Manager
  (`PoolCounters.system`). Before, it went into the current application's
  chunk and was freed at its quit (issue 1: the Appearance theme's cached
  scroll bar thumb, a temporary-memory GWorld). A boot makes about 40 such
  requests with virtual memory on and about 120 with it off.
- Overflow: stubs on `_NewHandle` and `_NewPtr` call the trap's routine;
  when it fails (after the application's grow zone procedure) with
  `TheZone` equal to `ApplZone` and no system-heap bit in the trap word,
  `pool_over` allocates the block in one of the application's chunks. A
  new chunk for it is at least 1 MB (`kOverChunk`) and is made only while
  2 MB of temporary memory stays free (`kOverReserve`). The stub on
  `_DisposePtr`, like the one on `_DisposeHandle`, finds a chunk's block
  by the zone in its header and frees the chunk with its last block. The
  engine's own allocation in a chunk's zone has `TheZone` on the chunk,
  so a failure there does not come back in. Scribble `O` tests it.
- Launch sizes: the Process Manager reads a `SIZE` resource in one
  routine, which copies 10 bytes (flags, preferred size, minimum size)
  into a record and adds the default stack (low memory 0x322) less 8 KB.
  `size_hook` replaces the `mr r3,r29` at its exit with a branch to a stub
  that raises the preferred size to 4 times what the file asks, capped at
  `size_cap`: a quarter of the Process Manager zone's `zcbFree` (an
  eighth with virtual memory off, where the partition reserves RAM), which
  `size_follow` sets at task level (0 with protection off). The site is
  found by its instructions, in the system heap (virtual memory off) or in
  the file-mapped code around the switch hook's site (`pm_hook_mapped`,
  patched with `OP_PATCH`). `xlog-read.py` prints a `launch sizes:` line.
- An owner must never outlive its memory: a stale owner can stop an
  application for writing its own new memory. The engine drops a domain
  and its chunks at `ExitToShell`, and a sweep drops any domain or chunk
  whose master pointer no longer points at it. A dropped slot keeps its
  name, partition and code range until reused (`Domain.dropped`), so a
  store taken from the ring after the quit still names the application.
- New applications: a `jGNEFilter` stub adds an application as a domain at
  its first event. Background-only applications get only null events,
  which do not reach the filter; the refresh (every 30 ticks) adds them.
- Virtual memory on: the Code Fragment Manager keeps an application's code
  section outside its partition (file mapped above logical RAM, or in a
  Process Manager heap block). `find_code` reads the `cfrg` and the PEF
  container, searches the partition for main's transition vector, checks
  the candidate (alignment, location, the code bytes read under
  `HoldMemory`), and gives those pages the application's owner byte in the
  owner table only. The owner table then covers the whole span. The
  control block is locked contiguous, the owner table locked, and the
  extension's partition and the hooks' blocks held.

### Preemption (kernel patch 6, `common/hyield.s`)

Mac OS switches applications only when the current one calls
`WaitNextEvent`, `GetNextEvent` or `EventAvail`. With preemption, the
current application's own native code that runs past a slice (20 ms,
`kSliceMs`) without a switch is made to call `WaitNextEvent(0, &e, 0,
nil)` when that is safe, and then goes on where it was.

- The redirect (kernel, `preempt`, at 0x146D0 in `NKScheduler.s`, "TRIGGER
  AN INTERRUPT IN THE BLUE TASK"): to run a 68k interrupt while the blue
  task runs native code, the scheduler moves it from its alternate context
  (the NCB, which holds the native registers: PC at +0xFC, MSR at +0xA4)
  to the emulator's. When the slice is over (`CB_NEXT`), the code is in
  user mode, the emulator's 68k interrupt mask (ECB r25) is 0, and the PC
  is in the current application's own code (`code_of` against
  `ApplZone`), the patch puts the stub's entry in the NCB's PC and keeps
  the PC in `CB_YIELD_PC`. After the interrupt the emulator resumes the
  native code, in the stub. One redirect is pending at a time.
- A slice starts when `ApplZone` changes (`slice_new`, called from
  `OP_SWITCH` and the lazy switch), and at `OP_YIELD`. The application
  `ApplZone` leaves gets its `CB_YIELDED` byte (one per owner): it has
  called `WaitNextEvent` itself, so its Toolbox is set up. `OP_FRESH`
  clears the byte for a new domain.
- The stub (`common/hyield.s`, in the system heap): moves r1 below the
  interrupted code's 224-byte red zone, aligns it to 64, saves r0, r2-r31,
  LR, CTR, CR, XER, f0-f13, FPSCR, and with AltiVec (the engine makes the
  branch over it a nop) VRSAVE, v0-v19 and VSCR when VRSAVE is not 0. It
  takes the PC with `OP_YIELD`, calls the engine's `yield_check` through
  its TVector, calls `WaitNextEvent` when the check says so, restores, and
  returns with `OP_RESUME`.
- `OP_RESUME`: jumping to an arbitrary PC with every register intact
  needs LR or CTR, which are the application's, so the kernel does it.
  The stub restores everything but r0, r1, r3, r4, LR, CTR, CR and XER and
  passes a 64-byte-aligned register block on its frame; the kernel finds
  it through `phys_of`, puts PC, LR and CR in r10, r12 and r13 (which
  `SchExitInterrupt` loads before its `rfi`), r0 in the ContextBlock, r1
  in the EWA, CTR and XER in place, and returns through `MPReturn`. It
  needs no control block, so a stub that comes back from `WaitNextEvent`
  after the engine stopped still returns. The stub is never freed.
- `yield_check` (engine, in the preempted application's context): task
  level (`CurrentExecutionLevel`; deferred tasks run with mask 0); the
  application's `CB_YIELDED` byte set; r1 between `ApplLimit` and
  `CurStackBase` with 32 KB free; and the stack's back chain ends at its
  base (0 or past `CurStackBase`) with return addresses that are the
  application's until the first that is not and none after it. A Toolbox
  routine that called back into the application leaves the application's
  frames below its own; the launch frames are the system's with none of
  the application's below. When the check says no, the next try is 2 ms
  later (`kYieldRetryMs`).
- Preemption is on with protection (`preempt_set` in `engine_set_mode`),
  off in Off mode and before `engine_stop`.

Measured (2026-11-14, QEMU, Scribble `Z`, 5 s of Scribble's own code that
never calls `WaitNextEvent`): with VM off, 188-192 redirects, all safe,
each a `WaitNextEvent` call, 48-52 Process Manager switches (none
without preemption), gaps up to 6 ms in Scribble's loop; with VM on, 226
calls and 68 switches. The sums of the two runs were equal every time.

Findings:
- GCC uses LR as a scratch register in a function that has saved it
  (Scribble's `spin` keeps the timebase in it), so the LR says nothing
  about the caller.
- A Mixed Mode switch frame, where 68k code called native code, ends the
  native back chain with a link that has bit 0 set (Scribble `U`: the
  chain ended at 0x1EDFE431). The 68k frames past it are not on the
  chain, so the check cannot see who called.
- `CallUniversalProc` from PowerPC code to a PowerPC routine leaves no
  frame: the callback's caller is the caller of `CallUniversalProc`.
- The redirects at startup were in applications that launch, compute and
  quit before they call `WaitNextEvent` (the `starting` count).
- A foreground switch waits for the preempted application: Command-Tab
  during Scribble `Z` (VM on) brought the Finder to the front only when
  `Z` ended. The Process Manager completes a major switch after the
  front application takes its suspend event, and the stub's
  `WaitNextEvent` has an empty mask.
- Force Quit is the hang recovery, and it works with Enforcer. To send
  Command-Option-Escape, use `qmp.py key cmd-opt-esc 600`: the keys must
  be held about 600 ms (100 ms is missed).
  - During a preempted hog (Scribble `Z`) the dialog comes up from the
    stub's `WaitNextEvent`; Force Quit stopped Scribble and the engine
    dropped its domain.
  - During a hog inside a 68k callback (`U`, `H`), which the stub never
    lets call `WaitNextEvent`, the Process Manager runs the dialog by
    itself, with Enforcer on or off: it is up within a second, and its
    buttons work. Under QEMU it is slow: the hog runs 5 to 6.5 times
    slower while the dialog is up, a click takes seconds to register,
    and the guest's PC samples are in the 68k emulator in the hog's
    context. How the Process Manager runs it there was not found.
  - A version that made the stub call `WaitNextEvent` (mask mouse-down
    and mouse-up) for 3 s after Command-Option-Escape, even inside a
    callback, made the dialog quick; it was taken out (89322d9 reverted),
    since the system's own path works and those calls let other
    applications run while a Toolbox call is in progress. With mask 0
    the dialog was drawn but took no clicks.
  - The dialog is not in the application's `WindowList` (low memory
    0x9D6): it is in another layer.

### Sparse memory (kernel patch 7)

A range of addresses whose pages get memory when they are first touched,
with virtual memory off. It is the kernel half of "Memory on demand"
(Roadmap); nothing in the engine uses it yet, and Scribble `J` tests it.

What the stock kernel has (read in `refs/powermac-rom/NanoKernel`,
`NKAddressSpaces.s`, `NKPaging.s`, `NKInterrupts.s`):
- `MPCreateArea` (MP call 72: r3 space or 0, r4 access, r5 length, r6
  separation, r7 flags, r8 base; returns r8 base, r9 area) makes an area
  with one page list entry (PLE) a page and no memory. Flags 0x600C place
  it exactly at the base with 4 KB pages; 0x2000 at or above; access 6 is
  read and write. Its PLE and fault-counter arrays come from the kernel's
  free page list when they are over 4 KB: about pages/1024 + pages/2048
  + 2 pages.
- The kernel's free page list is empty after startup on the test machine
  (`MPGetFreePageCount`, call 100, returned 0). `MPPutFreePage` (84, r3 a
  physical page) adds a page and checks nothing, so give it only pages
  Mac OS will never use again.
- `MPMapPage` (85) puts a page in a PLE; `MPUnmapPages` (86) takes it
  out; `MPDeleteArea` (74) needs nothing mapped. No path clears a page or
  maps on demand: `PagingFunc1` returns "page fault" for a PLE with no
  page, and for the blue task that goes to Mac OS.
- An area is looked up before the segment's own map, so an area above
  logical RAM takes the faults there. 0x40000000 was free on the 512 MB
  machine.

The patch:
- At `PagingFunc1`'s "no page" exit for a paged area, when the address is
  between `sparse_lo` and `sparse_hi`, `sparse_fill` takes a page from the
  pool, clears it, sets the PLE as `MPMapPage` does, adds to the area's
  `BytesMapped`, and runs the PLE test again, so the stock code builds the
  PTE and patch 5 gives it its owner's protection.
- The pool is a list through the first word of each free page, by
  physical address, under the HTAB lock (which `PagingFunc1`'s callers
  hold; the kernel's own free list needs the pool lock, taken first
  everywhere else).
- With the pool empty the fault goes on as in the stock kernel, and is
  counted. The caller has to keep the pool ahead of what can be touched.
- Operations of MP call 109 (`kcb.h`): `OP_SPARSE` (the range),
  `OP_POOL_PUT` (a physical page the caller owns), `OP_POOL_GET` (one
  back), `OP_SPARSE_FREE` (an address and a number of pages: their PTEs
  dropped, their pages back in the pool; returns how many), and
  `OP_SPARSE_INFO` (the pool's size, pages given, faults with it empty).
  A kernel without the patch returns paramErr, and the control block's
  version is unchanged.

Scribble `J` (2026-11-15, VM off): 48 system heap pages to the kernel's
free list, a 64 MB area at 0x40000000, a pool of 64 system heap pages.
48 pages touched 1.3 MB apart: all zero, all hold what was written, pool
16, no fault with the pool empty; 48 freed, pool 64; one touched again
reads zero; the pool taken back, the area deleted. Scribble `O`, `Z` and
`n` pass on this kernel.

### Findings that constrain the design

- Protection bits 11 make a page read-only for all code, the kernel
  included. Bits 01 do not stop the blue task's stores on 9.2.2.
- A 68k store to a protected page never reaches the Exception Manager: the
  emulator takes the fault.
- An exception handler in the 68k emulator's context runs with external
  interrupts enabled. The kernel tells the emulator about a pending
  interrupt with bits in the interrupted code's CR (cr2, mask 0x00E00000
  at KDP + 0x674). A hook that restores all of CR clears them, the
  interrupt is never run or ended, and the OpenPIC holds back every
  interrupt at its priority or below. The hooks restore only cr0 and cr1.
- The Process Manager, Window Manager and Layer Manager write into other
  applications' memory on every switch, so a rule must look at the code
  doing the store, not the target alone.
- On 9.2.2, MemTop (lowmem 0x108) ends a little above the system heap; size
  the owner table from Gestalt `'lram'`. Logical and physical addresses
  differ even with virtual memory off; translate each page with
  `GetPhysical`.
- The Process Manager keeps an application's `NSetTrapAddress` patches as
  its own and swaps them on every switch. The extension is an application,
  so the engine writes the trap table itself (OS traps at 0x400, Toolbox
  traps at 0xE00) and the timer puts its stubs back when a switch removes
  them. `_WaitNextEvent` and `_GetNextEvent` are patched per application
  the same way, which is why new applications are caught with
  `jGNEFilter`.
- Native code calls `TempNewHandle`, `HLock`, `HUnlock` and
  `DisposeHandle` through the trap table. The Process Manager's
  `TempHLock`, `TempHUnlock` and `TempDisposeHandle` work unchanged on a
  handle in a chunk's zone.
- Process Manager heap: partitions, temporary memory and its own blocks
  are handles in its zone. The master pointers are in one block near the
  top of the heap. Block headers are 12 bytes: tag word, physical size,
  then the relative handle (zone + relative handle is the master pointer).
  Each application's process entry (0x5A0 bytes) follows its partition.
- `GetDiskFragment` with `kFindLib` fails for an application's own
  fragment; `find_code` reads the file instead.
- With virtual memory on, the paging calls keep a page's read-only bit in
  its PLE. `setPTEntryGivenPage` (VM call 20) can lose a protect made
  between the Virtual Memory Manager's read and write of an entry, and
  `VMExchangePages` (only `LockMemoryContiguous`) swaps protection with the
  pages.
- One trap costs about 70 µs on the QEMU test machine, including putting
  the protection back.

### Known holes

Worst first. Keep in step with Limits in `README.md`.
1. A store by another application's code into the current application's
   page is allowed and logged (it can be a callback storing results).
2. A store by an application's 68k code in a deferred task, while the
   application is current, is denied and the application stopped from
   there: the 68k stub tests only the interrupt mask, and skipping a 68k
   store needs its instruction length.
3. A store the kernel allows opens the page until the next tick (16 ms),
   in the VSIDs it was made in: the application current then, and MP
   tasks, which share the blue task's segment registers.
4. The partly used first and last pages of a partition are not protected,
   and the last has no owner. The last holds the top of the application's
   A5 world.
5. With virtual memory on, code in an application's other fragments, and
   all of its code when `find_code` finds no match, is the system's.
6. An application launched since the last refresh (up to half a second)
   has no domain until its first event or `TempNewHandle`.
7. Any code can make MP call 109 itself: change owner bytes, change the
   mode or unregister the block. A stray store cannot change the tables
   (Scribble `K` tests that), but a program that calls the kernel on
   purpose can.
8. The system heap is log only.
9. An application's own patch of `_OSDispatch`, `DisposeHandle` or
   `ExitToShell`, made before the engine started, goes around the
   engine's stub while that application is current.
10. With virtual memory on, a protect can be lost to `setPTEntryGivenPage`
    until the engine protects the page again (pages not covered by VSIDs:
    the system heap, ranges past segment 5). For covered pages, the VM
    calls that insert a PTE themselves (`VMLastExportedFunc`,
    `VMSecondLastExportedFunc`, NKVMCalls.s 1822-1925) take PP from the
    PLE, so a page another application owns, paged in under the current
    VSIDs, is writable to the current application until its PTE is built
    again. Not tested.
11. With virtual memory on, the switch hook goes in only after the first
    lazy switch, whose PC the search needs (25 lazy switches in a VM-on
    boot before it was in). Until then, and at launches and quits, the
    VSIDs follow at the first store that faults after a switch, normally
    the Process Manager's store into the new process's stack, and a
    store by the new process into the previous application's memory
    before that is not caught. Ranges past segment 5 are protected page
    by page, with the old 16 ms window after a switch. (Who is current is
    right at once: the kernel reads `ApplZone` at each store. Scribble
    `W` tests that.)
12. `engine_stop` puts back the Exception Manager's system-wide handler
    without checking that it is still Enforcer's. A handler installed
    after Enforcer's is dropped.
13. A temporary handle in a chunk grows only inside the chunk:
    `SetHandleSize` can fail where the Process Manager's heap had room.
14. Crash cleanup is tested with Scribble `X` (a direct `ExitToShell`). A
    real crash of the extension, or a Force Quit, that does not reach
    `ExitToShell` through the trap table is not covered or tested.
15. Preemption covers native code only: 68k code, and native code under a
    68k caller (the chain ends at a switch frame), runs until it yields.
16. Preemption calls `WaitNextEvent`, which can move memory: an
    application that keeps a pointer into an unlocked handle across its
    own long loop can find the block moved. Not seen; not tested.
17. The yield stub puts its frame (about 1 KB) on the application's stack
    before the check looks at the room left, so an application within
    1 KB of its stack's limit overflows into its heap.
18. A long Toolbox call, or a callback the Toolbox makes (a `ModalDialog`
    filter), is never preempted: an application that never yields at a
    safe point holds the processor as before, until Force Quit. A
    preempted application gives background applications time, but a
    switch to another application in front waits until it yields
    itself.
19. Each engine start leaves one yield stub (about 1 KB) in the system
    heap.
20. Overflow serves new blocks only. Growing a block past the partition
    (`SetHandleSize`, `SetPtrSize`, `ReallocateHandle`, `PtrAndHand`,
    `Munger`) fails as before: a block cannot leave its zone. A sparse
    partition (virtual memory off, an application asking for 640 KB or
    more) moves that limit to 29 MB: Scribble `E` grows a 1 MB handle to
    20 MB there. With virtual memory on, and for smaller applications,
    the limit is the partition from launch sizes.
21. `FreeMem`, `MaxMem`, `PurgeSpace` and `GetProcessInformation` count
    only the partition. An application that asks for all it sees gets
    less than there is; About This Computer does not show an
    application's chunks.
22. An application that tests for memory by asking for a huge block and
    expects it to fail can now get it, down to the 2 MB reserve.
23. With virtual memory off, a partition raised at launch reserves RAM the
    application may never touch (up to an eighth of what was free). With
    Apple System Profiler, Sherlock 2, Scrapbook and Stickies running, the
    nine heaps held 30 MB, 81% of it free, against 10.6 MB without launch
    sizes.

Compatibility review (2026-11-13) also noted: programs that write-protect
their own pages lose those faults to the kernel and the hooks; the engine
leaks about 75 KB of system heap per start.

Later, as an opt-in debug mode for one application: a protected page after
each large block, to catch an overrun when it happens.

## Roadmap

Agreed on 2026-11-14. The aim is protection between applications first,
then preemption, without making the Toolbox reentrant: it stays
serialized, and applications run in parallel only outside it.

1. Kernel hardening. The kernel takes Enforcer's mechanism; the policy,
   and everything that needs the Toolbox (tracking processes, finding
   code, temporary memory, notifications, preferences), stays in the
   extension.
   - Done (2026-11-14): the owner table and the control block are
     read-only to Mac OS; the engine changes owners through kernel calls.
   - Done (2026-11-14, see "Switch cost" and "Per-application VSIDs:
     what the kernel has" below): measure what a switch costs, and scope
     per-application VSIDs in the kernel (building and removing
     page-table entries, address spaces).
     Today `protect_domain` re-protects the previous application's whole
     partition at each switch, and `MPSetAreaAccess` walks it page by
     page: the cost grows with the partition, and forced yields add
     switches. Measured (2026-11-14, see "Switch cost" below): the cost
     follows the pages an application writes between switches, about
     13 µs a page in QEMU, so a 64 MB working set costs about a quarter
     of a second at every switch.
   - Done (2026-11-14, kernel patch 5): per-application VSIDs. Each
     application gets its own segment IDs for the segments that hold
     application memory; the kernel builds a page-table entry writable
     when the owner table gives the page to that VSID's application,
     read-only otherwise. A switch loads the segment registers: no
     per-page work, no first-write faults on the application's own pages,
     nothing left open after a switch. Built on the stock kernel's
     binary, by rewriting the system space's segment registers rather
     than adding address spaces. Measured in "Switch cost" below.
   - Done (2026-11-14): a switch hook with virtual memory on. The
     Process Manager's code is file-mapped there; the kernel writes the
     branch through its physical address (`OP_PATCH`) on a held page, so
     the page stays clean to the Virtual Memory Manager.
2. Preemption at safe points, native code. On its tick the kernel checks
   whether the current application has run past its time slice with
   only its own code on the stack (no Toolbox call in progress), and if
   so resumes it in a stub that calls `WaitNextEvent` with an empty mask
   and then continues where it was. The Process Manager does the switch
   and still picks who runs next.
   - Done (2026-11-14, kernel patch 6, see "Preemption" above): the
     redirect at the blue task's 68k interrupt, the yield stub,
     `OP_RESUME`, the engine's check, and Scribble `Z`, `C` and `U`.
   - Toolbox calls that run their own event loop (`ModalDialog`) count
     as safe, from an allow-list checked in the guest.
   - Hang recovery: a clean quit at a safe point for an application that
     stops yielding.
   - A control panel setting and a per-application exclusion list.
   - 68k code later, if the emulator's state allows it.
3. Import interposition for native applications: wrappers on imports
   from system libraries. Exact "on behalf of" attribution for Toolbox
   writes, pointer checks, and the basis for an explicit Toolbox lock.
4. Research, after measuring how long applications stay inside Toolbox
   calls: an explicit lock, our own context switch, applications as
   scheduled tasks. The user has a plan for I/O waits, to discuss first.
5. Optional: owner bytes for third-party extensions, so writes by them
   are named and can be limited.
6. Memory on demand (agreed 2026-11-14, see "Memory on demand" below):
   an application's memory follows what it uses, and the Get Info sizes
   stop being a limit.

Real hardware testing matters more with each kernel change, a
dual-processor G4 above all.

### Memory on demand

The aim: an application gets "out of memory" only when the machine is
out of memory, not when it reaches the size typed into Get Info, and
memory it does not use is free for others.

A partition is three things today: a range of addresses, a reservation
of physical RAM made at launch, and a limit. The range has to stay: the
heap, the stack and the A5 world are in one zone at fixed addresses, and
all applications share one address space. The reservation and the limit
can go.

The design:
1. Sparse partitions. Each application gets a large range of addresses
   (bounded: applications share segments 0-5, about 1.5 GB), and a page
   gets physical memory when it is first touched. The heap grows inside
   the range; nothing moves.
2. Reclaim. Pages that lie wholly in free heap blocks go back to the
   pool and come back zero-filled. Compaction moves free space to the
   top of the zone.
3. Commit accounting, for the whole machine. The engine counts each
   application's bytes in use at the Memory Manager traps. A request
   fails only when the machine has no memory left after reclaiming:
   compacting zones, and purging purgeable handles of applications
   waiting in `WaitNextEvent` (not zones whose purge procedure is the
   application's code). Accounting at the request means a page fault
   never finds the pool empty.
4. `MaxMem`, `FreeMem` and the like report the application's share of
   what the machine has free, so an application that takes all it sees
   takes a sensible amount.
5. A guard page between the heap and the stack: a stack overflow faults
   instead of writing into the heap.
6. `GetProcessInformation` (`processSize`, `processFreeMem`) reports the
   pages backed and the bytes in use, so About This Computer shows what
   is used.

With virtual memory on, the Virtual Memory Manager already backs pages
on first touch: the work is the partition's size and place at launch (a
Process Manager hook), and 2 to 6; reclaim means telling the VM Manager
a page's contents are not needed. With virtual memory off, the kernel
needs a zero-fill pager and a page pool, on top of patch 5's PTE build.

Other designs, kept in reserve:
- Overflow chunks: when `NewHandle` or `NewPtr` fails in the
  application's zone, serve it from a chunk the application owns, as the
  temporary memory pools do. No pager; `RecoverHandle`, `SetHandleSize`
  and `FreeMem` do not see the chunk.
- Sizing at launch from the high-water mark of earlier runs.
- Compressing the pages of waiting applications, once there is a pager.
- Mapping code read-only from one copy with virtual memory off.
- A private address space for each application (the same addresses,
  other pages): removes the shared 1.5 GB, but system code reaches other
  applications' memory by address everywhere.

Measured on 2026-11-14 (VM off, 512 MB, Enforcer on):

- Free whole pages in running heaps (`tools/heap-pages.py`, which walks
  each application's zone in a physical dump). With the Finder, four
  background applications, Apple System Profiler, Sherlock 2, Scrapbook
  and Stickies running:

  | | Heap | Free | Whole free pages now |
  |---|---|---|---|
  | Apple System Profiler | 4,265 KB | 32% | 333 |
  | Sherlock 2 | 3,157 KB | 46% | 362 |
  | Scrapbook | 1,072 KB | 93% | 250 |
  | Finder | 958 KB | 49% | 108 |
  | all nine | 10,631 KB | 48% | 1,248 (4.9 MB) |

  Nearly all free space is already in whole pages (1,248 of the 1,286
  that one free block would give), so reclaim needs no compaction to get
  most of it: almost half of what partitions hold today.
- Launch does not touch the partition. Scribble with a 128 MB preferred
  size got a partition of 32,884 pages (0x16397000-0x1E40B000). The
  memwatch plugin on the RAM below 0x1E000000 (57,344 pages, all but the
  partition's top 1 MB) counted 223 pages touched from startup to
  Scribble at its prompt. So a large partition costs address space, not
  memory, and with virtual memory on a larger partition is sparse as it
  is.

Mac OS 9's heap layout, as read in the dump: the zone header is 0x30
bytes (`bkLim` at +0, `zcbFree` at +12); each block has a 16-byte header:
the previous block, flags, the size with the header, and the master
pointer's offset (relocatable) or the zone (nonrelocatable). The flags'
first byte is 0x6B nonrelocatable, 0xAB relocatable, 0x00 or 0x10 free;
the last block is a 12-byte nonrelocatable trailer.

Built so far (2026-11-14, see "The engine" for how):
- Launch sizes: an application gets up to 4 times its preferred size, no
  more than a quarter of the free memory (an eighth with virtual memory
  off). This is design point 1 inside
  what the Process Manager's heap can give: with virtual memory on the
  partition is sparse; with it off it reserves RAM. Scribble's 1,040 KB
  became 4,160 KB, VM off and on.
- Overflow chunks, the reserve design, for when the partition is full:
  Scribble `O` gets 8 MB in its partition, VM off and on. Apple System
  Profiler and Sherlock 2 ran with the stubs in and made no failed
  request.

Why not a partition that grows: the Process Manager allocates a partition
as one block of its heap, whose addresses are the logical memory (RAM, or
the virtual memory size). A range beyond that needs the partition placed
outside the Process Manager's heap, which means taking over launch.

Also built: `GetProcessInformation` adds an application's chunks to its
size and free memory (6; Scribble `O` checks it), and the control panel's
"Give applications more memory than Get Info sets" turns launch sizes and
overflow off (`State.want_mem`; off, Scribble `O` gets 3 of 32 blocks, as
without Enforcer).

Not built, and why:
- Reclaim (2). With virtual memory off it frees nothing usable: there is
  no pager to give a page to another address. With it on it would save
  page-outs of free blocks, through `ReleaseMemoryData` if 9.2.2's VM has
  it; not tried. It pays off with the kernel pager below.
- `MaxMem` and `FreeMem` (4): they still count the partition only, which
  errs on the safe side.
- The stack guard page (5). The page above `ApplLimit` is legal stack,
  and the one below is heap, so a guard needs `ApplLimit` lowered a page
  at launch; a store into it would then be denied like one into another
  application.
- Growing a block past the partition (hole 20), and RAM reserved by a
  larger partition with virtual memory off (hole 23). Both need the
  partition's range to exceed what backs it: a zero-fill pager in the
  kernel with a pool of pages, and the partition placed outside the
  Process Manager's heap.

The pager is built (2026-11-15, "Sparse memory (kernel patch 7)"), and
the engine uses it for overflow chunks and for partitions; see "Built"
below. Launch (`0x117E170` reads the sizes; the caller stores the
preferred size and calls one routine that makes the process, retrying
smaller on `memFullErr` down to the minimum) has the partition as a block
of the Process Manager's heap.

Traced so far (2026-11-15, a VM-off dump; addresses of that boot):
- The Process Manager's native fragment has its TOC at 0x010CC17C (206
  transition vectors in RAM name it).
- The routine launch calls to make the process (0x0117F4FC) is a thunk:
  `CallUniversalProc(*(0x103C), 942, 16, argument)`. Low memory 0x103C
  holds the 68k Process Manager's dispatcher, and selector 16 makes the
  process. So the partition is allocated in 68k code, not in the native
  fragment; its neighbours are selectors 28 (0x0117F538) and 4
  (0x0117F56C).
- The fragment calls the Resource Manager, `BlockMoveData` and the like
  through InterfaceLib transition vectors straight into the ROM
  (`Get1Resource` at 0xFFD13780), not through the trap table. A trap
  patch does not see those calls; that is why stubs on `_GetResource` and
  `_Get1Resource` saw no `SIZE` read at launch. The Memory Manager calls
  of native applications do go through the trap table (the overflow stubs
  see Scribble's).

- Correction: low memory 0x103C is the trap table's entry for
  `_OSDispatch` (0xE00 + 4 × 0x8F), not a 68k dispatcher. After the
  patches chained on it, the trap's routine is a selector-dispatched
  routine descriptor in the fragment's data (0x010CD3F8, 129 records of
  20 bytes: procInfo, ISA, flags, transition vector, selector), and every
  selector is native code in the fragment. Selector 16 is 0x011679A0, a
  wrapper around 0x0116ADAC, which makes the process; 0x3A
  (`GetProcessInformation`) is 0x011682A0 and 0x1D (`TempNewHandle`)
  0x0117C568. The whole Process Manager is native on 9.2.2.

Where the Process Manager uses the partition (read from the same dump;
offsets from the size hook's site Z, 0x0117E258 there):
- The process entry (1,416 bytes, its own handle) holds the partition's
  handle at +48, its address at +44, the size asked at +994 and the size
  reported at +72. The partition is allocated at Z − 0x14798 (P1: `bl` to
  the fragment's allocator, which makes a relocatable block high in the
  Process Manager's zone through `_HeapDispatch`), then locked.
- The zone is set up at the block's start (`InitZone`, 64 master
  pointers), the A5 world at its top and the stack under that. Nothing
  moves or resizes a partition while its application runs.
- The handle is used as a Memory Manager object in seven places: `HLock`
  at launch; `GetHandleSize` twice at quit, to take the application's
  entries out of the Notification and Time Manager queues; two more under
  virtual memory only; `DisposeHandle` at Z − 0x134DC (P2), at quit and
  after a failed launch; and, at quit, when the scrap lies in the dying
  heap, `SetHandleSize` and `HUnlock` to turn the partition's block into
  the scrap (the branch at Z − 0x8CD0, P3, chooses between that and
  copying the scrap to a new handle).
- `GetProcessInformation` reads +44 and +72 and asks `FreeMem` in the
  application's zone; it does not use the handle.

Built (2026-11-15), virtual memory off:
- The range and the pool (`sparse_start`, `pool_grow`, `pool_shrink`,
  `sparse_ensure`, `sparse_balance`): 0x20000000 to 0x40000000 on the
  512 MB machine (the next 256 MB above logical RAM, 512 MB long, inside
  segments 0-5), in 16 MB pieces. The area's page tables are fed to the
  kernel's free list from system heap pages (about 200 for this range).
  The owner table covers the range. The pool's pages are 1 MB blocks of
  temporary memory made in system mode (`BeginSystemMode`), locked. The
  engine keeps 4 MB of headroom plus what an allocation it sees needs,
  and gives whole blocks back at its poll. It is not a balloon over all
  free memory: other launches keep the memory they had.
- Overflow chunks in the range (`sparse_chunk`): one zone on whole
  pieces. Scribble `O`'s 16 overflow blocks share one.
- Sparse partitions (`part_hook`, `part_new`, `part_reap`): the size hook
  gives an application that asks for 640 KB or more a 29 MB preferred
  size; P1's stub asks `part_new`, which returns a handle the engine
  makes (a master pointer in the stubs' block, the partition a page into
  its pieces behind a Memory Manager header that `HLock` and
  `GetHandleSize` accept) for a request of 24 MB or more; P2's stub
  clears that master pointer instead of disposing, with no engine code,
  and `part_reap` returns the pieces; P3's stub takes the copy way for a
  partition in the range. The stubs stay if the engine stops while a
  sparse partition lives.
- `_MemoryDispatch` for the range (`pool_md`): with virtual memory off the
  ROM's `HoldMemory`, `LockMemory` and `GetPhysical` are 68k code that
  tests against low memory 0x1EF4 (the top of logical memory) and counts
  locks in a table of its pages, so a disk read into sparse memory, and
  with it every launch into a sparse partition, failed with ioErr (−36).
  The stub hands selectors 0 to 5 for an address in the range to the
  engine, which touches the pages and asks the kernel
  (`MPGetPhysicalAddress`, call 81) for their physical addresses. The
  disk driver does come through the trap.

Tested (VM off, 512 MB): Apple System Profiler, Sherlock 2 and Scrapbook
launched into sparse partitions at 0x20001000, 0x23001000 and 0x26001000
and ran, with 10 pool blocks (10 MB) behind the three; all quit, three
partitions given back, 13 of 14 pool blocks freed. Scribble `I` (a read
of 64 KB into touched, untouched and locked sparse pages) passes; `O`,
`Z` and `n` pass.

Left:
- A touch with the pool empty still faults as in the stock kernel. The
  headroom covers what the engine does not see (blocks the ROM allocates
  for itself without the trap table, the stack); an application that
  outruns it between polls is not handled.
- The engine tops the pool up at the traps it sees (`pool_top`): after a
  `NewHandle` or `NewPtr` of 16 KB or more in a sparse zone, and before
  `SetHandleSize`, `ReallocateHandle` and `SetPtrSize` to 16 KB or more
  while the current application's zone is sparse. Scribble `E` without
  the second group emptied the pool: the application quit with a type 2
  error, and the machine went on. Calls the ROM makes for itself past the
  trap table, smaller blocks and the stack come out of the headroom.
- Freed heap pages do not go back until the partition does (reclaim).
- `GetProcessInformation` reports the partition's 29 MB as its size, not
  the pages behind it, and a sparse chunk by its pieces.
- Launching with the pieces or the partition slots (16) used up falls
  back to the Process Manager's heap with the raised size.
- The Finder and the applications launched before the engine is up keep
  ordinary partitions.
- Not tested: an application that takes all the memory `MaxMem` reports;
  the scrap at quit (P3); a real Mac.

### Switch cost

Measured on 2026-11-14 in QEMU (TCG, so a real Mac's numbers differ, but
the shape holds). The engine times `protect_domain` at each switch
(`Counters.switch_tb`, `switch_tb_max`, `switch_pages`; `xlog-read.py`
prints a `switch:` line); Scribble `t`, `L` and `R` time the faults.

| | VM off | VM on |
|---|---|---|
| Switches a second, idle (background-only applications' null events) | 2.5 | 4.5 |
| Re-protecting at a switch, small applications only | 65 µs, 236 pages | 71 µs, 256 pages |
| Re-protecting, a page not written since the last switch | 0.08 µs | 0.08 µs |
| Re-protecting, a page written since the last switch | about 3-5 µs | about 3 µs |
| Longest switch, after Scribble wrote 64 MB | 78 ms | 48 ms |
| A write fault the kernel allows (`t`, with the engine's re-protect) | 16 µs | 45 µs |
| First write into each of 16,384 own pages after a switch back (`R`) | 171 ms (10 µs a page) | 182 ms (11 µs a page) |
| The same writes with no fault | 2.7 ms | 2.7 ms |
| First writes into own pages a switch, idle | 1.7 | 2.6 |

What it means:
- Idle with small applications the cost is about 0.1% of the time.
- The cost follows the pages an application writes between switches:
  each costs a fault after the switch in and a PTE change and `tlbie`
  at the switch out, about 13 µs together. An application that writes
  64 MB between `WaitNextEvent` calls pays about a quarter of a second
  at every switch. Preemption with 10-20 ms slices is not possible on
  this design.
- Per-application VSIDs remove both parts: a switch loads segment
  registers, the application's own page-table entries stay valid in its
  space, and nothing is re-protected.

With application VSIDs (kernel patch 5), measured the same way on
2026-11-14 (`xlog-read.py` prints the kernel's counts on its
`application VSIDs:` line):

| | VM off | VM on |
|---|---|---|
| First write into each of 16,384 own pages after a switch back (`R`) | 6.1 ms (0.2 µs a page) | 6.0-6.3 ms (0.1-0.2 µs a page) |
| The same writes again | 3.1 ms | 2.7-4.0 ms |
| The engine's work at a switch (`switch:` line) | 15-17 µs | 17-34 µs |
| VSID switches by the hook / made lazily at a store | 685 / 36 | 402 / 25 (none / 1,429 without the hook) |
| PTEs built again for another VSID, a switch | about 31 | about 73 |
| Stores the kernel allowed in a boot to the Finder | 10 | 12 |

The first writes after a switch cost TLB refills only: the
application's PTEs were still in the HTAB. The pages rebuilt at each
switch are shared ones in the covered segments (the Process Manager's
heap, other applications' pages touched by system code), each one HTAB
miss handled in the kernel. Scribble `F` reads each of the Finder's 259
pages after a switch to the Finder and back: 952 µs, against 37 µs
again, with 38 PTEs rebuilt. QEMU flushes its own TLB at every `mtsr`,
so that covers 259 TLB refills as well; a rebuild comes to roughly 14 µs
in QEMU, the cost of any page fault there. About 31 rebuilds a switch
(VM off) is then about 0.4 ms a switch in QEMU, and far less on a real
Mac, where a fault is a few µs.

### Per-application VSIDs: what the kernel has

Read on 2026-11-14 in the reconstructed source
(`refs/powermac-rom/NanoKernel/`, version 0x0228; three
files are not UTF-8, so use `LC_ALL=C grep -a`) and checked against
`kernel/stock/NanoKernel-v02.28.dis`. Offsets are kernel offsets.

- **Address spaces exist.** `AddressSpace` ('SPAC', 0xC0 bytes) holds 16
  segment register values at +0x30, its own BATs at +0x80 and its own
  list of areas; +0x78 and +0x7C are unused. MP call 70 creates one
  (`NKCreateAddressSpaceSub`, 0xF824): SR n = 0x20000000 | n << 20 |
  counter, the counter one more per space (20 bits, never reused).
  Spaces are never deleted (call 71 fails on the sentinel area every
  space has), so keep a fixed pool.
- **The blue task runs in the system space**, read in the guest: SRs
  0x20000000, 0x20100000 … 0x20F00000 (VSID = segment << 20, user key 1,
  supervisor key 0). The current space is `EWA.PA_CurAddressSpace`
  (EWA − 0x1C; the EWA's physical address is in SPRG0).
- **Switching.** `SchSwitchSpace` (0x13F78) loads SR0-15 from a space,
  stores the current space and reloads the BATs that differ; no TLB
  flush. The scheduler calls it only when a task's space differs from
  the current one (0x14678). Call 119 sets a task's space, but only for
  a stopped task; nothing changes a running task's space, and the blue
  task never stops. The CPU plugin's SIGP path already switches the
  CPU's space without changing the task's, so a kernel-side switch fits.
- **Page tables are global.** Segment maps, PMDTs, the HTAB and the page
  list entries (PLEs) for logical RAM (`FlatPageList`) are shared; only
  the area lists are per space. Two spaces can share every PLE and
  differ only in VSID, if area lookups (`FindAreaAbove`, 0x103EC) go to
  one area list.
- **PTE fill.** `PagingFunc1` (0x4C20) builds a PTE from the PLE: PP
  comes only from PLE bits 0x2 and 0x4 (0x4D74-0x4DB0), so today
  protection reaches the HTAB only through `MPSetAreaAccess` rewriting
  PLEs. The hash uses the VSID from `mfsrin`.
- **One PTE per page.** A PLE records exactly one HTAB slot (bit 0x800
  and bits 0-19). On a miss whose PLE already says "in the HTAB",
  `PagingFunc1` returns without building anything (0x4D88, 0x4FAC), so a
  miss under a second VSID would loop forever. Eviction (`NKPaging.s`
  395-417) panics unless the victim's PLE points at that slot, and
  rebuilds the victim's address from VSID bits 0-3 (so VSID bits 0-3
  must stay the segment number). R and C bits are merged into the PLE
  on invalidation, and with VM on the pager trusts them.
- **Segment granularity.** A VSID covers 256 MB. The RAM segments hold
  low memory, the system heap, the Process Manager's heap and every
  partition together, so per-application spaces get their own PTEs for
  every RAM page, not only for application pages. Leave segments 6 and
  up (kernel, emulator, I/O, ROM) on the system VSIDs.

The smallest design that fits:
1. An operation of call 109 that creates an application space: copy the
   system space's SRs and BATs, give the RAM segments fresh VSIDs, store
   the application's owner byte and the system space in +0x78/+0x7C.
2. Area lookups for an application space go to the system space's list
   (one patch at the entry of `FindAreaAbove`), and the area calls that
   use the current space (create, alias, current space) use the system
   space too.
3. A switch operation: set the blue task's `AddressSpacePtr`, move the
   task counts, call `SchSwitchSpace`.
4. At PTE fill, PP 11 (read-only for every key: the kernel retries
   stores in supervisor mode when the PLE allows it) when the page's
   owner is an application other than the current space's.
5. Keep one PTE per page: at 0x4D88 and 0x4FAC, when the recorded PTE
   has another VSID, invalidate and delete it and build one for this
   VSID. Every other site that trusts the PLE's one slot (the MP area
   calls, the VM calls, eviction, R/C merging, our `open` path) then
   stays right. The cost: the first access to a page after a switch,
   reads too, is one HTAB miss handled in the kernel, which is cheaper
   than today's protection fault plus re-protect; measure it.
6. `MemRetryDSI` and `task_write` treat a page whose PTE is read-only by
   owner as write-protected even when its PLE is writable.

The switch hook. The Process Manager sets `ApplZone` on every switch. A
QEMU watch on its physical address (`memwatch`, `lo=0x42AA,hi=0x42AE`)
found these writers in a VM-off guest:
- one native store, `stw r3, 0x2AA(0)`, ran 419 times in a few minutes
  of switching: the switch (a `stw r29` to 0x2AA 0x1C bytes before it
  ran 14 times). Its code fragment (136 KB of
  code, no exports, imports such as `InitZone`, `MaxApplZone`,
  `LMGetFinderName`, `PPCInit`, `WakeUpProcess`) is very likely the
  Process Manager's. It loads at a different address each boot (0x0116F2A8
  in one, 0x014CC638 in another, the same offset 0x10E08 into the code
  section);
- four more stores in the same fragment (launch, quit), a pair in another
  native fragment, and about 100 stores by 68k code.
To switch spaces at the right moment, patch the switch's store (found by
the fragment's code bytes) with a branch to a stub that stores
`ApplZone` and makes the switch call. The rarer writers can be caught
lazily: the kernel already reads `ApplZone` at each store it decides.

What was built (patch 5) differs from that plan in three ways:
- No new address spaces. The blue task stays in the system space, and
  `OP_SWITCH` rewrites that space's segment registers for the covered
  segments and loads them. That needs no `FindAreaAbove` redirect and no
  area-call changes, and the scheduler, SIGP, idle and sleep keep
  working, since they load the space's registers from the struct.
- The hook is not at the `ApplZone` store but at the end of the native
  context switch (B in `engine.c`), which comes after it, once the next
  process's stack is loaded. At the store (S, kept as a fallback) the
  Process Manager went on storing into the previous process's stack
  under the new VSIDs: 368 allowed stores in a boot against 170 at B.
  The context switch, at 0x0117DF00-0x0117E0DC in that boot, saves r13-r31,
  FPSCR, XER, f14-f31 and the vector registers into the context at r3,
  then loads them from the context at r4.
- The mask comes from the marked partitions and chunks, not from the
  Process Manager's zone: on 9.2.2 its zone header (0x1EEC9564 in a
  512 MB guest) lies above the partitions, so `bkLim` is below it.
Allowed stores in a boot then went from 170 to 10 with `kOwnerCode`: the
partly used first page of a partition holds the Process Manager's block
before the partition, and its plain owner byte made those stores fault
at each switch.

## Installer (`installer/`)

- It accepts a Mac OS ROM file whose ROM image holds NanoKernel 2.28 as
  Apple made it (CRC-32 of its 105,280 bytes, zeros after it), and
  recognizes one it has patched. A kernel with another release's patch
  (`kKernelOther`: it holds `lis r17,'EN'; ori r17,r17,'FK'`) and the stock
  file in Enforcer Backup is an update: it patches the backup. Remove puts
  the backup back only over an Enforcer-patched file.
- It replaces and trashes only files with Enforcer's creators (`is_ours`),
  and moves an older backup to the Trash instead of deleting it.
- `installer/rompatch.c` checks both Adler-32 checksums and the image's
  CRC-32, decompresses the 4 MB image, finds the kernel through
  ConfigInfo, applies the patch, sets the lane checksums, compresses again,
  moves the parcels after the image, and sets the boot script's
  `parcels-size`, `info-size` and checksums. The System Enabler moves too;
  the installer moves the data fork offsets in the `cfrg` to match.
- `installer/lzss.c` is Okumura's LZSS. It reproduces Apple's compressed
  image byte for byte. To test the patcher on the host, run
  `tools/rompatch-test.sh`.
- The File Manager renames and moves the open Mac OS ROM file; Mac OS keeps
  using it until the restart.

## Reading guest memory

`tools/xlog-read.py` and `tools/hooks-read.py` read logical memory with
QEMU's memsave, which goes through the hashed page table: a page whose PTE
is not in it does not read. That covers every page the engine protects and
any page not used lately. With application VSIDs, a page in a covered
segment reads only when its PTE was built for the VSIDs loaded at the
pause. `vdump.py` exits with "never in the blue task's address space"
when any page of a range does not read; read page by page instead.

To read anything else, use `tools/lmem.py`. It takes a physical dump and
reads it through the map from logical to physical memory, which with
virtual memory off is a few linear runs (on the test machine, logical +
0x4000 below 0x00BFC000 and logical + 0x35D000 from 0x00CB6000 up):

```bash
tools/lmem.py dump 0x010BA800 0x40
```

```bash
tools/lmem.py find 0x1EEC9520
```

memsave reads that go page by page over large ranges pause the guest for
minutes; the physical dump takes a few seconds.

## Working on the kernel

The scripts are in `kernel/`. They write into `kernel/stock/` and
`kernel/build/`, which git ignores.

To take the kernel out of the test machine's installed system, run
`kernel/extract.sh`. It copies the Mac OS ROM file from
`disks/os9.qcow2` (read only), checks its SHA-256, unpacks it with tbxi
into `kernel/stock/dump/`, and writes `NanoKernel-v02.28.bin` and its
disassembly `NanoKernel-v02.28.dis` (addresses are offsets in the
kernel).

`kernel/nanokernel.s` is the patched kernel: the stock binary, copied in
with `.incbin`, each patch assembled at its offset, and new code after the
stock kernel. To build it and pack it into a ROM file, run
`kernel/build.sh`. It prints every changed or added instruction and writes
`kernel/build/Mac OS ROM`. To install that file on `disks/os9-kernel.img`,
shut the machine down and run `kernel/install.sh` (it refuses while QEMU
has the image open).

After changing the kernel, write the installer's patch again and rebuild
it:

```bash
tools/make-kpatch.py && tools/rompatch-test.sh && ./build.sh installer
```

The ROM file's resource fork has to come from tbxi: its `cfrg` gives the
data fork offsets of the System Enabler's code fragments, which move when
the compressed image's size changes. With the stock resource fork, a
patched ROM stops at startup with "unimplemented trap".

To check which kernel is running, dump it and compare it with the build
(`build.sh` prints its size):

```bash
tools/qmp.py raw '{"execute":"pmemsave","arguments":{"val":15794176,"size":109248,"filename":"/tmp/nk-live.bin"}}'
```

```bash
cmp /tmp/nk-live.bin kernel/build/NanoKernel-v02.28.bin
```

The last 312 bytes are data the kernel writes: `task_log` (272 bytes),
then `enf_cb` (4 bytes), the physical address of the registered control
block, then patch 5's `vsid_mask`, `sys_space` (the blue task's
`AddressSpace`, physical) and `vsid_cur` (the owner whose VSIDs it
holds), then patch 7's `sparse_lo`, `sparse_hi`, `pool_head`,
`pool_count`, `sparse_fills` and `sparse_empty`. `build.sh` prints the physical address of `task_log` and
`enf_cb`. `task_log` is a
count, three zero words, then 16 entries of four words (the task's PC, the
address, the task's ID, the page's new PTE); the latest is number
(count − 1) mod 16.

### The stock kernel

- The Mac OS ROM file is from the 9.2.2 install (`MacROM for NewWorld`,
  2002-04-17); Mac OS reports it as Mac OS ROM 9.2.1.
- NanoKernel 2.28, 105,280 bytes, at offset 0x310000 of the 4 MB ROM
  image. On the test machine the image is at physical 0x00C00000, so the
  kernel runs at 0x00F10000.
- Free space: from the kernel's end (0x329B40) to the 68k emulator
  (0x360000), 222,400 zero bytes. Put new code there and writable data in
  RAM allocated at boot. ConfigInfo is at 0x30D000.
- `refs/powermac-rom/NanoKernel` reconstructs version 2.28;
  use it to read and name the binary, and check each patch site against
  the disassembly.

| Routine (source) | Offset |
|---|---|
| `IntDSI`, data fault entry (`NKInterrupts.s`) | 0x32C0 |
| `MemRetryDSI`: fault in a store the kernel runs; read-only page check | 0x35A0; 0x3630 |
| `HandleDSIDueToIllegalSyncPrimitive` | 0x340C |
| `IntAlignment` | 0x3460 |
| Exception return: `lwarx`/`sync`/`stwcx.` clearing the reservation | 0x2C28 |
| `FDP_053c`: emulated `lwarx` (`NKTranslation.s`) | 0x5D3C |
| `FDP_06b4`/`FDP_06c0`: emulated `stw`/`stwcx.` | 0x5EB4/0x5EC0 |
| `MPCreateArea`: the area flags check | 0xFC98 |
| `MPSetAreaAccess` (`NKAddressSpaces.s`): `@pageloop` (patch 2) | 0x10B68 |
| `ExceptionMemRetried`; its choice of path | 0x2980; 0x2A14 |
| `SIGP`: CPU plugin call; `IntReturnFromSIGP` | 0x48E0; 0x4A38 |
| `IntExternalSystem`: external interrupt entry | 0x4880 |
| PIH (OpenPIC): acknowledge; end of interrupt; CR bits for the emulator | 0x16068; 0x16028; 0x15524 |
| MP call table (entry n: routine − 4n); entry 109 (patch 4); `MPCallBad` | 0xACB8; 0xAE6C; 0xAF0C |
| `CommonMPCallReturnPath` (result in r3) | 0xB124 |
| The scheduler moves the blue task from native code to the emulator for a 68k interrupt (`NKScheduler.s`, patch 6) | 0x146D0 |
| `GetPTEFromPLE`; `AcquireLock` | 0x124F8; 0x2700 |

Kernel structures the patches use: the CoherenceGroup pointer is the word
at EWA − 0x338 (`mfsprg rX,0` gives the EWA); in the CoherenceGroup,
`LA_CpuPlugin` is at +0x28, `CpuPluginSize` at +0x30 and
`CpuPluginSpacePtr` at +0x4C; in an Area, `PageSize` is at +0x68 and
`AddressSpacePtr` at +0x6C. `MemRetryDSI` saves a task's r8, r9, r16 (the
flags) to r19 and CR at EWA − 0xE0 to − 0xC8; the HTAB lock is at
KDP − 0xB90 (the KDP pointer is the word at EWA − 4).

To see which path a write violation takes, use the pcwatch plugin (see
"QEMU plugins") at physical 0x00F12A14 (`ExceptionMemRetried`'s choice):
`regs=r9:r10,crmask=0x82208000` records the cause, the faulting PC and the
path (0x80200000 blue task, 0x00008000 CPU plugin, 0 another task).

## QEMU plugins

Four QEMU TCG plugins in `tools/` watch the guest. Each builds with its
`build.sh` against the plugin header of the QEMU that `qemu/build.sh` installs and
runs only in that QEMU; `run.sh` loads one with `PLUGIN=LIBRARY,ARGS`.
memwatch, pcwatch and iolog write their report when QEMU exits (for
example after `tools/qmp.py quit`).

`tools/memwatch/` reports which code reads or writes a range of physical
memory: each PC and kind (load or store) once, with a count and the first
address it touched, and how many of the range's pages were read or
written. To find what touches the space after the NanoKernel:

```bash
SCRATCH=1 DISK=disks/os9-kernel.img PLUGIN=tools/memwatch/build/libmemwatch.dylib,lo=0xF29B40,hi=0xF60000,out=run/mw.txt ./run.sh &
```

`tools/pcwatch/` counts the register values seen each time the guest runs
one instruction: up to 8 registers by gdb name, and optionally CR under a
mask. NanoKernel code runs with translation off, so its address is
physical:

```bash
SCRATCH=1 DISK=disks/os9-kernel.img PLUGIN=tools/pcwatch/build/libpcwatch.dylib,at=0xF12A14,regs=r9:r10,crmask=0x82208000,out=run/pw.txt ./run.sh &
```

`tools/iolog/` keeps a timeline of the latest events (`n=`, default
4096): each load and store to a physical range, device registers
included, with its PC and value, and each run of up to 8 marked
instructions (`mark=`), with up to 4 registers (`regs=`). To see each
interrupt the kernel acknowledges at the OpenPIC (a read of 0x800600A0
returns the vector) and ends (a write to 0x800600B0), with where the
interrupt arrived (r10 at the kernel's external interrupt entry):

```bash
SCRATCH=1 DISK=disks/os9-kernel.img PLUGIN=tools/iolog/build/libiolog.dylib,lo=0x800600A0,hi=0x800600C0,mark=0xF14884,regs=r10:r11,n=50000,out=run/io.txt ./run.sh &
```

The report's totals line counts every event, kept or not; here R and W
should be equal. To trace the USB controller's registers instead, turn on
QEMU's trace events while the guest runs: `tools/qmp.py hmp 'trace-event
usb_ohci_mem_write on'` (and `usb_ohci_mem_read`); they print to QEMU's
standard error.

### Store measurement (storewatch)

`tools/storewatch/` counts every store the guest makes into low memory
(below 0x2800) and the system heap (0x2800 to SysZone's end). For each
store it records the code that made it: the native PC, or for 68k code
the 68k instruction after the store (as Enforcer's 68k hook does). It
marks a store "app" when that code lies in the current application's
partition, which it reads from the running engine's 68k hook data. To
measure an idle and a busy phase:

```bash
SCRATCH=1 DISK=disks/os9-enforcer.qcow2 PLUGIN=tools/storewatch/build/libstorewatch.dylib,out=run/sw ./run.sh &
```

```bash
tools/storewatch/scenario.sh run/sw && tools/storewatch/report.py run/sw-use
```

The plugin runs on every store, so the guest slows down in proportion to
how many stores it makes: about 16 million instructions a second at the
desktop. To measure something else, create `run/sw.reset` to clear the
counts and `run/sw.dump` to write them (see `storewatch.c`).

Results on 9.2.2 (2026-11-12, Enforcer running; the busy phase opens a
window, uses the Calculator and Stickies, and switches applications):

| | Idle | Busy |
|---|---|---|
| Instructions | 334 million | 1,610 million |
| Low memory stores per million instructions | 682 | 752 |
| System heap stores per million instructions | 5,614 | 3,758 |
| Low memory stores by app code | 0 | 16 |
| System heap stores by app code | 0 | 0 |
| Stores into the 68k vectors (0000-00FF) | 0 | 0 |

- System code writes low memory all the time: `MemErr` and `TheZone` on
  every Memory Manager call, the current port, `ResErr`, `CrsrVis`, the
  File Manager and VBL queues.
- The only low memory global written by app code was `ResReadOnly`
  (0A5C), by a 68k application.
- The emulator saves state into a 544-byte frame, from a table near
  0x46000, when 68k code calls native code (most traps), from one
  instruction, 6806E34C. r24 still points at the application's trap
  instruction then, so the 68k-PC test counted the emulator's store as the
  application's. The plugin and Enforcer's 68k hook count stores by that
  emulator code (6806E300-6806E370) as system stores.
- Nothing wrote the 68k exception vectors, where a write through a NULL
  pointer with a small offset lands.

Trapping every store into low memory or the system heap is too slow, and
low memory's first page holds both the vectors and the most written
globals. Applications' own code almost never writes either area.

The plugin's `epoch=` option replays page-by-page protection on every
system heap page from 0x2000 up (protect each page, open it at the first
store, protect it again at each interval) and counts the traps:

```bash
SCRATCH=1 DISK=disks/os9-enforcer.qcow2 PLUGIN=tools/storewatch/build/libstorewatch.dylib,out=run/pm,epoch=ticks:250000:1000000:4000000:16000000 ./run.sh &
```

`ticks` protects again at each tick of the guest's clock (1/60 s); the
numbers are intervals of a fixed count of instructions, for busy work.
Results (2026-11-12, the same phases, 5,300 system heap pages; "open for
good" leaves the pages with the most traps unprotected; 70 µs a trap):

| Open for good | Traps per tick, idle / busy | CPU | Pages protected |
|---|---|---|---|
| none | 51 / 45 | about 20% | 99.4% |
| 64 pages | 1.6 / 6.3 | 0.7% / 2.6% | 98.7% |
| 128 pages | 0.1 / 2.3 | 0.1% / 1.0% | 97.5% |
| 256 pages | 0 / 0.8 | 0.3% | 95.2% |

With 16 million instructions an interval (busy CPU), the busy phase takes
124 traps an interval with no pages open for good (about 54% CPU) and 22
with 256 (10%). The pages with the most traps are the emulator's call
frames (0x44000 to 0x46000) and a few dozen others, written in nearly
every tick. A protected page holding a variable that Mac OS updates with
`lwarx`/`stwcx.` hung the machine before kernel patch 1.

## Icons

The icon is a blue shield with a gold rim and a memory chip on its face;
the extension's stands it on a puzzle piece. To change them, edit
`tools/make-icons.py` and run it: it rewrites `enforcer/icon-panel.r` and
`enforcer/icon-extension.r`, and with `-p DIR` writes a preview to
`DIR/icons.png`.

Each program carries its icons twice: the icon family at ID 128, which its
bundle names for the desktop database (256 colors, 1-bit mask), and an
`icns` at ID −16455, the custom icon, which Icon Services draws in 32-bit
color, also on a CD. Retro68's Rez clears every Finder flag, so the build
sets hasBundle and hasCustomIcon with `tools/finder-flags.py`.

## History

The design history, with test runs and numbers, is in git: `git log`, and
`git show 2c94d92:README.md` for the step-by-step development log up to
the deferred-task fix. The first prototype (everything in an extension,
no kernel patch) is in `git show c491006:README.md`.
