# Enforcer

Crash containment for Mac OS 9.2.2 on NewWorld Macs. When an application
writes into another application's memory, Enforcer blocks the write and
stops the application that made it. The other applications and the system
keep running, and you do not need to restart.

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

When Enforcer stops an application, a notification names the application,
the one whose memory it tried to write into, and the address. When the
write came from code that cannot be stopped safely (interrupt-time code, a
deferred task or a separate task), Enforcer blocks the write and lets the
application run, and the notification says so.

## How it works

- **The kernel.** The patched NanoKernel keeps an owner for each 4 KB page
  of memory: the system, or one application. On a write to a protected
  page, it looks up the owner of the code doing the write. A write by an
  application's code into another application's page is an offense.
- **The extension.** The Enforcer Extension starts at startup. It
  write-protects every application's memory except the current
  application's, and keeps the owner table up to date as applications
  launch and quit.
- **Temporary memory.** Each application's `TempNewHandle` memory comes
  from chunks owned by that application, so it is protected like its
  partition.
- **Stopping.** When the current application writes into another
  application, the write is denied and the application is stopped with
  `ExitToShell` in its own context. The target's memory is not changed.
- **Blocking.** When the writer cannot be stopped safely (an application
  that is not current, a Multiprocessing Services task, interrupt-time code,
  a deferred task), the kernel or the engine skips the write and the writer
  goes on.
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
- A write the kernel allows opens its page to every task for up to 16 ms.
- The partly used first and last pages of a partition are not protected.
- With virtual memory on, code in an application's own shared libraries
  and plug-ins counts as the system's.
- An application launched in the last half second has no owner until its
  first event.
- Enforcer's own control block and owner table are in the system heap,
  where any code can write them.
- With virtual memory on, a protection can be lost briefly while the
  Virtual Memory Manager updates a page.

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

The programs build with the Retro68 PowerPC toolchain from the
`os9-protect` repository, next to this one. To build all of them, run:

```bash
./build.sh
```

Each program builds a MacBinary `.bin` in its `build/` folder.
`CLAUDE.md` describes the test machine, the tests and the kernel work.

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
| `tools/` | Host scripts: read the engine's state and the guest's memory, build helpers |
