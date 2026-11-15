# Enforcer

Crash containment for Mac OS 9.2.2 on NewWorld Macs. When an application
writes into another application's memory, Enforcer blocks the write and
stops the application that made it. The other applications and the system
keep running, and you do not need to restart. An application that computes
for a long time without giving up the processor is made to share it.

## Requirements

- A NewWorld Mac (the first iMac and every Mac after it) running Mac OS
  9.2.2.
- The Mac OS ROM file from Mac OS 9.2.2 (Mac OS ROM 9.2.1, NanoKernel
  2.28). Some later Macs came with a Mac OS ROM file of their own; Enforcer
  Installer checks the file and refuses any it does not know.
- Virtual memory can be on or off.

OldWorld Macs are not supported: they load the kernel from the System file
instead.

Enforcer has been tested only on QEMU's `mac99` machine, not on real
hardware.

## Install and remove

To install, open Enforcer Installer and click Install, then restart. The
installer:
1. patches the NanoKernel in your Mac OS ROM file;
2. moves the original file to the folder "Enforcer Backup" at the top of
   the startup disk;
3. copies the Enforcer Extension and the Enforcer control panel into the
   System Folder.

To update, open the new Enforcer Installer and click Install.

To remove Enforcer, open Enforcer Installer and click Remove, then
restart. It puts the original Mac OS ROM file back and moves Enforcer's
files to the Trash.

If the Mac does not start up after installing, start up from a Mac OS 9 CD
and drag "Mac OS ROM" from "Enforcer Backup" into the System Folder,
replacing the patched one.

## Use

To choose what Enforcer does, open the Enforcer control panel:
- **Enforce** (the default): block writes into other applications' memory
  and stop the application that made them.
- **Log only**: record the writes and allow them.
- **Off**: memory and temporary memory work as without Enforcer.

If the Mac OS ROM file is not patched (for example after reinstalling Mac
OS), the extension does not start and the control panel says so. To fix
it, open Enforcer Installer and click Install.

"Also log writes into the system heap" records applications' writes into
the system heap. Enforcer never blocks those: applications and extensions
write there legitimately.

"Give applications more memory than Get Info sets" (on by default) lets an
application launch with a larger partition and take more memory when the
partition is full. To have the Get Info sizes apply as without Enforcer,
turn it off; applications already running keep what they have.

When Enforcer stops an application, a notification names the application,
the one whose memory it tried to write into, and the address. When the
write came from code that cannot be stopped safely (interrupt-time code, a
deferred task or a separate task), Enforcer blocks the write and lets the
application run, and the notification says so.

To stop an application that does not respond, use Force Quit
(Command-Option-Escape), as without Enforcer.

## How it works

- **The kernel.** The patched NanoKernel keeps an owner for each 4 KB page
  of memory: the system, or one application. On a write to a protected
  page, it looks up the owner of the code doing the write. A write by an
  application's code into another application's page is an offense.
- **Address spaces.** Each application runs with its own segment IDs for
  the memory that holds applications. In them, every other application's
  pages are read-only, and its own pages stay writable. An application
  switch loads the next application's segment IDs, so it changes no page
  protection, whatever the applications' sizes.
- **The extension.** The Enforcer Extension starts at startup. It keeps the
  owner table up to date as applications launch and quit, and turns the
  per-application segment IDs on.
