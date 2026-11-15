/*
 * engine.h: the protection engine shared by XLog, Guard and the Enforcer
 * Extension, and the state the Enforcer control panel reads.
 *
 * Each running application's partition (from the Process Manager) is a
 * domain. Its whole pages ([lo, hi), the partition rounded inward to pages)
 * are write-protected while the application is not current, and their bits
 * are set in a page bitmap over the blue area. Both write hooks manage
 * exactly the pages whose bit is set:
 *   - 68k code: common/h68k.s, in front of the emulator;
 *   - native code: common/hnat.s, in front of the Exception Manager.
 * A hook opens the faulting page, retries the store and records it in its
 * ring, unless the address is in the hooks' current range (the current
 * application's partition), where it only counts it.
 *
 * A store into another domain by the current application's own code (the
 * code lies in its partition) is an offense. In kModeEnforce the hooks deny
 * it: the page stays protected and the fault goes on. In kModeLog it is
 * allowed and logged. Stores by system code are always allowed. In
 * kModeOff nothing is protected. The hooks deny by address (HookData
 * deny_from): in kModeEnforce, everything above the system heap.
 *
 * The system heap can be one more domain (sys_on; see SysPages below): log
 * only, so offenses there are logged and never denied.
 *
 * Clean stop: a hook that denies a store writes ApplZone into the
 * Stopper's zone, and the engine stops that application with ExitToShell
 * instead of letting Mac OS show its "unexpectedly quit" alert:
 *   - a denied native store reaches the fallback handler (below) first,
 *     which resumes the application at ExitToShell;
 *   - a denied 68k store becomes a 68k bus error, and a 68k stub in front
 *     of the bus error vector (lowmem 0x08) calls _ExitToShell.
 * Both check that ApplZone still matches, and the stub also that the
 * fault was at task level (interrupt mask 0 in the frame's SR); otherwise
 * the fault goes on to Mac OS as before.
 *
 * A 16 ms Time Manager task:
 *   - notices a process switch (ApplZone changes), points both hooks'
 *     current range at the new application's partition, and protects all
 *     of the previous application's domain again;
 *   - takes the rings. A store is the target's own when ApplZone at the
 *     store is inside the target's partition, and cross-domain otherwise:
 *     logged by writer (PC and LR, or the 68k PC; application) and target.
 *     The page is protected again unless it is the current application's.
 * If a ring overflowed between ticks, every domain but the current one is
 * protected again.
 *
 * A write that reaches the Exception Manager without going through the
 * native hook (from another context) goes to a fallback handler in the
 * Exception Manager's system-wide slot. It opens the page and records the
 * store in a third ring.
 *
 * Safety rule: a protected page always has its bit set in the bitmap and a
 * live domain behind it; otherwise a fault on it kills the application or
 * hangs the machine. Domains are added as live, then bits, then protect;
 * removed as dying, unprotect, then bits, then dead.
 *
 * With the patched NanoKernel (kernel/, patch 4) the kernel decides each
 * store first, in every task (common/kcb.h). The engine keeps an owner
 * table next to the bitmap, one byte per page with the same pages set: 1
 * for the system heap, kOwnerApp + slot for an application's domain. The
 * control block and the owner table are write-protected, with owner
 * kOwnerTables: the kernel drops every store into them, and the engine
 * changes owner bytes, the mode and the current application through MP
 * call 109's operations (kcb_owners, kcb_set). The
 * kernel attributes a store to the owner of its code, allows it by making
 * the page writable (only its PTE: the page is protected again when the
 * engine protects it, or when its PTE leaves the page table) and records it
 * in the control block's ring, which the timer takes like the hooks' rings.
 * Own stores into the current application's pages are only counted. In
 * kModeEnforce it denies a store by the current application's code into
 * another application's page (in the blue task, not at interrupt time);
 * the hooks then deny that store as their own (HookData kdeny) and the
 * application is stopped cleanly. It drops any other offense into a page of
 * an application that is not current (another application's interrupt-time
 * code, an MP task): it skips the store, and the engine logs it as
 * kOffenseBlocked. Stores the kernel leaves alone reach the hooks as
 * before. Without patch 4 the registration fails and the engine runs on the
 * hooks alone.
 *
 * With patch 5 (application VSIDs, State.spaces) the engine protects no
 * domain page by page in the segments the kernel covers (by_vsid): each
 * application's pages are read-only in every other application's VSIDs,
 * and a hook in the Process Manager's context switch (pm_hook) loads the
 * next application's VSIDs. The timer no longer protects the previous
 * domain at a switch; after a store the kernel allowed, it drops the
 * page's PTE (kKcbOpFlush) instead of protecting the page.
 *
 * Temporary memory pools (needs the kernel's owner check): the engine
 * patches TempNewHandle (_OSDispatch selector 0x1D), DisposeHandle and
 * ExitToShell with 68k stubs that call its native code. An application's
 * temporary memory comes from chunks of its own: a chunk is temporary
 * memory of the application from the Process Manager (so it is freed when
 * the application quits), moved high and locked, with a Memory Manager
 * zone on the whole pages inside it, after a 16-byte signature ('EnfT' and
 * the zone's address). TempNewHandle allocates the handle in one of the
 * application's chunks, making a new one when none has room. The chunk's
 * pages are the domain's, like its partition's, and blocks only move
 * inside them, so their owner never goes stale. A chunk of the minimum
 * size, the application's first, is kept when empty; any other is freed
 * when its last handle is disposed in its owner's context. A request the
 * pools cannot take (no domain, no memory for a chunk) goes on to the
 * Process Manager as before, without an owner.
 *
 * Owners must never outlive the memory: a page whose owner is stale can
 * belong to another application, which would then be stopped for writing
 * its own memory. The ExitToShell stub drops the application's domain and
 * its chunks before the Process Manager frees them, and a sweep (each
 * tick, and before each TempNewHandle) drops a domain or a chunk whose
 * master pointer no longer points at it. A domain dropped this way is not
 * added again while its process is still listed.
 *
 * The program running the engine is not a domain.
 *
 * Virtual memory: what the timer, the hooks and the fallback handler use
 * is held in physical memory (the program's partition and the system heap
 * blocks), and the control block and owner table are locked where they
 * are (the kernel uses them by physical address). The sweep runs at
 * interrupt time only with virtual memory off.
 */

