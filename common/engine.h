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
 * The program running the engine is not a domain. Run it only with virtual
 * memory off.
 */

#ifndef ENGINE_H
#define ENGINE_H

#include <MacTypes.h>
#include <Processes.h>
#include <Timer.h>
#include <MixedMode.h>

#include "hook.h"

enum {
    kMaxDomains = 32,
    kMaxWriters = 512,
    kFallbackRing = 16,
    kOffenseLog = 16,
    kEngineMagic = 'ENF3',
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
} Domain;

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

/* A store by an application's own code into another domain. */
typedef struct {
    unsigned long seq;  /* 1 for the first */
    unsigned long time; /* seconds since 1904 */
    unsigned long pc;   /* native PC, or the 68k instruction after the store */
    unsigned long addr, source;
    unsigned long denied; /* the application was stopped */
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
 * Not usable yet: an atomic update (lwarx/stwcx.) of a variable on a
 * protected page hangs the machine. The NanoKernel handles the stwcx.
 * fault by running the stwcx. again itself (NKTranslation.s FDP_06c0);
 * the reservation is gone by then, so it fails without a fault, the
 * caller's loop retries, and no hook ever sees a fault to open the page.
 * The system heap holds many such variables. Nothing turns this on but
 * SysLog and SysGuard (xlog/), which reproduce the hang. */
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
