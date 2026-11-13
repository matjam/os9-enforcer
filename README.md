# os9-enforcer

Enforcer adds crash containment to Mac OS 9.2.2: an application that writes
into another application's memory is stopped before the write lands, and
the rest of the system keeps running without a restart. This repository
holds Enforcer, the protection engine and write hooks it is built on, and
the test programs and host scripts used to develop it.

The toolchain (Retro68), the QEMU test machine, its disk images and the
tools that control the guest are in `os9-protect`, next to this folder
(`~/src/os9-protect`). Its README describes the test machine. Scripts here
find it through the `OS9_PROTECT` environment variable, which defaults to
`../os9-protect`.

## Layout

| Directory | What it is |
|---|---|
| `common/` | The protection engine, the 68k and native write hooks, NanoKernel calls |
| `enforcer/` | Enforcer: the startup extension and the control panel |
| `xlog/` | XLog, Guard, SysLog and SysGuard: console front ends on the engine |
| `scribble/` | Scribble: stores into other memory, to test Guard and Enforcer |
| `h68k/` | H68KTest: tests for both write hooks |
| `wlog/` | WLog: the first write logger, on the 68k hook only |
| `probe/` | Probe: the first page protection experiment |
| `tools/` | Host scripts that read the engine's and hooks' state from the guest |
| `kernel/` | Takes the stock NanoKernel out of the test machine's Mac OS ROM file (`extract.sh`), packs a ROM file (`pack.sh`) and installs it in the kernel test disk (`install.sh`) |

## Building

To build every program, run:

```bash
./build.sh
```

To build some of them, name their directories, for example
`./build.sh enforcer scribble`. Each program builds in its own `build/`
directory as a `.bin` (MacBinary) file that `os9-protect/tools/push.sh` can
copy into the guest. `build.sh` uses the PowerPC toolchain built in
`$OS9_PROTECT/toolchain`; to build that, see "Toolchain" in os9-protect's
README.

## Running the test machine from here

Run the test machine's commands from this folder through `../os9-protect`.
`run.sh` changes to its own folder first, so a `DISK=` path is relative to
os9-protect:

```bash
SCRATCH=1 DISK=disks/os9-novm.qcow2 ../os9-protect/run.sh &
```

`push.sh` takes paths relative to the current folder:

```bash
../os9-protect/tools/push.sh -n Test xlog/build/Guard.bin scribble/build/Scribble.bin
```

`disks/os9-novm.qcow2` has virtual memory off, which every program here
needs. `SCRATCH=1` throws away every disk write when the machine stops.

## Page protection probe

`probe/` is a PowerPC console application that write-protects one page of its
own heap with the NanoKernel's area calls, then stores to it and reports what
the Exception Manager handler saw. To run it in a throwaway guest:

