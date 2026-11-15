# Contributing to Enforcer

Please contribute. The most useful thing you can do is run Enforcer and
say what happened, because so far it has run only in an emulator (see the
AI use disclosure in `README.md`).

## Tell me how it went

You do not need to find a bug to help. Open an issue and say:

- Which Mac (model, processor, RAM), and whether it is real hardware or
  an emulator.
- Whether virtual memory is on, and which third-party extensions and
  applications you use.
- What you saw. For example:
  - Is the Mac more stable, less stable, or the same?
  - When an application crashed, did Enforcer stop it and leave the rest
    running? What did the notification say?
  - Do applications get by with less trouble about memory? What does
    About This Computer show?
  - Does anything run slower, or stop working?

"It works on my Power Mac G4 with these twelve extensions" is a useful
report.

## Report a bug

1. Open an issue.
2. Say what you did, what you expected and what happened. Steps that make
   it happen again are worth more than anything else.
3. Add what you can of:
   - The Enforcer version, the Mac and the Mac OS version. The version
     is in the title bar of the Enforcer control panel, for example
     "Enforcer 0.1.0" (or "0.1.0+1a2b3c4" for a build made after that
     release).
   - Whether virtual memory is on, and the settings in the Enforcer
     control panel.
   - A screenshot of the notification, the error or the screen.
   - Whether it still happens in Log only, with "Give applications more
     memory than Get Info sets" turned off, and with Enforcer off. That
     tells which feature is involved (see "If something does not work" in
     `README.md`).
   - In QEMU: the output of `tools/xlog-read.py`, which prints Enforcer's
     state and its log of writes from the guest's memory.

If the Mac does not start up after installing, `README.md` says how to
put the original Mac OS ROM file back (see Remove).

## Fix a bug

Using an AI assistant is welcome here: this project was written with
one. This is how I would like a fix to arrive, with or without one:

1. Open an issue first, as above. If you use an AI assistant, have it
   write down what it found: where the fault is, how it knows, and
   anything else it thinks matters.
2. Send a pull request that fixes that issue, and name the issue in it.
3. Keep the change to that one problem. No reformatting, renaming or
   other improvements in the same pull request; open another issue for
   those.
4. Test it, and say in the pull request what you ran and what you saw:
   - In QEMU, with virtual memory on and with it off. Both matter: bugs
     here have shown up in only one of the two.
   - On a real Mac if you have one. Say which.
   - With the Scribble tests that cover the area (`CLAUDE.md`, section
     "Scribble"), and after a change to the kernel patch,
     `tools/rompatch-test.sh`.
5. Start the commit subject with `fix:` so the change is listed in the
   next release (`CLAUDE.md`, "Conventions").

If you or your assistant could not test something, say so. An honest
"not tested with virtual memory on" is better than a claim nobody
checked.

`DEVELOPMENT.md` says how to build Enforcer and run it in QEMU.
`CLAUDE.md` is the full development guide: how to set up and drive the
test machine, how each part works, the known holes, and what was learned
on the way. Point your assistant at both before it changes anything.

## Add or change a feature

Open an issue before you write the code. Say:

- What you want to add or change.
- Why: the problem it solves, and for whom.
- How it should behave, including with virtual memory on and off.
- What it could break: other applications, extensions, or the limits
  listed in `README.md`.

I am open to anything that improves memory protection, memory use or
performance. A feature outside those is less likely to go in, so ask
first.

## What stays out of the repository

Apple's files do not go into git: no Mac OS ROM file, no system
software, no disk images. The installer carries only the bytes that
differ from Apple's kernel.

## License

Enforcer is under the MIT License (`LICENSE`). By contributing, you agree
that your contribution is under the same license.