- **Temporary memory.** Each application's `TempNewHandle` memory comes
  from chunks owned by that application, so it is protected like its
  partition. Temporary memory that system software makes for itself (in
  the Process Manager's system mode) stays the Process Manager's and
  outlives the application.
- **Memory sizes.** The sizes in an application's Get Info window stop
  being a hard limit. At launch, an application gets up to four times its
  preferred size, but no more than a quarter of the free memory (an eighth
  with virtual memory off). When its
  partition is full, a new block comes from temporary memory that the
  application owns, as long as 2 MB of temporary memory stays free.
- **Stopping.** When the current application writes into another
  application, the write is denied and the application is stopped with
  `ExitToShell` in its own context. The target's memory is not changed.
- **Blocking.** When the writer cannot be stopped safely (an application
  that is not current, a Multiprocessing Services task, interrupt-time code,
  a deferred task), the kernel or the engine skips the write and the writer
  goes on.
- **Preemption.** An application whose own PowerPC code runs for 20 ms
  without calling `WaitNextEvent` is made to call it, when that is safe:
  at task level, with no Toolbox call in progress. Other applications get
  time, and it then goes on where it was. It is on with Enforce and Log
  only.
- **Without the extension,** the patched kernel opens no write-protected
  page that Apple's kernel would not.

## Limits

Not covered:
- An overrun inside an application's own memory. It can only damage that
  application.
- Writes by device DMA, which do not go through the MMU.
- The system heap: writes into it are logged only.

Known holes:
- A write by another application's code into the current application's
  memory is allowed and logged. It can be a callback storing results.
- A write by an application's 68k code in a deferred task, while the
  application is current, stops the application from inside the deferred
  task.
- A write the kernel allows opens its page for up to 16 ms to the
  application that was current and to Multiprocessing Services tasks.
- With virtual memory on, Enforcer finds Mac OS's application switch
  only after the first switch following startup. Until then, a switch
  takes effect at the first write that faults after it, normally Mac OS's
  own write to the new application's stack, and a write by the new
  application into the previous application's memory before then is not
  caught. The same applies at an application's launch and quit, with
  virtual memory on or off.
- With virtual memory on, when the current application touches a page of
  another application that is on disk, the Virtual Memory Manager can map
  it writable for the current application until the mapping is next
  rebuilt.
- The partly used first and last pages of a partition are not protected.
- With virtual memory on, code in an application's own shared libraries
  and plug-ins counts as the system's.
- An application launched in the last half second has no owner until its
  first event.
- A program that calls the patched kernel's Enforcer call on purpose can
  change Enforcer's owner table or turn the check off. A stray write
  cannot: the kernel drops writes into Enforcer's own tables.
- With virtual memory on, a protection can be lost briefly while the
  Virtual Memory Manager updates a page.
- Application memory above 1.5 GB (more logical memory than Mac OS 9
  normally has) is protected page by page as before: for up to 16 ms after
  a switch, a write by the new application into a page the previous one
  was using is not caught.

Preemption:
- Only PowerPC code is preempted. A 68k application, and PowerPC code
  called from 68k code, runs until it calls `WaitNextEvent` itself.
- An application that is inside a long Toolbox call, or in a routine the
  Toolbox called back (a dialog filter, for example), is not preempted.
- An application that has not called `WaitNextEvent` once since it
  launched is not preempted.
- Background applications get time from a preempted application, but
  bringing another application to the front (a click in its window,
  Command-Tab) waits until the preempted one calls `WaitNextEvent`
  itself.
- `WaitNextEvent` can move memory. An application that keeps a pointer
  into an unlocked handle across a long computation can find the block
  moved after it was preempted.
- There is no setting to turn preemption off, or to leave an application
  out, other than turning Enforcer off.

Memory sizes:
- Growing an existing block (`SetHandleSize`, `SetPtrSize`) past the
  partition still fails: a block cannot move out of its heap. Only new
  blocks come from outside it.
- `FreeMem`, `MaxMem` and About This Computer count only the partition,
  not the memory an application has outside it.
- With virtual memory off, a larger partition reserves real memory whether
  the application uses it or not.
- The sizes are one setting for every application: on or off.

Compatibility:
- An application's temporary memory comes from chunks of its own. A large
  temporary handle can fail to grow with `SetHandleSize` where Mac OS would
  have grown it.
- Programs that write-protect pages themselves (debuggers, garbage
  collectors) do not see faults on pages Enforcer opens for them.
- If the Enforcer Extension quits without stopping cleanly in a way that
  does not go through `ExitToShell`, its timer and handlers are left
  behind. A quit through `ExitToShell`, as after a crash, is handled.

## Building

Enforcer builds with Retro68's PowerPC toolchain, which
`toolchain/build.sh` builds from source at a pinned commit. The build
needs no Apple files.

To install the packages the toolchain needs on macOS, run:

```bash
brew install boost cmake ninja gmp mpfr libmpc bison texinfo
```

On Debian or Ubuntu, run:

```bash
sudo apt-get install cmake ninja-build g++ libgmp-dev libmpfr-dev libmpc-dev libboost-all-dev bison flex texinfo ruby python3
```

To build the toolchain, run this once. It fetches Retro68 into
`toolchain/Retro68`, builds in `toolchain/build` (2.3 GB) and installs in
`~/.local/opt/retro68`; on an 8-core Mac it takes about 7 minutes:

```bash
toolchain/build.sh
```

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
the HFS disk image `Enforcer.dsk`, for an emulator. Keep the three
programs in one folder: the installer copies the other two from its own
folder.

To use a Retro68 toolchain you already have, set `RETRO68` to its folder
(the one that holds `bin/`).

GitHub Actions builds every push and keeps `dist/` as the artifact
"Enforcer". `CLAUDE.md` describes the test machine, the tests and the
kernel work.

| Directory | What it is |
|---|---|
| `common/` | The protection engine, the 68k and native write hooks, NanoKernel calls |
| `enforcer/` | Enforcer Extension and the Enforcer control panel |
| `installer/` | Enforcer Installer: patches the Mac OS ROM file, installs and removes Enforcer |
| `kernel/` | The NanoKernel patches and the scripts that build and pack the ROM file |
| `xlog/` | XLog, Guard, SysLog and SysGuard: console front ends on the engine |
| `scribble/` | Scribble: writes into other memory, to test the engine |
| `tmem/` | TMem: temporary memory experiments and a pool prototype |
| `h68k/` | H68KTest: tests for both write hooks |
| `wlog/` | WLog: the first write logger |
| `probe/` | Probe: the first page protection experiment |
| `toolchain/` | The scripts that install the Retro68 toolchain in `~/.local/opt/retro68` and find it |
| `qemu/` | The patch and the script that install QEMU for the test machine in `~/.local/opt/qemu-os9` |
| `refs/` | Reference sources (submodules): the NanoKernel reconstructions and tbxi |
| `tools/` | Host scripts: drive the test machine, read the engine's state and the guest's memory, QEMU plugins, build helpers |

## License

Enforcer is under the MIT License; see `LICENSE`. Retro68, QEMU and the
reference sources in `refs/` have their own licenses.