```bash
SCRATCH=1 ../os9-protect/run.sh &
../os9-protect/tools/push.sh -n Probe probe/build/Probe.bin
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

`wlog/scenario.sh [WLog.bin]` boots a scratch guest if none is running,
runs WLog, has the Finder draw over it, then stops it and saves screenshots
as `os9-protect/run/ws-*.png`. `wlog/find-wlog.py dump.bin` finds WLog's
state in a physical memory dump and prints its recent faults.

## Write hooks

Two small PowerPC handlers catch stores to protected pages before the system
sees them, make the page writable and let the store run again. One covers
68k code, the other native code. Both keep the same data block
(`common/hook.h`):
- the pages they manage: a range `[lo, hi)` and, optionally, a page bitmap
  over it, one bit per 4 KB page;
- a "current" range `[cur_lo, cur_hi)`, the current application's own
  memory: a store there is only counted (`count_self`);
- a 256-entry ring, in the system heap, recording every other store they
  handle;
- `passed`, the last write fault they did not handle, which goes on to the
  original handler.

Each hook makes the page writable with `MPSetAreaAccess` and returns to the
kernel with r3 = 0, which runs the faulting instruction again. Anything else
goes to the original handler with every register as it arrived. Both stay
installed after the installing application quits.

A protected page is read-only for all code, the NanoKernel included (page
protection bits 11). The kernel also offers read-only for user mode only
(`kAccessSupRWUserRO`, bits 01), which would let the kernel's own stores
through, but on 9.2.2 the blue task's stores go through it as well (tested
under Guard: 171 process switches, no write faults). So page protection
cannot tell Mac OS code from the kernel, let alone from an application.

Both hooks also apply a deny rule to the stores they record. When the code
doing the store lies inside the current range, the current application's
own code is writing outside its memory: the hook sets bit 0 of the ring
entry's `pc`. When the fault address is at or above `deny_from`, it also
leaves the page protected and passes the fault on, which stops the
application; it counts these in `count_denied` and keeps the last in
`denied`. `deny_from` is ~0 (deny nothing) after install. For native code
the store's code is the faulting PC; for 68k code it is the next 68k
instruction, except when the store is made by the emulator code that fills
a 68k-to-native call frame (`frame`, 0x6806E300 on 9.2.2), whose stores
the hook treats as system stores.

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
across 40 dumps taken while the Finder and QuickTime ran. To check it again,
take 40 dumps half a second apart (before installing the hook):

```bash
tools/edp-scan.py 40 0.5
```

`common/h68k.h` installs and removes it: `h68k_install(area)` and
`h68k_uninstall()`. `h68k_install` refuses unless the context block's
handler is in the emulator's code and the space is all zeros. The code may
take at most 704 bytes (it takes 700); a static assertion checks this.

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
while the guest runs (the newest 64 ring entries of each, or N):

```bash
tools/hooks-read.py
```

It reads everything in one pause of the guest through os9-protect's
`tools/vdump.py`.

## Cross-application write logger

`xlog/` (XLog) logs stores from one application into another's memory,
across every running application. It only logs: every store is allowed.
The same directory builds Guard, which denies some of them (see
[Guard](#guard)), and SysLog and SysGuard (see
[Watching the system heap](#watching-the-system-heap-syslog-and-sysguard)).

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
SCRATCH=1 DISK=disks/os9-novm.qcow2 ../os9-protect/run.sh &
../os9-protect/tools/push.sh -n XLog xlog/build/XLog.bin
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
- An atomic update (`lwarx`/`stwcx.`) by system code of a variable in a
  protected page hangs the machine (see
  [Watching the system heap](#watching-the-system-heap-syslog-and-sysguard)).
  Not seen with applications' partitions so far.

`scribble/` (Scribble) tests it. It stores into a word in the middle of the
Finder's partition the value already there, so an allowed store changes
nothing:
- `n`: a store from its native code: denied;
- `6`: a store from 68k code in its heap: denied;
- `b`: `BlockMoveData`, system code: allowed;
- `o`: a store into its own heap: allowed;
- `s`, `S`: a store from its native or 68k code into the middle of the
  system heap, for SysLog and SysGuard (see
  [Watching the system heap](#watching-the-system-heap-syslog-and-sysguard));
- `t`: times traps, with the Enforcer Extension running (see
  [What a trap costs](#what-a-trap-costs)).

A denied store stops Scribble; open it again for the next test. To run
both:

```bash
SCRATCH=1 DISK=disks/os9-novm.qcow2 ../os9-protect/run.sh &
../os9-protect/tools/push.sh -n Test xlog/build/Guard.bin scribble/build/Scribble.bin
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
./build.sh enforcer
```

This makes `enforcer/build/Enforcer Extension.bin` and
`enforcer/build/Enforcer.bin`, with the names, types and creators the
Finder needs.

Testing it needs a disk that keeps its changes across a restart, so not a
`SCRATCH=1` guest: `disks/os9-enforcer.qcow2` in os9-protect (its README
says how to make it again). To install, boot it and push both files:

```bash
DISK=disks/os9-enforcer.qcow2 ../os9-protect/run.sh &
../os9-protect/tools/push.sh -n Install "enforcer/build/Enforcer Extension.bin" enforcer/build/Enforcer.bin
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