#ifndef ENGINE_H
#define ENGINE_H

#include <MacTypes.h>
#include <Processes.h>
#include <Timer.h>
#include <MixedMode.h>

#include "hook.h"
#include "kcb.h"

enum {
    kMaxDomains = 32,
    kMaxWriters = 512,
    kFallbackRing = 16,
    kOffenseLog = 16,
    kEngineMagic = 'ENF4',
    /* Gestalt selector whose value is the engine's State, while it runs. */
    kEngineGestalt = 'Enfr'
};

enum { kModeOff, kModeLog, kModeEnforce };

/* Where a store was caught. In Writer.source and Offense.source, combined
   with kWriterAppCode: the store's code is the writing application's own. */
enum { kSrcNative, kSrc68k, kSrcFallback, kSources };
enum { kSrcMask = 0xFF, kWriterAppCode = 0x100 };

/* engine_start errors. */
enum {
    kEngineNoSlot = -100,  /* Exception Manager system slot not found */
    kEngineNoMemory = -101,
    kEngineNoArea = -102,  /* NanoKernel area of the blue task not found */
    kEngineHook68k = -103, /* the 68k hook would not install */
    kEngineHookNative = -104,
    kEngineBusy = -105     /* another engine (XLog, Guard, Enforcer) runs */
};

typedef struct {
    volatile unsigned long live;  /* in use: its pages may be protected */
    volatile unsigned long dying; /* being removed: never protect again */
    unsigned long seen;           /* found by the last refresh */
    unsigned long lo, hi;         /* whole pages inside the partition */
    unsigned long location, size; /* the partition */
    ProcessSerialNumber psn;
    char name[16];
    unsigned long self_writes, foreign_writes_in;
    unsigned long mp;             /* the partition's master pointer, or 0 */
    volatile unsigned long quit;  /* dropped (ExitToShell, or its partition
                                     freed) while the process is listed */
    unsigned long code_lo, code_hi; /* its native code's pages outside the
                                       partition, owned in the table only */
    unsigned long code_state;     /* kCode* */
    unsigned long dropped;        /* when dropped: Counters.removed after it.
                                     A dropped slot keeps its name,
                                     partition and code range until it is
                                     used again, for naming a store taken
                                     after the drop (the application quits
                                     at a clean stop before the timer takes
                                     the store from the hook's ring) */
} Domain;

