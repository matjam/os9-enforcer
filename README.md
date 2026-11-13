# os9-enforcer

Crash containment for Mac OS 9.2.2 on stock NewWorld Macs: an application
that writes into another application's memory, or into the system's, is
stopped before the write lands, and the rest of the system keeps running
without a restart.

The test machine (QEMU), its disk images, the Retro68 toolchain and the
tools that control the guest are in `os9-protect`, next to this folder.
Scripts here find it through `OS9_PROTECT` (default `../os9-protect`).

## Plan

The NanoKernel takes every page fault first, so it holds the mechanism:
who owns each page, which owner is current, and whether a write is
allowed. An extension and a control panel set the policy and tell the
user. A first prototype did everything from an extension (see
[Prototype](#prototype)); moving the mechanism into the kernel removes its
limits.

1. **Kernel tooling.** Done (2026-11-12): extract the stock kernel, repack
   the ROM file, install it on a test disk, boot it, and compare the kernel
   in memory (see [Working on the kernel](#working-on-the-kernel)).
2. **Protect the system heap without breaking the system.** Done
   (2026-11-12): patches 1 to 3 in `kernel/nanokernel.s` and a fix to the
   68k hook. Patch 1 fixes `stwcx.` on protected pages. The kernel runs a faulting store itself. For
   `stwcx.` that store failed without a fault, because the caller's
   reservation is gone, and the caller's loop retried forever. Now, when
   the kernel's `stwcx.` fails, it stores the word's current value back
   under a new reservation, which faults on a write-protected page and
   reaches the blue task. Under SysLog the patched kernel ran for 4 minutes
   and 48,000 traps without hanging; 608 of the write faults the hooks
   handled in one 40-second run came from the `stwcx.` of a ROM atomic
   dequeue loop (0xFFD88980).

   That run showed the next limit: keyboard and mouse stop within a
   minute. The CPU plugin (the machine's native module for interrupt
   routing, caches and sleep, registered with `MPRegisterCpuPlugin`) lives
   in the system heap: its code, data and stack are 13 pages at 0x0031E000
   on the test machine. The kernel runs it in SIGP mode, and a write fault
   there does not reach the blue task: the kernel abandons the plugin call
   and returns an error (`IntReturnFromSIGP`). 3,733 plugin calls failed
   that way in 40 seconds. The blue task cannot read the plugin's range
   (it is in the kernel's CoherenceGroup).

   Patch 2 keeps those pages writable: `MPSetAreaAccess` skips the pages
   of the plugin's range when a call would remove write access. With it,
   no plugin call failed (0 faults on the SIGP path, 64,250 to the hooks).

   Each run also had three write faults in Multiprocessing Services tasks,
   and a write fault in a task other than the blue task stops that task:
   - 0xFFCD5EB0, a function prologue saving registers on the task's stack
     (`stfd f31,-8(r1)`): the stack is in a protected system heap page;
   - 0xFFCD646C, ROM MP library glue storing a kernel call's results
     through pointers (`stw r4,0(r7)`, before `li r0,19; sc`);
   - 0x003138AC, code in the system heap.

   Patch 3 lets those stores through until step 3 gives the kernel page
   owners. When a task other than the blue task or the CPU plugin stores
   to a read-only page, the kernel sets the page's PTE to read/write and
   runs the store again (in `MemRetryDSI`, where the fault of a store the
   kernel runs for a task arrives). The PLE still says read-only, so the
   page is protected again when its PTE leaves the HTAB or when
   `MPSetAreaAccess` protects it again; until then every task can write to
   it, the blue task included. The kernel counts each one in `task_log`
   (see [Working on the kernel](#working-on-the-kernel)): three in a
   typical run, from two tasks, at the PCs above.

   Keyboard and mouse still stopped, within 10 to 20 seconds, and with
   them every external interrupt. os9-protect's iolog plugin, on the
   OpenPIC's acknowledge and end-of-interrupt registers, counted 17,163
   acknowledges and 17,162 ends: the kernel never ended the USB interrupt
   taken right after the 68k hook's `MPSetAreaAccess` call, and the OpenPIC
   then held back every interrupt at its priority or below. The kernel
   tells the 68k emulator about a pending interrupt with bits in the
   interrupted code's CR. The hook runs with interrupts enabled and put
   back all of CR from its entry, which cleared those bits, so the emulator
   never ran the interrupt. The hook now puts back only cr0 and cr1, the
   fields it changes (`common/h68k.s`).

   With that fix, SysLog ran for 2 minutes with keyboard and mouse working:
   53,174 write faults handled (11,877 from 68k code), every acknowledge
   ended, 20 of them taken inside the hook. Scribble's `s` and `S` stores
   into the system heap were logged as the application's code and allowed.
3. **Page owners in the kernel.** Done (2026-11-12): patch 4 in
   `kernel/nanokernel.s`, with `common/kcb.h` and changes to the engine
   and both hooks.
   - The engine allocates a control block (16 KB, physically contiguous)
     and an owner table (one byte per 4 KB page of logical RAM, 128 KB for
     512 MB) in the system heap. It registers the block's physical address
     with MP call 109, a selector the stock kernel leaves unused (there it
     returns −4, and the engine runs on its hooks alone). Owner bytes: 0
     none, 1 the system heap, 3 + slot for an application's domain. The
     engine sets them together with the hooks' bitmap, and keeps the
     current owner and the mode (pass, log, enforce) in the block.
   - On a store to a read-only page that has an owner, in any task, the
     kernel looks up the owner of the code doing the store: the PC, or for
     the 68k emulator the 68k PC (r24 − 2). Code nobody owns counts as the
     system's. A store by an application's code into a page that is not
     its own is an offense. In enforce mode the kernel denies it when the
     page is another application's and the writer is the current
     application in the blue task. The denied store reaches the hooks,
     which deny it (the kernel names the page in `HookData.kdeny`) and stop
     the application through `ExitToShell`.
   - Every other store is allowed: the kernel makes the page's PTE
     writable, runs the store again and records it in the block's ring
     (496 entries). The engine's timer takes the ring every 16 ms and
     protects the page again. Stores into the current application's own
     pages are only counted. On a switch the engine protects the previous
     application's pages at once, and the new one's pages open one fault
     at a time.
   - Results, with SysLog and then SysGuard and Scribble: the kernel
     decided every write fault, and neither hook was entered. SysLog ran 2
     minutes (7,030 ticks) with keyboard and mouse working; the kernel
     decided 49,584 stores into the system heap and none was lost.
     Scribble's `n` and `6` (native and 68k stores into the Finder) were
     logged under SysLog and denied under SysGuard, each with a clean stop.
     `b` (system code) and `o` (its own heap) were allowed; `s` and `S`
     (into the system heap) were logged and allowed. When SysGuard quits,
     the kernel's pointer to the block is 0 again.
   - Findings: on 9.2.2, MemTop (lowmem 0x108) ends a little above the
     system heap, below the applications' partitions, so the engine sizes
     the table from Gestalt `'lram'`. With virtual memory off, a system
     heap block's logical and physical addresses still differ (the block
     at logical 0x0133F000 was at physical 0x0169C000), so the engine
     translates each page with `GetPhysical`.
   - Limits, for the next steps:
     - An allowed store opens the page for every task until the engine
       protects it again, on its next tick.
     - Code outside every partition and the system heap has no owner and
       counts as the system's. On the test machine, code at 0x1E75xxxx to
       0x1E7Exxxx writes into the Finder's heap while the Finder is
       current: likely the Finder's code in the Process Manager's heap
       (step 4).
     - A store by an application that is not current, or by another task,
       is logged and allowed, never denied: only the current application
       can be stopped cleanly.
     - The control block and the owner table are system heap memory that
       any code can write.
     - The CPU plugin's pages are still kept writable by patch 2.
4. **Pages per application.** Every page has one owner:
   - An application's partition starts and ends on page boundaries. Its
     heap, stack and globals are its own.
   - Temporary memory: an application's first `TempNewHandle` assigns it a
     page in the Process Manager's heap. Its later blocks come from its
     pages, and it gets another page when they are full.
   - The system heap belongs to the system.
   - Owners are set when pages are handed out and cleared when an
     application quits, by patches to the Memory Manager and the Process
     Manager (from the extension, or in the ROM image).

   What the Process Manager's heap holds (2026-11-12, from memory dumps of
   the test machine; see [Reading guest memory](#reading-guest-memory)):
   - Partitions, temporary memory and the Process Manager's own blocks.
     The master pointers of all of them are in one block near the top of
     the heap (0x1EECxxxx on the test machine), in the order they were
     allocated, and relative handles count from 0x1EEC9564, just above
     them (the zone pointer in the Process Manager's globals).
   - Block headers are 12 bytes: a tag word (first byte 0xAB for a
     relocatable block in use, 0x6B for a nonrelocatable one, 0x10 for a
     free one; second byte the master pointer flags, 0x80 locked; last
     byte the size correction), the physical size, then the relative
     handle (relocatable), the zone (nonrelocatable) or the next free
     block (free). No header names an owner.
   - Each application's process entry is a block right after its
     partition (0x5A0 bytes): its process number, type and creator, links
     to the other entries, and its partition's pointer, handle and size.
   - System temporary memory is in a handle list (capacity, count, then
     handles) whose handle is in the Process Manager's globals (0x010BA820
     on the test machine, next to the zone pointer). The code that writes
     into the Finder's heap from outside its partition (0x1E75xxxx to
     0x1E7Exxxx) is in two of its blocks, so by the system's own records
     it is system code.
   - Where the Process Manager records an application's own temporary
     memory is not found yet: a 64 KB `TempNewHandle` by Scribble (test
     `m`) put its handle in no process entry or handle list found so far.
     A patch to `TempNewHandle` from the extension is the other way, if
     PowerPC callers reach it through the trap.
5. **Attribution.** Judge a store by the code doing it: an application's
   code fragments and code resources in its partition, or system code in
   ROM and the system heap. For 68k code that is the emulated PC (r24).
   This covers code that runs for one application while another is current
   (VBL and Time Manager tasks, I/O completion routines, Open Transport
   notifiers) and system code writing on an application's behalf. Step 3
   does this for code inside a partition; code an application owns
   outside it (in temporary memory, or loaded into the system heap for
   it) still counts as the system's.
6. **Extension and control panel.** Done (2026-11-12): the Enforcer
   Extension and the Enforcer control panel from the prototype now use the
   NanoKernel's owner check.
   - The extension registers the owner check at startup (`kcb_start`) and
     removes it when it quits. With the stock kernel it runs as before,
     without the check.
   - The control panel's third footer line says whether the owner check is
     on, with the writes checked and stopped, or that the NanoKernel is not
     patched (MP call 109 returns −4).
   - New option, "Also log writes into the system heap": log only, and
     enabled only when the owner check is on. "Enforcer Preferences" keeps
     it next to the mode; a file from an earlier version reads as off.
   - Boot test on `disks/os9-kernel-enforcer.img`, a copy of
     `os9-kernel.img` with both installed: the extension starts at boot
     and registers the check. Scribble `n` is stopped cleanly, with the
     notification naming Scribble, the Finder and the address. After a
     restart the system heap option is still on.
   - With the system heap option on, Desktop Printer Spooler and ATI Video
     Accelerator store into the system heap at startup from their own code
     (20 stores in the first second). `tools/xlog-read.py` lists these as
     "logged"; they are never denied.
   - One store at startup was recorded as an offense between two
     applications (Desktop Printer Spooler into ATI Video Accelerator) but
     not denied. The engine refreshes the owner table every 30 ticks, so a
     partition just created can still have a stale owner. Check this when
     the Process Manager hooks of step 4 set owners at once.
7. **Installer for users.** Done (2026-11-12): `installer/`, Enforcer
   Installer. It patches the user's own Mac OS ROM file, so no Apple bytes
   ship: `installer/kpatch.h` holds only the 939 bytes that differ between
   the stock kernel and ours (`tools/make-kpatch.py`).
   - It accepts any Mac OS ROM file whose ROM image holds NanoKernel 2.28
     as Apple made it (CRC-32 of its 105,280 bytes, zeros after it), and
     recognizes one it has patched.
   - `installer/rompatch.c`:
     - Checks both Adler-32 checksums and the image's CRC-32.
     - Decompresses the 4 MB ROM image and finds the kernel through
       ConfigInfo.
     - Applies the patch, sets the image's lane checksums, and compresses
       it again.
     - Moves the parcels after the image, and sets the boot script's
       `parcels-size` and `info-size` and both checksums.
     - The System Enabler moves too (by 720 bytes); the installer moves the
       data fork offsets in the resource fork's `cfrg` to match.
   - `installer/lzss.c` is Okumura's LZSS. It gives back Apple's
     compressed image byte for byte, so the patched file differs from the
     one `kernel/build.sh` makes with tbxi only in padding bytes between
     parcels. To test it on the host, run `tools/rompatch-test.sh`.
   - Install writes the patched copy into the System Folder, gives it type
     `tbxi`, moves the original into "Enforcer Backup" at the top of the
     startup disk, and renames the copy. The File Manager renames and
     moves the open Mac OS ROM file; Mac OS goes on using it until the
     restart. Then Install copies the Enforcer Extension and control panel
     from its own folder; a running extension goes to the Trash first.
   - Remove checks that the backup has the stock kernel, puts it back, and
     moves the patched file, the extension and the control panel to the
     Trash.
   - Test on `disks/os9-installer.qcow2` (an overlay on `os9-novm.qcow2`,
     stock ROM file):
     - Install took under 10 seconds.
     - After the restart the extension registered the kernel's owner check.
     - Install again replaced the running extension.
     - After Remove and a restart, the stock instruction was back at
       0x3630.
     - The Trash, with the patched file, then emptied.
   - Recovery without the installer: start up from a Mac OS 9 CD and drag
     "Mac OS ROM" from "Enforcer Backup" into the System Folder, replacing
     the patched one.
   - OldWorld Macs load the kernel from a `krnl` resource in the System
     file instead, which needs its own path. Not done.

Not covered:
- An overrun inside an application's own memory. It can only damage that
  application.
- Writes by device DMA, which do not go through the MMU.
- Virtual memory. Keep it off until the kernel's pager and the owner table
  are known to agree.

Later, as an opt-in debug mode for one application: a protected page after
each large block, to catch an overrun past its end when it happens.

## Prototype

The first version is a startup extension. It write-protects other
applications' partitions with the NanoKernel's area calls and catches the
faults with hooks in front of the 68k emulator's and the Exception
Manager's handlers. Its full description is in this README up to commit
c491006 (`git show c491006:README.md`).

What it showed, which the plan depends on:
- Page protection bits 11 make a page read-only for all code, the kernel
  included. Bits 01 (read-only in user mode) do not stop the blue task's
  stores on 9.2.2, so protection bits cannot tell Mac OS code from the
  kernel or from an application.
- A 68k store to a protected page never reaches the Exception Manager. The
  emulator takes the fault: the application gets a bus error, or at
  interrupt time the machine hangs.
- Hooks in the blue task see only its faults. A write fault in a
  Multiprocessing Services task stops that task without reaching them
  (patch 3 lets those stores through; with patch 4 the kernel decides
  them).
- An exception handler in the 68k emulator's context runs with external
  interrupts enabled (the kernel disables them for native-context
  handlers only). The kernel's interrupt handler tells the emulator about
  a pending interrupt by setting bits in the interrupted code's CR (cr2,
  mask 0x00E00000 at KDP + 0x674 on the test machine). A handler that puts
  back all of the CR it saved clears them; the interrupt is then never run
  or ended, and the OpenPIC stops every interrupt at its priority or below
  (step 2).
- The Process Manager, Window Manager and Layer Manager write into other
  applications' memory on every switch, so a rule has to look at the code
  doing the store.
- One trap costs about 70 µs on the QEMU test machine (Apple Silicon
  host), including putting the protection back; most of it is likely QEMU
  flushing its TLB.
- Protecting the system heap page by page costs 1 to 3% of the CPU in
  interactive use if the few dozen pages the system writes all the time
  are left open ("Store measurement" in os9-protect's README).
- With the stock kernel, a protected page holding a variable updated with
  `lwarx`/`stwcx.` hangs the machine. With the system heap protected, the
  CPU looped in the ROM's atomic increment at 0xFFD88A3C on a protected
  page, and no hook saw a fault. Patch 1 fixes this (step 2 of the plan).
- A write violation in the blue task reaches the hooks only while the
  blue task has exceptions enabled; the kernel counts every one by cause
  (`tools/faults-read.py` compares the counts). To see which path each one
  takes, use os9-protect's pcwatch plugin at the kernel's delivery
  decision, physical 0x00F12A14 (offset 0x2A14 in `ExceptionMemRetried`):
  `regs=r9:r10,crmask=0x82208000` records the cause, the faulting PC and
  the path (0x80200000 blue task, 0x00008000 CPU plugin, 0 another task).

| Directory | What it is |
|---|---|
| `common/` | The protection engine, the 68k and native write hooks, NanoKernel calls |
| `enforcer/` | Enforcer Extension and the Enforcer control panel |
| `installer/` | Enforcer Installer: patches the Mac OS ROM file, installs and removes Enforcer |
| `xlog/` | XLog, Guard, SysLog and SysGuard: console front ends on the engine |
| `scribble/` | Scribble: stores into other memory, to test the engine |
| `h68k/` | H68KTest: tests for both write hooks |
| `wlog/` | WLog: the first write logger |
| `probe/` | Probe: the first page protection experiment |
| `tools/` | Host scripts that read the engine's and hooks' state and the guest's memory, and build helpers |

To build them, run `./build.sh`, or name directories:
`./build.sh xlog scribble`. Each builds a MacBinary `.bin` in its `build/`
folder with the toolchain in `$OS9_PROTECT/toolchain`. The `enforcer/`
build also needs Python 3. To run them, boot a guest with virtual memory
off and copy them in:

```bash
SCRATCH=1 DISK=disks/os9-novm.qcow2 ../os9-protect/run.sh &
```

```bash
../os9-protect/tools/push.sh -n Test xlog/build/SysLog.bin scribble/build/Scribble.bin
```

To replace a running Enforcer Extension, quit it first: the Finder cannot
replace a file that is in use. In Script Editor (Apple Extras, AppleScript),
run `tell application "Enforcer Extension" to quit`. After copying the new
one in, open it to start it, or restart. (Holding Shift from the host to
start up with extensions off does not work: the guest does not see a key
held down before its USB keyboard driver loads.)

### Icons

The icon is a blue shield with a gold rim and a memory chip on its face.
The extension's icon stands the shield on a puzzle piece. To change them,
edit `tools/make-icons.py` and run it: it rewrites `enforcer/icon-panel.r`
and `enforcer/icon-extension.r`. With `-p DIR` it also writes a preview
image of every size and color depth to `DIR/icons.png`.

Each program carries its icons twice:
- The icon family at ID 128, which its bundle (`BNDL` and `FREF` in
  `panel.r` and `extension.r`) names for the Finder's desktop database.
  The desktop database keeps only up to 256 colors and a 1-bit mask.
- The same `icns` at ID −16455, the file's custom icon. Icon Services
  draws that in 32-bit color with soft edges, also on a CD, where the
  Finder cannot register a bundle.

Retro68's Rez clears every Finder flag, so the build sets hasBundle and
hasCustomIcon in the MacBinary header with `tools/finder-flags.py`.
`push.sh` (`hcopy`) keeps both flags.

## Reading guest memory

`tools/xlog-read.py` and `tools/hooks-read.py` read logical memory with
QEMU's memsave, which goes through the hashed page table: a page whose PTE
is not in it does not read. That covers every page the engine protects
(protecting a page removes its PTE) and any page not used lately. For
small structures the engine and hooks touch all the time that is enough.

To read anything else, use `tools/lmem.py`. It takes a physical dump and
reads it through the map from logical to physical memory, which with
virtual memory off is a few linear runs: on the test machine (512 MB),
logical + 0x4000 below 0x00BFC000 and logical + 0x35D000 from 0x00CB6000
up. Logical and physical addresses differ, so a physical dump read at
logical addresses gives other memory. For example, to print the Process
Manager's globals and find where a handle is stored:

```bash
tools/lmem.py dump 0x010BA800 0x40
```

```bash
tools/lmem.py find 0x1EEC9520
```

The physical dump takes a few seconds. memsave reads that go page by page
over large ranges pause the guest for minutes.

## Working on the kernel

The scripts are in `kernel/`. They write into `kernel/stock/` and
`kernel/build/`, which git ignores, since the files are Apple's.

To take the kernel out of the test machine's installed system:

```bash
kernel/extract.sh
```

It copies the Mac OS ROM file from os9-protect's `disks/os9.qcow2` (read
only), checks its SHA-256, unpacks it with tbxi into `kernel/stock/dump/`,
and writes `NanoKernel-v02.28.bin` and its disassembly
`NanoKernel-v02.28.dis` (addresses are offsets in the kernel).

`kernel/nanokernel.s` is the patched kernel: the stock binary, copied in
with `.incbin`, and each patch assembled at its offset, with new code
after the stock kernel. To build it and pack it into a ROM file, run:

```bash
kernel/build.sh
```

It prints every changed or added instruction (a diff of the stock and
patched disassemblies) and writes `kernel/build/Mac OS ROM`. To install
that file on the kernel test disk, shut the machine down and run:

```bash
kernel/install.sh
```

`install.sh` writes the file into the System Folder of os9-protect's raw
`disks/os9-kernel.img`, checks both forks, and refuses while QEMU has the
image open. The Finder cannot replace the file in a running guest: the
system keeps it open, and it is name locked. (Enforcer Installer can: see
step 7.)

After changing the kernel, write the installer's patch again and rebuild
it:

```bash
tools/make-kpatch.py && tools/rompatch-test.sh && ./build.sh installer
```

`build.sh` packs with `kernel/pack.sh DUMP`, which also packs the
unmodified dump on its own (`kernel/pack.sh`). tbxi builds both forks. The
resource fork has to come from tbxi: its `cfrg` resource gives the data
fork offsets of the System Enabler's code fragments, which follow the
compressed ROM image and move when its compressed size changes. With the
stock resource fork, a patched ROM stops during startup with "Sorry, a
system error occurred. unimplemented trap". For the stock dump, tbxi's
resource fork has the same resources as Apple's (types, IDs, names,
attributes and data).

To boot it (`SCRATCH=1` keeps the disk unchanged):

```bash
SCRATCH=1 DISK=disks/os9-kernel.img ../os9-protect/run.sh &
```

To check which kernel is running, dump it from guest memory and compare it
with the one you built (`build.sh` prints its size; 106,480 bytes with
patches 1 to 4):

```bash
../os9-protect/tools/qmp.py raw '{"execute":"pmemsave","arguments":{"val":15794176,"size":106480,"filename":"/tmp/nk-live.bin"}}'
```

```bash
cmp /tmp/nk-live.bin kernel/build/NanoKernel-v02.28.bin
```

The last 276 bytes are data the kernel writes: `task_log` (272 bytes),
then `enf_cb` (4 bytes). Once either is written, `cmp` reports a
difference there.

The repacked stock dump and the kernel with patches 1 to 4 boot to the
Finder, and the kernel in memory matches the build byte for byte
(2026-11-12).

`build.sh` prints the physical addresses of both. `enf_cb` (0x00F29FEC
now) holds the physical address of the registered control block, or 0. To
read it:

```bash
../os9-protect/tools/qmp.py hmp 'xp /1wx 0xF29FEC'
```

To read the control block, its counts and its last events from the
engine's side, use `tools/xlog-read.py` while an engine runs.

With no control block registered, or in pass mode, patch 3 records each
page it opens for a task other than the blue task in `task_log` (0x00F29EDC
now). It is a count, three zero words, then 16 entries of four words: the
task's PC, the address, the task's ID and the page's new PTE (lower word).
The latest entry is number (count − 1) mod 16. To read it:

```bash
../os9-protect/tools/qmp.py raw '{"execute":"pmemsave","arguments":{"val":15900380,"size":272,"filename":"/tmp/task-log.bin"}}'
```

```bash
xxd -g4 -c16 /tmp/task-log.bin
```

### The stock kernel

- The Mac OS ROM file is from the 9.2.2 install (`MacROM for NewWorld`,
  dated 2002-04-17). Mac OS reports it as Mac OS ROM 9.2.1.
- The kernel is NanoKernel 2.28, 105,280 bytes, at offset 0x310000 of the
  4 MB ROM image. On the test machine the image is at physical 0x00C00000,
  so the kernel runs at 0x00F10000 (15794176 above).
- Free space: from the end of the kernel (0x329B40) to the 68k emulator
  (0x360000) the image holds 222,400 zero bytes, also zero in the running
  machine. The kernel already runs past its 64 KB `KernelCodeSize`, over
  the hardware init slot at 0x320000, which NewWorld does not use. Put new
  code there, below 0x360000, and writable data in RAM allocated at boot.
  The 16 KB after the exception table is not free: ConfigInfo is at
  0x30D000.
- os9-protect's `refs/powermac-rom/NanoKernel` reconstructs version 2.28
  and matches the binary wherever compared, so use it to read and name the
  binary. It has not been rebuilt (it needs MPW under `empw`). Patch the
  stock binary and check each site against the disassembly.

| Routine (source) | Offset |
|---|---|
| `IntDSI`, data fault entry (`NKInterrupts.s`) | 0x32C0 |
| `MemRetryDSI`: fault in a store the kernel runs; read-only page check (patch 3) | 0x35A0; 0x3630 |
| `HandleDSIDueToIllegalSyncPrimitive` | 0x340C |
| `IntAlignment` | 0x3460 |
| Exception return: `lwarx`/`sync`/`stwcx.` clearing the reservation | 0x2C28 |
| `FDP_053c`: emulated `lwarx` (`NKTranslation.s`) | 0x5D3C |
| `FDP_06b4`/`FDP_06c0`: emulated `stw`/`stwcx.` | 0x5EB4/0x5EC0 |
| `MPCreateArea`: the area flags check (`lisori r16, ~%11101111`) | 0xFC98 |
| `MPSetAreaAccess` (`NKAddressSpaces.s`): `@pageloop` (patch 2) | 0x10B68 |
| `ExceptionMemRetried`; its choice of path (pcwatch here) | 0x2980; 0x2A14 |
| `SIGP`: CPU plugin call (`NKInterrupts.s`); `IntReturnFromSIGP` | 0x48E0; 0x4A38 |
| `IntExternalSystem`: external interrupt entry | 0x4880 |
| PIH (OpenPIC): acknowledge; end of interrupt; CR bits for the emulator | 0x16068; 0x16028; 0x15524 |
| MP call table (entry n: routine − 4n); entry 109 (patch 4); `MPCallBad` | 0xACB8; 0xAE6C; 0xAF0C |
| `CommonMPCallReturnPath` (result in r3) | 0xB124 |
| `GetPTEFromPLE`; `AcquireLock` | 0x124F8; 0x2700 |

Kernel structures the patches use: the CoherenceGroup pointer is the word
at EWA − 0x338 (`mfsprg rX,0` gives the EWA); in the CoherenceGroup,
`LA_CpuPlugin` is at +0x28, `CpuPluginSize` at +0x30 and
`CpuPluginSpacePtr` at +0x4C; in an Area, `PageSize` is at +0x68 and
`AddressSpacePtr` at +0x6C. On the test machine the CoherenceGroup is at
physical 0x1F7FC1C0, with its signature `CGRP` in its first word.
`MemRetryDSI` saves a task's r8, r9, r16 (the flags) to r19 and CR at
EWA − 0xE0 to − 0xC8; the HTAB lock is at KDP − 0xB90 (the KDP pointer
is the word at EWA − 4). On the test machine the EWA and the KDP are at
physical 0x1F7FE000.