## What a trap costs

Each store that finds its page protected costs a trap: the hook opens the
page, and the engine protects it again at its next 16 ms tick. To measure
it, Scribble's `t` test rewrites a word on 32 of the Finder's pages with
`BlockMoveData` for two seconds, with Enforcer's protection off and then
on, and divides the extra time by the traps. On the QEMU test machine
(Apple Silicon host) one trap costs 60 to 76 µs, about 70 µs, including
Enforcer putting the protection back; most of it is likely QEMU flushing
its TLB when the page table changes.

os9-protect's storewatch plugin measured how many stores Mac OS makes into
low memory and the system heap, and what protecting the system heap page
by page would cost (see "Store measurement" in its README): about 20% of
the CPU if every page is protected again at every tick, and 1 to 3% in
interactive use if the few dozen pages the system writes all the time are
left open.

## Watching the system heap: SysLog and SysGuard

SysLog and SysGuard are XLog and Guard built with `XLOG_SYSHEAP=1`. The
engine then adds the system heap as one more domain: the whole pages from
SysZone's header to its end (`bkLim`), followed as the heap grows. It is
log only: the hooks deny app-code stores only at or above `deny_from`,
which is the system heap's end in enforce mode. A page that traps on 4
ticks in a row is left open for a while (1 s, doubling up to 64 s when it
keeps turning hot). Pages holding the engine's own blocks, pages that are
not plain read/write, and pages that held the stack pointer of a trapped
store are never protected. `tools/xlog-read.py` prints the counts.

It does not work yet. On 2026-11-12 SysLog protected 5,253 pages (43 left
out for the engine's own blocks), the hooks handled 22 traps, and then the
machine hung:
- The CPU looped in the ROM's atomic increment at 0xFFD88A3C (`lwarx`,
  `addi`, `sync`, `stwcx.`, `bne` back) on 0x6F7B8, a protected system heap
  page, alternating with NanoKernel code.
- No hook saw a fault, the kernel's exception counts did not move, and the
  engine's timer stopped.
- The NanoKernel handles a data fault by running the faulting instruction
  itself, with the caller's translation (`NKTranslation.s` in os9-protect's
  `refs/powermac-rom/NanoKernel`). For `stwcx.` it runs `stwcx.`
  (FDP_06c0), but the reservation the caller's `lwarx` took is gone by
  then, so the store fails without a fault and the caller's loop tries
  again. A fault inside the kernel's attempt is what reaches a hook, and it
  never comes.

So a protected page that holds a variable updated with `lwarx`/`stwcx.`
hangs the machine. The system heap has many: the ROM's atomic routines
(IncrementAtomic and others) are used by Open Transport, the Time Manager
and drivers. The same can happen in an application's partition if system
code updates a variable there atomically while the application is not
current; it has not been seen in testing.

Two ways around it:
- Change the NanoKernel so that a `stwcx.` that faults on a protected page
  reaches the blue task as an ordinary write fault. The hook opens the page
  and resumes the `stwcx.`, which fails because the reservation is gone,
  and the caller's loop then succeeds. On the Macs that load the Mac OS ROM
  file from the System Folder (as the test machine does), the kernel ships
  in that file.
- Without changing the kernel: a Multiprocessing Services task that finds
  the blue task stuck at a `stwcx.` on a protected page (from its saved
  context) and opens that page for good. Not tried; it is not known whether
  such a task gets the processor while the blue task loops.

To reproduce, push SysLog with Scribble and open SysLog; Scribble's `s` and
`S` tests store into the middle of the system heap from native and 68k
code. Use a `SCRATCH=1` guest: it hangs.

## NanoKernel

Work on the kernel starts from the kernel the test machine actually runs,
not from a rebuild. To take it out of the installed system:

```bash
kernel/extract.sh
```