/* Domain.code_state. With virtual memory on, the Code Fragment Manager can
 * keep an application's code section outside its partition (file mapped),
 * where the kernel would count its stores as the system's. The engine
 * finds the code section of the application's main fragment from the
 * application's own context, at an event call (the GetNextEvent filter
 * stub, below), and gives its pages the application's owner byte
 * in the owner table only: they are not protected and have no bitmap bit.
 * Pages that already have an owner, or are in the partition, are left
 * alone. Code in other fragments (an application's own shared libraries)
 * still counts as the system's. */
enum {
    kCodeUnknown = 0, /* not looked for yet */
    kCodeInside,      /* in the partition (virtual memory off), or none */
    kCodeOwned,       /* code_lo-code_hi owned */
    kCodeFailed       /* not found: kCodeFailed + the step that failed */
};

/* A temporary memory chunk (see above). */
enum {
    kMaxChunks = 128,
    kChunkMin = 0x20000,  /* 128 KB */
    kChunkSlack = 0x1000, /* zone header, master pointers, signature */
    kChunkMasters = 32,
    kChunkSig = 'EnfT'
};

typedef struct {
    volatile unsigned long live;
    long domain;
    Handle handle;         /* the Process Manager's temporary memory */
    unsigned long block;   /* *handle: it is locked, so it never moves */
    unsigned long lo, hi;  /* its whole pages */
    unsigned long zone;    /* lo + 16 */
    unsigned long handles; /* handles made in it and not disposed */
    unsigned long keep;    /* the domain's first chunk: kept when empty */
} Chunk;

/* The 68k stubs, in a system heap block of their own that is never freed:
   an application's own patch of one of the traps can still lead to a
   stub, and a disabled stub passes every call on.
 *
 * The engine writes the trap table entries itself (OS traps at 0x400,
 * Toolbox traps at 0xE00) instead of calling NSetTrapAddress: the Process
 * Manager keeps an application's NSetTrapAddress patches as that
 * application's own and takes them out of the table whenever another
 * process is current, and the program running the engine is an
 * application. Each stub goes on to the routine that was in the table
 * when the engine installed it (*_old, never changed). The Process Manager
 * can put that routine back when it switches out an application with its
 * own patch of the trap; the timer then puts the stub back. It writes an
 * entry only when it holds exactly *_old, so the chain cannot loop. */
enum {
    kPoolMagic = 'Pool',
    kPoolOsd = 28,
    kPoolDh = 43,
    kPoolEts = 12,
    kPoolGne = 27
};

/* The GetNextEvent filter (jGNEFilter, low memory 0x29A, which every
 * application shares; WaitNextEvent and GetNextEvent call it in the
 * calling application's context) adds a newly launched application as a
 * domain at its first event call, before it lets any other process run
 * (the half-second refresh can come much later), and finds an
 * application's native code. It calls the engine only when ApplZone
 * differs from last, so once per process switch, and then goes on to the
 * filter that was installed before it (gne_old, or none). The trap table
 * cannot be used for this: each application has its own patches of
 * _WaitNextEvent and _GetNextEvent, which the Process Manager swaps on
 * every switch. */
typedef struct {
    unsigned long magic;
    volatile unsigned short enabled;
    volatile unsigned short inside; /* the engine's own TempNewHandle */
    unsigned long osd_old, dh_old, ets_old;
    RoutineDescriptor rd_new, rd_disposed, rd_quit;
    unsigned short osd[kPoolOsd], dh[kPoolDh], ets[kPoolEts];
    volatile unsigned long last; /* ApplZone at the last filter call */
    unsigned long gne_old;
    RoutineDescriptor rd_launch;
    unsigned short gne[kPoolGne];
    /* The ExitToShell stub calls the engine while this is set: from
       engine_start to engine_stop, pools on or off, so the engine stops
       when the program running it quits without engine_stop (a crash). */
    volatile unsigned short quit_hook;
} PoolStubs;

