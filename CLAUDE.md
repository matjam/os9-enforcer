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
- Command-Option-Escape sent through QMP (`qmp.py key cmd-alt-esc`) does
  not bring up Force Quit in the guest, idle or not, so the system's own
  Force Quit cannot be tested this way.

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
    filter), is never preempted, and there is no hang recovery: an
    application that never yields at a safe point holds the processor as
    before. A preempted application gives background applications time,
    but a switch to another application in front waits until it yields
    itself.
19. Each engine start leaves one yield stub (about 1 KB) in the system
    heap.

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

Real hardware testing matters more with each kernel change, a
dual-processor G4 above all.

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
(`../os9-protect/refs/powermac-rom/NanoKernel/`, version 0x0228; three
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
../os9-protect/tools/qmp.py raw '{"execute":"pmemsave","arguments":{"val":15794176,"size":109248,"filename":"/tmp/nk-live.bin"}}'
```

```bash
cmp /tmp/nk-live.bin kernel/build/NanoKernel-v02.28.bin
```

The last 288 bytes are data the kernel writes: `task_log` (272 bytes),
then `enf_cb` (4 bytes), the physical address of the registered control
block, then patch 5's `vsid_mask`, `sys_space` (the blue task's
`AddressSpace`, physical) and `vsid_cur` (the owner whose VSIDs it
holds). `build.sh` prints the physical address of `task_log` and
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
| The scheduler moves the blue task from native code to the emulator for a 68k interrupt (`NKScheduler.s`, patch 6) | 0x146D0 |
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
