# Enforcer

Enforcer adds memory protection between applications, preemption, and
memory on demand to Mac OS 9.2.2 on NewWorld Macs. It is a patch to the
Mac OS kernel, an extension and a control panel.

Enforcer has been tested only on QEMU's `mac99` machine, not on real
hardware.

## AI use disclosure

Enforcer was written almost entirely with AI (Anthropic's Claude),
working under my direction. I am not doing all of this by hand: that
would be crazy. Mac OS 9's internals are ancient, mostly undocumented,
and hardly anyone remembers how they work, so the kernel patches come
from reading disassembly and reconstructed NanoKernel sources, and then
testing until things held up.

Does it work? It seems to. Every feature here was tested in an emulator
(QEMU), with test programs written to misbehave on purpose and with the
applications that come with Mac OS 9.2.2. What has not happened yet:

- It has not run on a real Mac.
- It has not been used day to day for any length of time.
- No third-party extension or application has been tried with it.

So yes, it is vibecoded, or whatever other name you like. I am fine with
that as long as it works and breaks nothing. But it patches the kernel
of your operating system, so treat it accordingly: back up first, and
keep a Mac OS 9 CD at hand in case the Mac does not start up (see
Remove). It may eat your dog, copy your homework or burn your house
down. No warranty; see `LICENSE`.

If you try it on real hardware, I would like to hear how it went, good
or bad: please open an issue.

## Why

Mac OS 9 has three limits that later systems removed. Enforcer addresses
each one without changing how applications are written.

- **One application can take the whole Mac down.** All applications and
  the system share one address space, and nothing stops an application
  from writing into another's memory. A bug in one application damages
  another, or the system, and the usual result is a restart and lost
  work in every open application.
- **One application can hold the processor.** Multitasking is
  cooperative: an application runs until it calls `WaitNextEvent`. An
  application that computes for a long time freezes the others.
- **Memory is fixed at launch.** Each application gets one block of
  memory whose size is typed into its Get Info window. An application
  reports "out of memory" when that block is full, however much the Mac
  has free, and memory an application does not use is not available to
  others.

## What it does

- **Crash containment.** When an application writes into another
  application's memory, Enforcer blocks the write and stops the
  application that made it. The other applications and the system keep
  running, and you do not need to restart.
- **Preemption.** An application whose own PowerPC code runs for 20 ms
  without calling `WaitNextEvent` is made to call it. Other applications
  get time, and the application then goes on where it was.
- **Memory on demand.** The sizes in Get Info stop being a hard limit:
  - An application that asks for 640 KB or more is launched with a
    29 MB partition. The partition uses RAM only where the application
    touches it, and gives the RAM back when the application quits.
  - A smaller application is launched with up to four times its
    preferred size.
  - When an application's partition is full, a new block comes from
    temporary memory that the application owns.

## How it works

Mac OS 9 on a NewWorld Mac already runs on a small kernel, the
NanoKernel, which is stored in the Mac OS ROM file and controls the
processor's memory management unit (MMU). Mac OS uses it for virtual
memory and for Multiprocessing Services, but not to protect applications
from each other. Enforcer adds that.

- **The kernel patch.** Enforcer Installer patches the NanoKernel in the
  Mac OS ROM file. The patched kernel keeps an owner for each 4 KB page
  of memory: the system, or one application. Without the extension
  running, the patched kernel behaves as Apple's.
- **Address spaces.** Each application runs with its own view of the
  memory that holds applications. In it, the application's own pages are
  writable and every other application's pages are read-only. An
  application switch changes which view the processor uses, so the cost
  of a switch does not depend on the applications' sizes.
- **Deciding a write.** A write to a read-only page stops in the kernel,
  which looks up the owner of the page and the owner of the code doing
  the write. A write by the system is allowed. A write by an
  application's code into another application's page is denied: the
  memory is not changed.
- **Stopping the application.** The Enforcer Extension keeps the owner
  table up to date as applications launch and quit. When the kernel
  denies a write by the current application, the extension makes that
  application quit through `ExitToShell`, as if it had quit itself.
  When the writer cannot be stopped safely (interrupt-time code, a
  Multiprocessing Services task, an application in the background), the
  write is skipped and the writer goes on.
- **Temporary memory.** Mac OS gives all applications temporary memory
  from one shared heap. Enforcer gives each application its temporary
  memory from blocks that the application owns, so it is protected like
  the partition.
- **Preemption.** The kernel's timer notices an application that has run
  its own PowerPC code for 20 ms. At a point where that is safe (no
  Toolbox call in progress, not at interrupt time), the application is
  redirected into a call to `WaitNextEvent`, and returns to where it was
  when Mac OS gives it time again.
- **Memory on demand.** Enforcer changes the size the Process Manager
  reads at launch. For a 29 MB partition it also supplies the memory: a
  range of addresses above the Mac's memory where the patched kernel
  gives a page RAM the first time it is touched, from a pool that
  Enforcer fills from free memory and returns when it is not needed.

## Requirements

- A NewWorld Mac (the first iMac and every Mac after it) running Mac OS
  9.2.2. OldWorld Macs are not supported.
- The Mac OS ROM file from Mac OS 9.2.2 (Mac OS ROM 9.2.1, NanoKernel
  2.28). Enforcer Installer checks the file and refuses any other.
- Virtual memory can be on or off.

## Download

1. Download `Enforcer-VERSION.sit.hqx` from the
   [latest release](https://github.com/matjam/os9-enforcer/releases/latest)
   and copy it to the Mac.
2. Drop the file on StuffIt Expander, which a Mac OS 9 installation
   includes. It makes the folder
   "Enforcer VERSION", with Enforcer Installer, Enforcer Extension, the
   Enforcer control panel, Read Me and License.

The file is text (BinHex), so you can transfer it by any means: a web
browser, FTP, or a disk from another kind of computer.

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

Preemption is on with Enforce and Log only.

## Compatibility

Enforcer has been run with the software of a standard Mac OS 9.2.2
installation: its extensions and control panels, the Finder, Apple
System Profiler, Sherlock 2, Scrapbook and Stickies. No third-party
extension or application has been tested. What follows is what the
design implies; treat it as a guide to what to check.

Before you rely on Enforcer with your software, set the control panel to
Log only and use the Mac as usual. Log only records each write Enforcer
would block and stops nothing. If the log names an application you
trust, that application does something Enforcer does not allow.

### Extensions and control panels

- **Code that an extension loads into the system heap counts as the
  system's.** Enforcer never blocks or stops it, including when it runs
  inside an application. Most extensions work this way, so most are
  neither helped nor hindered. A bug in an extension can still damage
  any application.
- **Trap patches chain as usual.** Enforcer patches a few Memory Manager
  and Process Manager traps (`NewHandle`, `NewPtr`, `DisposePtr`,
  `DisposeHandle`, `SetHandleSize`, `SetPtrSize`, `ReallocateHandle`,
  `_OSDispatch`, `_MemoryDispatch`, `ExitToShell`) and passes each call
  on. An extension that patches the same traps runs before or after
  Enforcer's patch, as with any two extensions.
- **Software that replaces Mac OS's virtual memory, or patches the
  kernel or the Mac OS ROM file itself, is not expected to work.** This
  covers memory doublers, and enablers for processor upgrade cards that
  change the ROM file. Enforcer Installer refuses a Mac OS ROM file it
  does not recognize.
- **Extensions that put code into each application's heap** have that
  code counted as the application's. If it writes into another
  application, the application it runs in is stopped.

### Applications

- **PowerPC and 68k applications are both protected.** Only PowerPC
  code is preempted.
- **Applications that share memory by passing pointers** to each other
  (and then write through them) are stopped in Enforce mode, unless the
  pointer is into the other application's stack, which is where the
  results of calls between applications go. Apple events, the Clipboard,
  files and shared libraries are not affected: the system does the
  copying.
- **A write by another application's code into the current application**
  is allowed and logged, because callbacks work that way.
- **Applications that take all the memory they can find at launch** get
  what `MaxMem` reports, which is the partition's size. With a 29 MB
  partition that can be more RAM than the application took before.
- **Applications that test for low memory by asking for a very large
  block** and expect it to fail can now get the block.
- **Applications that depend on never being interrupted** during a long
  computation can be affected by preemption, which calls `WaitNextEvent`
  for them: a pointer kept into an unlocked handle can be left pointing
  at a block that moved.
- **Debuggers and programs that write-protect their own pages** (some
  garbage collectors) do not see faults on pages Enforcer opens for
  them.

### If something does not work

1. To see whether memory on demand is the cause, turn off "Give
   applications more memory than Get Info sets" and launch the
   application again.
2. To see whether protection or preemption is the cause, choose Log
   only (protection off, preemption on), then Off (both off).
3. To take Enforcer out, hold Shift at startup to turn extensions off,
   or remove it with Enforcer Installer.

## Limits

Crash containment:
- An overrun inside an application's own memory is not caught. It can
  only damage that application.
- Writes into the system heap are logged, not blocked.
- Writes into another application's stack are logged, not blocked. Mac
  OS itself has one application's code write there: when it calls a
  routine of one application on behalf of another (Sherlock 2's window
  does this when you click it from another application), the result goes
  on the caller's stack.
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
- About This Computer shows the RAM a 29 MB partition has taken, not
  29 MB. The application itself still sees the whole partition
  (`FreeMem`, `MaxMem`).
- Memory an application frees inside its partition goes back to the Mac
  only when the application quits.
- If an application touches new memory faster than Enforcer can supply
  it and the supply runs out, the application fails as on a Mac without
  enough memory.
- At most 16 applications get a 29 MB partition at once. The Finder and
  applications launched during startup keep ordinary partitions.
- The setting is one for every application: on or off.

Temporary memory:
- A large temporary handle can fail to grow with `SetHandleSize` where
  Mac OS would have grown it.

`CLAUDE.md` has the full list of known holes and how each part works.

## Contributing

Reports from real Macs are the most useful contribution: see
[CONTRIBUTING.md](CONTRIBUTING.md) for what to report and how to send a
fix. In a report, give the version: it is in the title bar of the
Enforcer control panel and of Enforcer Installer, and in the Finder's Get
Info window for each of Enforcer's files.

To build Enforcer from source and run it in QEMU, see
[DEVELOPMENT.md](DEVELOPMENT.md).

## License

Enforcer is under the MIT License; see `LICENSE`. Retro68, QEMU and the
reference sources in `refs/` have their own licenses.