typedef struct {
    unsigned long news;   /* TempNewHandle calls taken into a chunk */
    unsigned long passed; /* passed on: no domain, or no room */
    unsigned long disposed;
    unsigned long chunks_made, chunks_freed, chunks_dropped;
    unsigned long quits;  /* domains dropped at ExitToShell */
    unsigned long gone;   /* domains and chunks the sweep found freed */
    unsigned long reinstalled; /* stubs the timer put back in the table */
    unsigned long launched; /* domains added at an event call */
} PoolCounters;

/* A cross-domain writer: a store site, the application current at the
   store, and the domain written. For 68k stores, pc is the 68k instruction
   after the store and lr is 0. */
typedef struct {
    unsigned long pc, lr, writer_zone, target_lo;
    unsigned long first_addr, last_addr, count;
    unsigned long source;
    char writer[16];
    char target[16];
} Writer;

/* Offense.denied */
enum {
    kOffenseAllowed = 0, /* logged only */
    kOffenseStopped = 1, /* denied, and the application was stopped */
    kOffenseBlocked = 2  /* the kernel dropped the store; the writer, not
                            current (or an MP task), goes on */
};

/* A store by an application's own code into another domain. */
typedef struct {
    unsigned long seq;  /* 1 for the first */
    unsigned long time; /* seconds since 1904 */
    unsigned long pc;   /* native PC, or the 68k instruction after the store */
    unsigned long addr, source;
    unsigned long denied; /* kOffense* */
    char writer[16];
    char target[16];
} Offense;

/* The clean stop's shared words and its 68k bus error stub, in a system
   heap block of its own: it stays allocated if another program patches
   the vector after the engine, so the stub keeps passing faults on. */
enum { kStopperMagic = 'Stop', kStopperCode = 28 };

typedef struct {
    unsigned long magic;
    unsigned long old_vector;        /* the bus error vector it replaced */
    volatile unsigned long zone;     /* the application to stop, or 0 */
    volatile unsigned long stops[kSources]; /* clean stops, by source */
    unsigned short code[kStopperCode];
} Stopper;

/* The system heap, watched page by page while sys_on is set. Its pages run
 * from the first whole page after SysZone's header to the zone's end
 * (bkLim), followed as the heap grows and shrinks. A page is never
 * protected when it holds one of the engine's own blocks (the hooks write
 * some of them while handling a fault), is not plain read/write, or has
 * held the stack pointer of a trapped store (so an exception frame pushed
 * on that stack can never fault).
 *
 * Most stores into the system heap are by system code, and each one that
 * finds its page protected costs a trap. A page that traps on kSysStreak
 * timer ticks in a row is left open ("hot") for kSysHotTicks << backoff
 * ticks, then protected again. Its backoff goes up by one when it turns hot
 * again within kSysHotTicks ticks of being protected, and back to 0 when it
 * turns hot after kSysCoolTicks ticks or more.
 *
 * Needs the patched NanoKernel (kernel/): with the stock kernel, an atomic
 * update (lwarx/stwcx.) on a protected page hangs the machine (patch 1),
 * the CPU plugin stops interrupt handling when its stack is protected
 * (patch 2), and Multiprocessing Services tasks stop on their first store
 * (patch 3). Nothing turns this on but SysLog and SysGuard (xlog/). */
enum {
    kSysMagic = 'SysP',
    kSysMaxPages = 16384, /* 64 MB */
    kSysSpare = 4096,     /* pages for the heap to grow into */
    kSysMaxHot = 256,
    kSysStreak = 4,
    kSysHotTicks = 64,    /* about a second */
    kSysMaxBackoff = 6,
    kSysCoolTicks = 4096
};

/* tools/xlog-read.py names these in this order. */
typedef struct {
    unsigned long traps;     /* stores trapped, all code */
    unsigned long app_traps; /* of those, by the current application's code */
    unsigned long hot;       /* pages left open */
    unsigned long hot_full;  /* not left open: the hot list was full */
    unsigned long cooled;    /* hot pages protected again */
    unsigned long reheated;  /* hot again soon after: backoff went up */
    unsigned long excluded;  /* pages of the engine's own blocks */
    unsigned long not_rw;    /* pages that are not plain read/write */
    unsigned long stacks;    /* pages left open for a stack */
    unsigned long resweeps;  /* whole heap protected again (ring overflow) */
    unsigned long grown, shrunk;
} SysCounters;

