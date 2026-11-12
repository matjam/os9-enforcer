# os9-protect

Test machine and tooling for adding crash containment to Mac OS 9.2.2: a
crashed or hung application should not take the rest of the system down.

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
`disks/os9.qcow2`). `disks/os9-novm.qcow2` is an overlay on `os9.qcow2` with
virtual memory turned off in the Memory control panel; the hook tests use it,
so that no page is ever paged out under a hook. An overlay records only its
own changes, so changing `os9.qcow2` after the overlay was made corrupts the
overlay. To make it again:

```bash
qemu-img create -f qcow2 -b os9.qcow2 -F qcow2 disks/os9-novm.qcow2
```

Then boot it with `DISK=disks/os9-novm.qcow2 ./run.sh`, turn Virtual Memory
off in the Memory control panel and restart.

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
anything else is copied as raw data.

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

## Page protection probe

`probe/` is a PowerPC console application that write-protects one page of its
own heap with the NanoKernel's area calls, then stores to it and reports what
the Exception Manager handler saw. To build it and run it in a throwaway
guest:

```
cmake -S probe -B probe/build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain/build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake --build probe/build
SCRATCH=1 ./run.sh &
tools/push.sh -n Probe probe/build/Probe.bin
```

Then open the Probe disc in the guest and run Probe. It pauses after printing
the area information; press Return to run the tests.

## Write logger

`common/nk.h` holds the NanoKernel area calls (`nk_protect`, `nk_unprotect`,
`nk_area_from_address`) and the Exception Manager declarations, including
`nk_find_system_installer()`, which finds the installer for the Exception
Manager's system-wide handler slot.

`wlog/` (WLog) write-protects its own application heap and logs stores into
it made while another application is current. It lists writers by store
site and application: native stores by PC and LR, 68k stores by the 68k
instruction after the store, marked `(68k)`.

WLog installs the 68k write hook and points it at its heap. Without the
hook, a 68k store into the protected heap at interrupt time or during a
process switch hangs the whole machine. A Time Manager task re-protects
pages every 16 ms. On each tick it takes the 68k stores the hook caught and
protects their pages again. The hook records the current zone (ApplZone) at
each store, and WLog names the application from it through the Process
Manager when it prints the report. Run WLog only in a `SCRATCH=1` guest.

`run/wlog-scenario.sh [WLog.bin]` boots a scratch guest if none is running,
runs WLog, has the Finder draw over it, then stops it and saves screenshots
as `run/ws-*.png`.

## Write hooks

Two small PowerPC handlers catch stores to protected pages before the system
sees them, make the page writable and let the store run again. One covers
68k code, the other native code. Both keep the same data block
(`common/hook.h`):
- the pages they manage: a range `[lo, hi)` and, optionally, a page bitmap
  over it, one bit per 4 KB page;
- a "current" range `[cur_lo, cur_hi)`, the current application's own
  memory: a store there is only counted (`count_self`);
- a 64-entry ring, in the system heap, recording every other store they
  handle;
- `passed`, the last write fault they did not handle, which goes on to the
  original handler.

Each hook makes the page writable with `MPSetAreaAccess` and returns to the
kernel with r3 = 0, which runs the faulting instruction again. Anything else
goes to the original handler with every register as it arrived. Both stay
installed after the installing application quits.

Both hooks also apply a deny rule to the stores they record. When the code
doing the store lies inside the current range, the current application's
own code is writing outside its memory: the hook sets bit 0 of the ring
entry's `pc`. With `enforce` set, it also leaves the page protected and
passes the fault on, which stops the application; it counts these in
`count_denied` and keeps the last in `denied`. For native code the store's
code is the faulting PC; for 68k code it is the next 68k instruction.

The hooks only see faults in the blue task (Mac OS itself, including its
interrupt-level code). A write fault in any other NanoKernel task, such as
a Multiprocessing Services task, stops that task without reaching them. To
check that none happened, compare the kernel's count of write faults with
the hooks' total:

```bash
tools/faults-read.py
```

### 68k hook