It copies the Mac OS ROM file out of os9-protect's `disks/os9.qcow2`
(read only), checks its SHA-256 against the known file, unpacks it with
tbxi, and writes into `kernel/stock/` (ignored by git, since the files are
Apple's):
- `Mac OS ROM` and `dump/`, tbxi's unpacked tree, which `tbxi build` can
  turn back into a ROM file;
- `NanoKernel-v02.28.bin`, the kernel (105,280 bytes);
- `NanoKernel-v02.28.dis`, its disassembly, with addresses as offsets in
  the kernel.

The ROM file is a NewWorld bootinfo file (`MacROM for NewWorld`, dated
2002-04-17). The kernel is version 2.28, at offset 0x310000 of the 4 MB
ROM image. On the test machine it runs at physical 0x00F10000: during the
`stwcx.` hang the CPU's LR was 0x00F15EB4, which is kernel offset 0x5EB4.

The reconstructed source in os9-protect's `refs/powermac-rom/NanoKernel` is
also version 2.28 and matches the binary instruction for instruction
wherever it was compared (2026-11-12):

| Routine (source) | Offset | |
|---|---|---|
| `IntDSI`, data fault entry (`NKInterrupts.s`) | 0x32C0 | |
| `HandleDSIDueToIllegalSyncPrimitive` | 0x340C | |
| `IntAlignment` | 0x3460 | |
| Exception return: `lwarx`/`sync`/`stwcx.` clearing the reservation | 0x2C28 | |
| `FDP_053c`: emulated `lwarx` (`NKTranslation.s`) | 0x5D3C | |
| `FDP_06b4`/`FDP_06c0`: emulated `stw`/`stwcx.` | 0x5EB4/0x5EC0 | the hang |
| `MPSetAreaAccess`: the access-bits mask (`lisori r16, ~%11101111`) | 0xFC98 | |

So the source can be used to read and name the binary. It has not been
rebuilt here (it needs Apple's MPW assembler under the `empw` emulator), and
nothing yet shows that it rebuilds byte for byte. Changes to the kernel are
best made as patches to the stock binary, checked against
`NanoKernel-v02.28.dis` at each patch site.

### Booting a ROM file

The test machine boots the Mac OS ROM file in the System Folder of
os9-protect's `disks/os9-kernel.img`, a raw disk image (its README says
how to make it). To pack `kernel/stock/dump` into a ROM file and install
it, shut the machine down, then run:

```bash
kernel/pack.sh
```

```bash
kernel/install.sh
```

`pack.sh` builds the data fork with `tbxi build` and copies the resource
fork, type, creator and Finder flags from the stock file. It writes
`kernel/build/Mac OS ROM`. To pack another dump, pass its folder:
`kernel/pack.sh DUMP`.

`install.sh` replaces the file in the image's System Folder, checks the
copy's SHA-256, and removes the folders macOS adds to a volume it mounts.
It refuses to run while QEMU has the image open. The Finder cannot do this
inside the guest: the running system keeps its ROM file open, and the file
is name locked (Finder flags 0x3000).

To boot it:

```bash
DISK=disks/os9-kernel.img ../os9-protect/run.sh &
```

To check which kernel is running, dump it from guest memory and compare it
with the file you built (here the stock kernel):

```bash
../os9-protect/tools/qmp.py raw '{"execute":"pmemsave","arguments":{"val":15794176,"size":105280,"filename":"/tmp/nk-live.bin"}}'
```

```bash
cmp /tmp/nk-live.bin kernel/stock/NanoKernel-v02.28.bin
```

15794176 is 0x00F10000, where the kernel runs. The repacked stock dump
(data fork SHA-256 `0184ce4b…6d1476`, which differs from Apple's file only
in padding and tbxi's checksum strings) boots to the Finder, reports Mac OS
9.2.2 with Mac OS ROM 9.2.1, and the kernel in memory matches
`NanoKernel-v02.28.bin` byte for byte (2026-11-12).