typedef struct {
    unsigned long page, until; /* until: a tick */
} HotPage;

typedef struct {
    unsigned long magic;
    unsigned long cap;        /* pages the per-page arrays hold */
    unsigned long lo;         /* the first page */
    volatile unsigned long n; /* pages watched: [lo, lo + n * kPage) */
    unsigned long tick;       /* timer ticks while watching */
    unsigned long n_hot;
    SysCounters c;
    HotPage hot[kSysMaxHot];
    /* cap entries each, in the same block after this header. */
    unsigned short *last;      /* the tick of the page's last trap */
    unsigned short *cooled_at; /* the tick it was last protected again */
    unsigned char *streak, *backoff, *flags;
} SysPages;

/* tools/xlog-read.py names these in this order. */
typedef struct {
    unsigned long self[kSources], foreign[kSources], lost[kSources];
    unsigned long outside; /* ring entries outside every live domain */
    unsigned long switches, timer_runs, kernel_errors;
    unsigned long refreshes, added, removed;
    unsigned long dropped, nwriters;
    unsigned long info_errors; /* GetProcessInformation failures */
    unsigned long app_code[kSources]; /* offenses, by where caught */
    /* What protecting the previous application's pages at a switch costs:
       the pages, and the time in timebase ticks (tb_per_ms of them a
       millisecond). */
    unsigned long switch_pages, switch_tb, switch_tb_max, tb_per_ms;
    /* Switches the hook in the Process Manager's switch passed to the
       kernel (State.pm_stub), as of the last tick. */
    unsigned long pm_switches;
} Counters;

/* In the system heap, so it outlives a program that stops without
   cleaning up; readers check magic and running. */
typedef struct {
    /* For tools/xlog-read.py: where everything is. */
    unsigned long magic, self;
    unsigned long off_counters, n_counters;
    unsigned long off_domains, n_domains, domain_size;
    unsigned long off_writers, max_writers, writer_size;
    unsigned long off_cur;
    unsigned long off_offenses, max_offenses, offense_size;
    unsigned long off_mode;
    unsigned long off_stopper;
    unsigned long off_sys;
    unsigned long off_kernel;

    /* Control. A control panel sets want_mode and want_sys; engine_poll
       applies them. */
    volatile unsigned long mode, want_mode;
    volatile unsigned long running; /* cleared when the engine stops */
    char name[16];                  /* the program running the engine */
    long sys_dom;                   /* the system heap's domain, or -1 */
    volatile unsigned long sys_on, want_sys;
    unsigned long deny_from;        /* as the hooks' */
    SysPages *sys;                  /* kept while the engine runs, or 0 */

    Counters c;
    long cur; /* domain of the current application, or -1 */
    unsigned long cur_zone;
    volatile unsigned long active;
    unsigned long area;
    unsigned long my_lo, my_hi; /* the engine's own partition */
    unsigned long seen[kSources];
    volatile HookData *h68k, *hnat;
    unsigned long had68k, hadnat; /* hooks were installed before us */
    /* The hooks' own and passed-on counts, kept because removing a hook
       frees its data. */
    unsigned long hook_self[2], hook_passed[2];
    unsigned char *bitmap;
    Domain domains[kMaxDomains];
    Writer writers[kMaxWriters];
    /* The last kOffenseLog offenses: number n is at (n - 1) % kOffenseLog. */
    volatile unsigned long n_offenses;
    Offense offenses[kOffenseLog];
    /* Written by the fallback handler; fb_count is the last seq. */
    volatile unsigned long fb_count, fb_idx;
    HookEvent fb_ring[kFallbackRing];
    TMTask timer;
    RoutineDescriptor timer_rd;
    Stopper *stopper; /* 0 if the clean stop is not installed */

    /* The kernel's owner check (common/kcb.h), tools/xlog-read.py names
       these in this order. kcb is 0 when it is not registered, and kcb_err
       says why: the kernel's error (-4 without patch 4), or an engine
       error (kEngineNoMemory; kEngineNoArea: no single physical range). */
    const KernelCB *kcb;        /* read-only: kcb_set and kcb_owners change it */
    long kcb_err;
    /* The owner table: kcb->entries bytes, read-only (kcb_owners changes
       them). */
    const unsigned char *owners;
    char *kcb_raw, *owners_raw; /* their allocations */
    unsigned long kseen;        /* the last kernel event taken */
    unsigned long kevents;      /* events taken */
    unsigned long klost;        /* events overwritten before the timer ran */
    unsigned long kdenied;      /* denied stores (the hooks record them) */

    /* Temporary memory pools: pool is 0 when they are not installed (no
       owner check, or no memory). */
    PoolStubs *pool;
    PoolCounters pc;
    unsigned long vm;           /* virtual memory is on */
    unsigned long pm_zone;      /* the Process Manager's zone, or 0 */
    unsigned long owners_size;  /* the owner table's bytes (whole pages) */
    Chunk chunks[kMaxChunks];
    /* Denied native stores the fault handler skipped instead of stopping
       the application: they came from above task level (skip_store). */
    unsigned long skips;
    /* Application VSIDs (kernel patch 5, kcb.h kKcbOpSpaces). spaces is 1
       when the kernel has them; vsid_mask has the segments they cover
       (bit n: segment n), those of the partitions and chunks marked so
       far, 0-5 only; spaces_on is 1 while they are on, which is while
       protection is on. With them on, each application's pages in those
       segments are read-only in every other application's VSIDs, so a
       switch protects nothing there. */
    unsigned long spaces, vsid_mask, spaces_on;
    /* The hook in the Process Manager's switch: the address of the
       instruction it replaces, or 0, and the stub it calls. pm_mapped is
       1 when the site is in file-mapped code (virtual memory on), patched
       through the kernel (kcb_patch) on a held page; pm_tried and
       pm_tries are the last lazy-switch PC searched from, and how many. */
    unsigned long pm_site;
    unsigned long *pm_stub;
    unsigned long pm_mapped, pm_tried, pm_tries;
    /* Preemption (kernel patch 6, kcb.h kKcbOpPreempt): the yield stub
       (common/hyield.h), or 0; preempt_on is 1 while the kernel has it,
       which is while protection is on, with slices of slice_ms. */
    struct YieldData *yield;
    unsigned long preempt_on, slice_ms;
} State;