A store from 68k code to a protected page never reaches the Exception
Manager. The emulator handles the fault itself: in an ordinary context the
application gets a bus error (error type 1), and at interrupt time or during
a process switch the emulator double-faults and the machine hangs.

`common/h68k.s` goes in front of the emulator's own exception entry. The
NanoKernel delivers every exception taken in emulator context to the address
in the emulator context block (logical 0x68FFF14C, normally 0x6806D440).

Each ring entry holds:
- a sequence number;
- the emulator instruction that faulted, and the fault address;
- r24, the emulator's 68k instruction pointer;
- the emulator's return address (LR);
- the current zone (ApplZone), the value stored (usually r4) and the 68k
  stack pointer (A7, which is r1).

The emulator is a threaded interpreter: each opcode handler fetches the next
opcode with `lhau rX,2(r24)` before its own store, so at the fault r24 is
the next 68k instruction + 2 (`hook_next_pc()` subtracts the 2).

The hook's code and data live in unused space in the emulator data page
(logical 0x68FFFB80-0x68FFFEFF: data, then code), because it must be within
a relative branch (32 MB) of the emulator's entry. That space stayed zero
across 40 dumps taken while the Finder and QuickTime ran.

`common/h68k.h` installs and removes it: `h68k_install(area)` and
`h68k_uninstall()`. `h68k_install` refuses unless the context block's
handler is in the emulator's code and the space is all zeros.

### Native hook

The NanoKernel delivers exceptions in native code to the handler in the
native context block (logical 0x68FFF44C), normally the ROM Exception
Manager entry 0xFFCF0F28. That entry saves every register in a frame on the
interrupted stack, so a store to a protected stack page faults again inside
it, which is fatal. `common/hnat.s` goes in front of it and never touches
the stack.

Its ring entries hold the faulting PC, the fault address, the interrupted
r3, LR and r4, the current zone and the stack pointer (r1).

The hook's code, data block and ring live in one system heap block, placed
by `hnat_install(area)` in `common/hnat.h`. The install patches the data
block's address into the code and the original handler into its final
absolute branch. `hnat_uninstall()` puts the original handler back and frees
the block.

### Building and testing

`common/hooks.cmake` assembles both hooks at build time; call
`hooks_embed(target)` from an application's CMakeLists.

`h68k/` (H68KTest) tests both hooks. Run it in a `SCRATCH=1` guest booted
from `disks/os9-novm.qcow2`. The first run installs them and runs:
- A: a 68k store to an unprotected page;
- B: a 68k store to a protected page in the 68k hook's range: the hook opens
  the page, the store lands and the ring records it;
- C: the same for a native store, through the native hook;
- D: as C, with the page in the current range: counted, not recorded;
- E: native stores to the application's own stack while those pages are
  protected: the native hook opens them;
- F: after a pause, a 68k store to a protected page outside the range: the
  application is killed with error type 1 and the system stays up.

The second run prints both hooks' counters and removes them.

To read both hooks' counters, rings and passed-on records from the host
while the guest runs:

```bash
tools/hooks-read.py
```

It reads everything in one pause of the guest through `tools/vdump.py`.

## Cross-application write logger

`xlog/` (XLog) logs stores from one application into another's memory,
across every running application. It only logs: every store is allowed.
The same directory builds Guard, which denies some of them (see "Guard"
below).

Each running application's partition, from the Process Manager, is a
domain. While an application is not current, XLog write-protects the whole
pages of its partition and sets their bits in a page bitmap over the blue
area. Both write hooks manage exactly those pages. A store into a protected
page is opened and recorded by the hook, unless it lands in the current
application's own domain, which the hooks only count.

A Time Manager task runs every 16 ms:
- When ApplZone changes (a process switch), it points the hooks' current
  range at the new application's partition and protects the previous
  application's domain again.
- It takes the hooks' rings. A store is the target's own when ApplZone at
  the store is inside the target's partition. Otherwise it is logged by
  store site (PC and LR for native code, the next 68k instruction for 68k
  code), writing application and target. The page is protected again unless
  it belongs to the current application.

