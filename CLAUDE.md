# os9-enforcer: development notes

`README.md` describes Enforcer for its users. Keep it short. This file
holds what you need to build, test and change Enforcer, and what was
learned on the way.

## Conventions

- The test machine (QEMU), its disks and the tools that drive the guest
  are in `os9-protect`, next to this repository. Scripts here find it
  through `OS9_PROTECT` (default `../os9-protect`).
- Commit each working step to `main`. The remote, `origin`, is the
  private GitHub repository matjam/os9-enforcer.
- Commit only our own sources, scripts and patches. Apple's files stay out
  of git: `kernel/stock/` and `kernel/build/` are ignored, and the
  installer carries only the bytes that differ from the stock kernel
  (`installer/kpatch.h`).
- Write docs instruction first ("To X, use Y"), in plain technical prose.
- When a hole is fixed or found, update both the Known holes list here and
  the Limits section of `README.md`.

## Build

To build every program, run `./build.sh`. To build some, name their
directories: `./build.sh enforcer scribble`. Each builds a MacBinary `.bin`
in its `build/` folder. The `enforcer/` build also needs Python 3.

`common/hooks.cmake` (`hooks_embed`) assembles the write hooks into each
engine program and links DriverServicesLib.

The toolchain is Retro68's PowerPC toolchain (GCC 16, Multiversal
Interfaces). `toolchain/find.sh` picks one for `build.sh`,
`kernel/build.sh` and `tools/make-dist.sh`, in this order:
1. `RETRO68`, if set;
2. `toolchain/build/toolchain`, built by `toolchain/build.sh` from the
   `toolchain/Retro68` submodule;
3. os9-protect's.

The submodule is pinned to the commit os9-protect's toolchain was built
from (a4a389c41). To move it, check out the new commit in
`toolchain/Retro68`, commit the submodule, and rebuild both toolchains.

Toolchains built on different hosts do not build byte-identical
programs; our own code comes out the same, the C library linked in does
not:
- `toolchain/build.sh` on macOS: the extension and the control panel
  match os9-protect's build. In the installer, newlib's `dtoa` read-only
  data is 64 bytes longer, which moves the data after it.
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
the artifact "Enforcer". It caches the toolchain under the submodule
commit and the hash of `toolchain/build.sh`; either change rebuilds it.

## Test machine

To boot a disk, run from `os9-protect`:

```bash
DISK=disks/os9-vmoff.qcow2 ./run.sh &
```

To test with more RAM, set `MEM` (megabytes, default 512; Mac OS 9 uses
up to 1536). `xlog-read.py` cannot read the engine's state with virtual
memory on; use the control panel there.

`run.sh` starts QEMU again from the hard disk whenever the guest restarts.
`SCRATCH=1` discards disk writes, so it also loses an install at the
installer's restart; to keep one, use an overlay disk instead.

| Disk (in `os9-protect/disks/`) | What it is |
|---|---|
| `os9.qcow2` | The base install of Mac OS 9.2.2 |
| `os9-novm.qcow2` | Overlay on `os9.qcow2`, virtual memory off |
| `os9-installer.qcow2` | Overlay on `os9-novm.qcow2`, stock ROM file, for installer tests |
| `os9-pool.qcow2` | Overlay on `os9-installer.qcow2`, virtual memory on (990 MB, 512 MB of RAM) |
| `os9-vmoff.qcow2` | Overlay on `os9-installer.qcow2`, virtual memory off |
| `os9-kernel.img` | Raw image for kernel builds (`kernel/install.sh`) |
| `os9-kernel-enforcer.img` | Copy of `os9-kernel.img` with the extension and panel |

To copy files into the running guest, use `push.sh`. It builds an HFS
image and inserts it as the CD:

```bash
../os9-protect/tools/push.sh -n Test enforcer/build/"Enforcer Extension.bin" scribble/build/Scribble.bin
```

To drive the guest, use `../os9-protect/tools/qmp.py` (`screenshot FILE`,
`click X Y`, `dclick X Y`, `type TEXT`, `key NAME`, `hmp CMD`, `raw JSON`).

To install or update Enforcer in the guest, push Enforcer Installer, the
Enforcer Extension and Enforcer (the control panel) together, open the
installer, click Install, then Restart. The Finder cannot replace a running
extension; the installer moves it to the Trash first.

To read the engine's state, counts, domains and offense log while it runs,
use `tools/xlog-read.py`.

### Scribble

Scribble (`scribble/`) makes the stores the engine has to judge. Type a
letter and Return. Each store writes the value already there, so an
allowed store changes nothing.

| Key | Store | Expected in enforce mode |
|---|---|---|
| `n` | native code into the Finder | denied, Scribble stopped cleanly |
| `6` | 68k code into the Finder | denied, Scribble stopped cleanly |
| `b` | `BlockMoveData` (system code) into the Finder | allowed |
| `o` | into its own heap | allowed |
| `s`, `S` | native, 68k into the system heap | logged, allowed |
| `t` | time 2 s of trapped writes | prints the cost of one trap |
| `m`, `M` | 64 KB kept, 1 MB freed, of temporary memory | in Scribble's own chunks |
| `T` | native code into the Finder's temporary memory | denied, Scribble stopped |
| `P` | write and read back 600 MB of temporary memory | pages out other applications (VM on) |
| `Y` | an MP task into the Finder | dropped by the kernel, Scribble goes on |
| `V` | a Time Manager task into a background-only application, 5 s later; make the Finder current | dropped by the kernel |
| `D` | a deferred task into the Finder, Scribble current | skipped by the engine, Scribble goes on |
| `X` | make the Enforcer Extension call `ExitToShell` without `engine_stop` | engine stops itself, control block unregistered |

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
  unused (it returns −4 there). The kernel takes only `CB_VERSION` 2 and
  returns paramErr for another, so a mismatched extension and kernel
  protect nothing. Owner bytes: 0 none, 1 the system heap, 3 + slot for an
  application's domain.