/* Install the hooks, the fallback handler and the timer, read the process
   list, and start in mode. name is the running program's, for the log.
   Returns 0 and the state, or a kEngine error. */
long engine_start(State **out, unsigned long mode, const char *name);

/* Call from the program's event loop (never at interrupt time), every half
   second or so: re-reads the process list and applies want_mode. */
void engine_poll(State *s);

/* Change mode now (event loop only). */
void engine_set_mode(State *s, unsigned long mode);

/* Start or stop watching the system heap now (event loop only). Starting
   can fail (no free domain or no memory); sys_on says what happened. */
void engine_set_sys(State *s, int on);

/* Stop protecting, remove the timer and the fallback handler, and remove
   the hooks if this engine installed them. The state stays allocated. */
void engine_stop(State *s);

/* Temporary memory in chunks, and the chunks, for the control panel. */
static inline int engine_live_chunks(const State *s)
{
    int n = 0;
    for (int i = 0; i < kMaxChunks; i++)
        if (s->chunks[i].live)
            n++;
    return n;
}

/* For reports. */
int engine_domain_at(State *s, unsigned long addr);
void engine_writer_name(State *s, unsigned long zone, char out[16]);

/* Applications stopped cleanly so far. Inline, as below. */
static inline unsigned long engine_clean_stops(const State *s)
{
    const Stopper *st = s->stopper;
    if (!st)
        return 0;
    unsigned long n = 0;
    for (int i = 0; i < kSources; i++)
        n += st->stops[i];
    return n;
}

/* The applications being tracked (not the system heap, which has no
   partition). Inline, so that readers of the state (the control panel)
   need not link the engine. */
static inline int engine_live_domains(const State *s)
{
    int n = 0;
    for (int i = 0; i < kMaxDomains; i++)
        if (s->domains[i].live && !s->domains[i].dying &&
            s->domains[i].size)
            n++;
    return n;
}

#endif