A store that reaches the Exception Manager from another context goes to a
fallback handler in its system-wide slot, which opens the page and records
the store in a third ring. The process list is read again every half
second, adding and removing domains.

To run it, boot a scratch guest from the VM-off overlay, push XLog, and
open it. It prints a status line every 10 seconds. To stop it, bring it to
the front and press a key; it prints the domains and the 24 most frequent
cross-domain writers.

```bash
SCRATCH=1 DISK=disks/os9-novm.qcow2 ./run.sh &
tools/push.sh -n XLog xlog/build/XLog.bin
```

To read its counters, domains and writers from the host while it runs:

```bash
tools/xlog-read.py
```

XLog's state begins with a header giving the offsets and sizes of its parts,
which `xlog-read.py` checks against its own copy of the layout.

XLog marks a writer `(app code)` when the code doing the store is the
writing application's own, and lists those stores at the end of its report:
these are the stores Guard denies.

## Guard

Guard is XLog built with `XLOG_ENFORCE=1`. It stops an application whose
own code writes into another application's memory, and allows everything
else.

The rule: a store into another application's protected pages is denied when
the code doing it lies inside the current application's partition (its
code fragment and 68k code resources are in its heap). The hook leaves the
page protected, so the target's memory is unchanged, and the engine stops
the application with `ExitToShell` (see [Clean stop](#clean-stop)). Guard
prints each denial in its window:

```
DENIED: Scribble's own code (native, 1E1B5C98) wrote into Finder at 1EA34100
```

Stores made by system code are allowed and logged, even when they land in
another application's memory: the Process Manager, Window Manager and Layer
Manager do this on every switch, and their code lives in the system heap
and ROM, outside every partition.

Limits:
- A store by system code on an application's behalf is allowed, for example
  `BlockMoveData` called with a bad destination. Guard looks at the code
  doing the store, not at who called it.
- Only other applications' partitions are protected. The system heap and
  low memory are not.
- An application's code must be inside its partition, or its stores are
  treated as system code. With virtual memory on, the Code Fragment Manager
  can map code from its file outside the partition (not tested); run Guard
  with virtual memory off.
- A 68k denial at interrupt time hangs the machine (the emulator cannot take
  a bus error there). This needs the current application's own interrupt
  task to write into another application.
- A native denial at interrupt time would call `ExitToShell` at interrupt
  time: the native side cannot tell interrupt time from task level (not
  tested).

`scribble/` (Scribble) tests it. It stores into a word in the middle of the
Finder's partition the value already there, so an allowed store changes
nothing:
- `n`: a store from its native code: denied;
- `6`: a store from 68k code in its heap: denied;
- `b`: `BlockMoveData`, system code: allowed;
- `o`: a store into its own heap: allowed.

A denied store stops Scribble; open it again for the next test. To build and
run both:

```bash
cmake -S scribble -B scribble/build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain/build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake --build scribble/build
SCRATCH=1 DISK=disks/os9-novm.qcow2 ./run.sh &
tools/push.sh -n Test xlog/build/Guard.bin scribble/build/Scribble.bin
```

Open Guard first, then Scribble. Guard's state reads the same way as XLog's,
with `tools/xlog-read.py`.

## Protection engine

XLog, Guard and Enforcer share one engine, `common/engine.c` (domains, the
timer task, the fallback handler and the hooks' setup). Its state lives in
the system heap and starts with a header that `tools/xlog-read.py` reads. It
has three modes: off (nothing protected), log only (as XLog) and enforce (as
Guard). Only one engine can run at a time: a second one refuses to start
while the hooks have an owner.

### Clean stop

Mac OS stops a faulting application with "unexpectedly quit, because an
error of type 1 occurred" and advises a restart, because it cannot know
what the application changed. After a denial, the engine knows that the
store did not happen, so it stops the application itself, with no alert:

- A hook that denies a store writes ApplZone (the application to stop) into
  a word in a system heap block, the `Stopper`.
- A denied native store reaches the engine's handler in the Exception
  Manager's system-wide slot before any other handler. The handler sets the
  faulting context's PC, r2 and r12 to `ExitToShell`'s and resumes it, so
  the application quits through the normal exit path.
- A denied 68k store becomes a 68k bus error. A 28-word 68k stub in the
  `Stopper`, in front of the bus error vector (low memory 0x08), calls
  `_ExitToShell`.

Both check that ApplZone still matches the word, and the stub also checks
that the frame's interrupt mask is 0. Any other fault goes on to the
previous handler, as before. The engine restores the vector when it stops;
if another program has patched the vector since, the stub stays in place
and passes every fault on.

`tools/xlog-read.py` prints the `Stopper`'s address and its count of clean
stops by kind (native, 68k).

## Enforcer

Enforcer is Guard's protection for everyday use: it starts with the
machine, has no window, and has a control panel.

- **Enforcer Extension** (file type `appe`, in the Extensions folder) is a
  background-only application. Mac OS launches it at startup. It runs the
  engine in the mode saved in "Enforcer Preferences" (stop, by default) and
  publishes its state under Gestalt selector `'Enfr'`. If virtual memory is
  on, it does not start.
- **Enforcer** (file type `APPC`, in the Control Panels folder) is the
  control panel, under Apple menu > Control Panels. It shows whether the
  extension runs, two checkboxes, the last stores it caught, and counters:
  - "Protect each application's memory from the others": off turns
    protection off;
  - "Stop an application that writes into another's memory": off only logs
    those stores.

  A change takes effect within half a second and is saved.

When Enforcer stops an application, Mac OS shows no alert (see
[Clean stop](#clean-stop)). Enforcer posts a notification instead:

> Enforcer stopped “Scribble” because it tried to write into the memory of
> “Finder” at address 1E9DC100. The write was blocked, so “Finder” was not
> changed and you do not need to restart.

If Mac OS stopped the application instead (a denial at interrupt time), its
own alert appears and the notification leaves out the last part.

The claim covers the denied store only. The system heap and low memory are
not protected, so an application that crashes for another reason can still
have damaged them, and Mac OS's own alert then still applies.

The extension's timer task and fallback handler run code in its own
partition. Do not force-quit it: quit it with the quit Apple event (as at
shutdown), which removes them first.

To build both:

```bash
cmake -S enforcer -B enforcer/build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain/build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake --build enforcer/build
```

This makes `enforcer/build/Enforcer Extension.bin` and
`enforcer/build/Enforcer.bin`, with the names, types and creators the
Finder needs.

Testing it needs a disk that keeps its changes across a restart, so not a
`SCRATCH=1` guest. `disks/os9-enforcer.qcow2` is an overlay on the VM-off
overlay; to make it again (which removes Enforcer and everything else done
on it):

```bash
qemu-img create -f qcow2 -b os9-novm.qcow2 -F qcow2 disks/os9-enforcer.qcow2
```

To install, boot it and push both files:

```bash
DISK=disks/os9-enforcer.qcow2 ./run.sh &
tools/push.sh -n Install "enforcer/build/Enforcer Extension.bin" enforcer/build/Enforcer.bin
```

In the guest, drag each file onto the System Folder icon and click OK when
the Finder offers to put it into the Extensions or Control Panels folder.
Then restart. To remove Enforcer, drag both out of those folders and
restart.

To update an installed Enforcer, first drag the running Enforcer Extension
out of the Extensions folder (to the desktop or the Trash): its file is in
use, so the Finder cannot replace it. Then install as above; the Finder
offers to replace the control panel. The old extension stops at the
restart.

A slow drag over a folder springs it open, so a scripted drag
(`tools/qmp.py drag`) can drop inside a folder in the System Folder
instead. To avoid that, turn off "Spring-loaded folders" in the Finder's
Edit > Preferences; the test disk has it off.

To check that the hook's space in the emulator data page stays zero, take 40
dumps half a second apart (do this before installing the hook):

```bash
tools/edp-scan.py 40 0.5
```
