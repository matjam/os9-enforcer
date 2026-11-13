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
2. **Fix `stwcx.` on protected pages.** Done (2026-11-12), patch 1 in
   `kernel/nanokernel.s`. The kernel runs a faulting store itself. For
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

   Keyboard and mouse still stop, within 35 seconds. Each run also has
   three write faults in Multiprocessing Services tasks, and a write fault
   in a task other than the blue task stops that task:
   - 0xFFCD5EB0, a function prologue saving registers on the task's stack
     (`stfd f31,-8(r1)`): the stack is in a protected system heap page;
   - 0xFFCD646C, ROM MP library glue storing a kernel call's results
     through pointers (`stw r4,0(r7)`, before `li r0,19; sc`);
   - 0x003138AC, code in the system heap.

   The stopped tasks are the likely reason input stops (the guest's
   keyboard and mouse are USB), not yet shown. The fix is for the kernel
   to handle write faults in every task the way the hooks do for the blue
   task, which is step 3's job. Scribble's `s` and `S` tests wait on it.
3. **Page owners in the kernel.**
   - Write faults in every task go through the owner check, not only the
     blue task's: today an MP task's write fault stops the task.
   - The CPU plugin's pages (from `MPRegisterCpuPlugin`) belong to the
     kernel and are never protected (patch 2 does this for now).
   - A table in RAM with one owner byte per 4 KB page (128 KB for 512 MB),
     allocated at boot. Owners: free, system, low memory, one per
     application.
   - Kernel calls to set the owner of a range of pages and to set the
     current owner.
   - On a write fault the kernel checks the page's owner against the
     current owner and the code doing the store, then allows, logs or
     denies it. A denied store reaches the blue task as a write fault, and
     the extension stops the application.
   - Open question: on a switch, change page protection all at once, or
     page by page as faults come in.
   - New code goes in the free space after the kernel in the ROM image,
     assembled with the Retro68 toolchain, with branches from the patch
     sites.
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
5. **Attribution.** Judge a store by the code doing it: an application's
   code fragments and code resources in its partition, or system code in
   ROM and the system heap. For 68k code that is the emulated PC (r24).
   This covers code that runs for one application while another is current
   (VBL and Time Manager tasks, I/O completion routines, Open Transport
   notifiers) and system code writing on an application's behalf.
6. **Extension and control panel.** Carry over from the prototype: start
   at boot, policy (off, log, stop), a clean stop through `ExitToShell`,
   and a notification naming both applications.
7. **Installer for users.**
   - Ship a patcher, not a ROM file (Apple's file cannot be
     redistributed). It checks the user's Mac OS ROM file against a
     supported SHA-256, unpacks its parcels (LZSS), patches the kernel,
     repacks it, and keeps the original for uninstall.
   - Replace the open, name-locked file on the running system through the
     File Manager: rename it, move it out of the System Folder, write the
     new file, restart. Needs a test; the Finder refuses.
   - Recovery: boot from the install CD and put the original back.
   - Support Mac OS ROM 9.2.1 (Mac OS 9.2.2, NanoKernel 2.28) first.
     OldWorld Macs load the kernel from a `krnl` resource in the System
     file instead, which needs its own path.

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
  Multiprocessing Services task stops that task without reaching them.
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
| `xlog/` | XLog, Guard, SysLog and SysGuard: console front ends on the engine |
| `scribble/` | Scribble: stores into other memory, to test the engine |
| `h68k/` | H68KTest: tests for both write hooks |
| `wlog/` | WLog: the first write logger |
| `probe/` | Probe: the first page protection experiment |
| `tools/` | Host scripts that read the engine's and hooks' state from the guest |

To build them, run `./build.sh`, or name directories:
`./build.sh xlog scribble`. Each builds a MacBinary `.bin` in its `build/`
folder with the toolchain in `$OS9_PROTECT/toolchain`. To run them, boot a
guest with virtual memory off and copy them in:

```bash
SCRATCH=1 DISK=disks/os9-novm.qcow2 ../os9-protect/run.sh &
```

```bash
../os9-protect/tools/push.sh -n Test xlog/build/SysLog.bin scribble/build/Scribble.bin
```

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
system keeps it open, and it is name locked.

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
with the one you built (its size is in `build.sh`'s last line; 105,404
bytes with patches 1 and 2):

```bash
../os9-protect/tools/qmp.py raw '{"execute":"pmemsave","arguments":{"val":15794176,"size":105404,"filename":"/tmp/nk-live.bin"}}'
```

```bash
cmp /tmp/nk-live.bin kernel/build/NanoKernel-v02.28.bin
```

The repacked stock dump and the kernel with patches 1 and 2 boot to the
Finder, and the kernel in memory matches the build byte for byte
(2026-11-12).

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
| `HandleDSIDueToIllegalSyncPrimitive` | 0x340C |
| `IntAlignment` | 0x3460 |
| Exception return: `lwarx`/`sync`/`stwcx.` clearing the reservation | 0x2C28 |
| `FDP_053c`: emulated `lwarx` (`NKTranslation.s`) | 0x5D3C |
| `FDP_06b4`/`FDP_06c0`: emulated `stw`/`stwcx.` | 0x5EB4/0x5EC0 |
| `MPCreateArea`: the area flags check (`lisori r16, ~%11101111`) | 0xFC98 |
| `MPSetAreaAccess` (`NKAddressSpaces.s`): `@pageloop` (patch 2) | 0x10B68 |
| `ExceptionMemRetried`; its choice of path (pcwatch here) | 0x2980; 0x2A14 |
| `SIGP`: CPU plugin call (`NKInterrupts.s`); `IntReturnFromSIGP` | 0x48E0; 0x4A38 |

Kernel structures the patches use: the CoherenceGroup pointer is the word
at EWA − 0x338 (`mfsprg rX,0` gives the EWA); in the CoherenceGroup,
`LA_CpuPlugin` is at +0x28, `CpuPluginSize` at +0x30 and
`CpuPluginSpacePtr` at +0x4C; in an Area, `PageSize` is at +0x68 and
`AddressSpacePtr` at +0x6C. On the test machine the CoherenceGroup is at
physical 0x1F7FC1C0, with its signature `CGRP` in its first word.