- On a store to a read-only page that has an owner, in any task, the kernel
  looks up the owner of the code doing the store: the PC, or for the 68k
  emulator the 68k PC (r24 − 2). Code nobody owns is the system's. A store
  by an application's code into a page that is not its own is an offense.
  In enforce mode (`task_write`):
  - into the current application's page: allowed and logged (it can be a
    callback the current application called);
  - by the current application in the blue task, with the 68k interrupt
    mask at 0: denied. The denied store reaches the hooks, which deny it
    too (`HookData.kdeny` names the page) and stop the application;
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

### The engine (`common/engine.c`, `common/engine.h`)

- One domain per application: its partition's whole pages, its temporary
  memory chunks, and with virtual memory on its main code section. The
  engine protects every domain but the current one, and on a switch
  protects the previous application's pages at once.
- Current is the Process Manager's current process (`ApplZone`), the one
  whose code runs in the blue task between `WaitNextEvent` calls, in the
  foreground or the background. `check_switch` reads it on each tick.
  The new current application's pages are not opened at the switch: its
  first store into each page faults, the kernel allows it, and the page
  stays open (`kernel_event` protects again only pages outside the
  current domain) until the application is switched out.
- The span is 2 GB (`kSpanHi`): with 1.5 GB of RAM the Process Manager
  places applications at about 0x5E000000, from the top of RAM down. The
  owner table covers logical RAM, plus 256 MB with virtual memory on.
- The partly used first page of a partition, where the heap and often the
  code start, gets the application's owner byte only, unprotected
  (`own_first_page`). Without it, code there was the system's.
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
3. A store the kernel allows opens the page for every task until the next
   tick (16 ms).
4. The partly used first and last pages of a partition are not protected,
   and the last has no owner. The last holds the top of the application's
   A5 world.
5. With virtual memory on, code in an application's other fragments, and
   all of its code when `find_code` finds no match, is the system's.
6. An application launched since the last refresh (up to half a second)
   has no domain until its first event or `TempNewHandle`.
7. The control block and the owner table are in the system heap, where any
   code can write them.
8. The system heap is log only.
9. An application's own patch of `_OSDispatch`, `DisposeHandle` or
   `ExitToShell`, made before the engine started, goes around the
   engine's stub while that application is current.
10. With virtual memory on, a protect can be lost to `setPTEntryGivenPage`
    until the engine protects the page again.
11. The engine sees a switch on its timer's tick (`check_switch` reads
    `ApplZone`), so for up to 16 ms after a switch `CB_CURRENT` still
    names the previous application:
    - a store by the previous application's code into the new current one
      is denied and the new one stopped. Possible; not observed;
    - a store by the new application's code into the previous one is
      allowed, and the previous one's open pages take it without a fault.
12. `engine_stop` puts back the Exception Manager's system-wide handler
    without checking that it is still Enforcer's. A handler installed
    after Enforcer's is dropped.
13. A temporary handle in a chunk grows only inside the chunk:
    `SetHandleSize` can fail where the Process Manager's heap had room.
14. Crash cleanup is tested with Scribble `X` (a direct `ExitToShell`). A
    real crash of the extension, or a Force Quit, that does not reach
    `ExitToShell` through the trap table is not covered or tested.

Compatibility review (2026-11-13) also noted: programs that write-protect
their own pages lose those faults to the kernel and the hooks; the engine
leaks about 75 KB of system heap per start.

Later, as an opt-in debug mode for one application: a protected page after
each large block, to catch an overrun when it happens.

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
any page not used lately.

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
../os9-protect/tools/qmp.py raw '{"execute":"pmemsave","arguments":{"val":15794176,"size":106480,"filename":"/tmp/nk-live.bin"}}'
```

```bash
cmp /tmp/nk-live.bin kernel/build/NanoKernel-v02.28.bin
```

The last 276 bytes are data the kernel writes: `task_log` (272 bytes),
then `enf_cb` (4 bytes), the physical address of the registered control
block. `build.sh` prints the physical address of both. `task_log` is a
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
- os9-protect's `refs/powermac-rom/NanoKernel` reconstructs version 2.28;
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
| `GetPTEFromPLE`; `AcquireLock` | 0x124F8; 0x2700 |

Kernel structures the patches use: the CoherenceGroup pointer is the word
at EWA − 0x338 (`mfsprg rX,0` gives the EWA); in the CoherenceGroup,
`LA_CpuPlugin` is at +0x28, `CpuPluginSize` at +0x30 and
`CpuPluginSpacePtr` at +0x4C; in an Area, `PageSize` is at +0x68 and
`AddressSpacePtr` at +0x6C. `MemRetryDSI` saves a task's r8, r9, r16 (the
flags) to r19 and CR at EWA − 0xE0 to − 0xC8; the HTAB lock is at
KDP − 0xB90 (the KDP pointer is the word at EWA − 4).

To see which path a write violation takes, use os9-protect's pcwatch
plugin at physical 0x00F12A14 (`ExceptionMemRetried`'s choice):
`regs=r9:r10,crmask=0x82208000` records the cause, the faulting PC and the
path (0x80200000 blue task, 0x00008000 CPU plugin, 0 another task).

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
