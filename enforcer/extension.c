/*
 * Enforcer Extension: the protection engine (common/engine.c) as a
 * background-only application. Mac OS launches it at startup from the
 * Extensions folder (file type 'appe'), so every application is protected
 * from the start, without a window.
 *
 * It publishes its state under the Gestalt selector 'Enfr' for the
 * Enforcer control panel. Values below 0x1000 are status codes, not a
 * state:
 *   kEnforcerVMOn         virtual memory is on, so it did not start
 *                         (earlier versions; this one runs with it);
 *   kEnforcerNoKernel     the NanoKernel is not patched;
 *   kEnforcerOtherKernel  the NanoKernel has another release's patch;
 *   kEnforcerFailed+n     engine_start or the kernel's owner check failed
 *                         with error -n.
 * It runs only with the kernel's owner check registered: on the engine's
 * hooks alone, a protected page can hang the machine (an lwarx/stwcx. loop)
 * and a store into one kills an MP task.
 *
 * The mode (off, log only, enforce) and whether to watch the system heap
 * are kept in "Enforcer Preferences" in the Preferences folder. The control
 * panel requests a change through the state's want_mode and want_sys; this
 * program applies it and saves it.
 *
 * When it stops an application, it posts a notification naming the
 * application, the one whose memory it tried to write into, and the
 * address, and saying that no restart is needed.
 */

#include <string.h>
#include <MacTypes.h>
#include <MacMemory.h>
#include <Events.h>
#include <Gestalt.h>
#include <AppleEvents.h>
#include <Files.h>
#include <Processes.h>

#include "engine.h"
#include "enforcer.h"

/* In InterfaceLib (Mac OS 8.5 and later), not in the Multiversal
   Interfaces. */
extern pascal OSErr NewGestaltValue(OSType selector, long value);
extern pascal OSErr ReplaceGestaltValue(OSType selector, long value);
extern pascal OSErr DeleteGestaltValue(OSType selector);

enum {
    kOnSystemDisk = -32768,
    kNMType = 8,          /* NMRec.qType */
    kPollTicks = 30,      /* re-read the process list every half second */
    kNotes = 4
};

static Boolean g_quit;

static void publish(long value)
{
    if (NewGestaltValue(kEngineGestalt, value) == gestaltDupSelectorErr)
        ReplaceGestaltValue(kEngineGestalt, value);
}

/* Preferences -------------------------------------------------------- */

/* Version 1 had only magic and mode (sys is then 0); version 2 added sys
   (mem is then 1). */
typedef struct {
    unsigned long magic, mode, sys, mem;
} Prefs;

static OSErr prefs_spec(FSSpec *spec)
{
    int16_t vref;
    int32_t dir;
    OSErr err = FindFolder(kOnSystemDisk, kPreferencesFolderType, true, &vref,
                           &dir);
    if (err)
        return err;
    err = FSMakeFSSpec(vref, dir, kEnforcerPrefsName, spec);
    return err == fnfErr ? noErr : err;
}

static Prefs load_prefs(void)
{
    static const Prefs defaults = {kEnforcerPrefsMagic, kModeEnforce, 0, 1};
    FSSpec spec;
    int16_t ref;
    Prefs p = defaults;
    LONGINT n = sizeof p;
    if (prefs_spec(&spec) || FSpOpenDF(&spec, fsRdPerm, &ref))
        return defaults;
    OSErr err = FSRead(ref, &n, &p);
    FSClose(ref);
    if (err == eofErr && n == 8) {
        p.sys = 0;
        p.mem = 1;
    } else if (err == eofErr && n == 12) {
        p.mem = 1;
    } else if (err || n != sizeof p) {
        return defaults;
    }
    if (p.magic != kEnforcerPrefsMagic || p.mode > kModeEnforce)
        return defaults;
    p.sys = p.sys != 0;
    p.mem = p.mem != 0;
    return p;
}

static void save_prefs(unsigned long mode, unsigned long sys,
                       unsigned long mem)
{
    FSSpec spec;
    int16_t ref;
    Prefs p = {kEnforcerPrefsMagic, mode, sys, mem};
    LONGINT n = sizeof p;
    if (prefs_spec(&spec))
        return;
    FSpCreate(&spec, kEnforcerCreator, 'pref', smSystemScript);
    if (FSpOpenDF(&spec, fsRdWrPerm, &ref))
        return;
    FSWrite(ref, &n, &p);
    SetEOF(ref, sizeof p);
    FSClose(ref);
}

/* Notifications ------------------------------------------------------ */

static NMRec g_nm[kNotes];
static Str255 g_nm_str[kNotes];
static int g_nm_next;

static void add(unsigned char *p, const char *s)
{
    while (*s && p[0] < 255)
        p[++p[0]] = (unsigned char)*s++;
}

static void add_hex(unsigned char *p, unsigned long v)
{
    static const char hex[] = "0123456789ABCDEF";
    char s[9];
    for (int i = 0; i < 8; i++)
        s[i] = hex[(v >> (28 - 4 * i)) & 15];
    s[8] = 0;
    add(p, s);
}

/* "Enforcer stopped “Scribble” because it tried to write into the memory
   of “Finder” at address 1EA34100. The write was blocked, so “Finder” was
   not changed and you do not need to restart." (Mac Roman curly quotes.)
   When Mac OS stopped the application instead (clean is 0), its own alert
   has already asked for a restart, so the last part is left out. For a
   store the kernel dropped (kOffenseBlocked), the writer's interrupt-time
   code or MP task goes on. */
static void notify(const Offense *o, int clean)
{
    NMRec *n = &g_nm[g_nm_next];
    unsigned char *str = g_nm_str[g_nm_next];
    g_nm_next = (g_nm_next + 1) % kNotes;
    NMRemove(n); /* in case it is still queued; an error otherwise */

    str[0] = 0;
    if (o->denied == kOffenseBlocked) {
        add(str, "Enforcer blocked a write by \xD2");
        add(str, o->writer);
        add(str, "\xD3 into the memory of \xD2");
        add(str, o->target);
        add(str, "\xD3 at address ");
        add_hex(str, o->addr);
        add(str, ". \xD2");
        add(str, o->writer);
        add(str, "\xD3 was not stopped: the write came from its "
                 "interrupt-time code or a separate task. If it misbehaves, "
                 "quit it.");
    } else {
        add(str, "Enforcer stopped \xD2");
        add(str, o->writer);
        add(str, "\xD3 because it tried to write into the memory of \xD2");
        add(str, o->target);
        add(str, "\xD3 at address ");
        add_hex(str, o->addr);
        add(str, ". The write was blocked, so \xD2");
        add(str, o->target);
        add(str, clean ? "\xD3 was not changed and you do not need to "
                         "restart."
                       : "\xD3 was not changed.");
    }

    memset(n, 0, sizeof *n);
    n->qType = kNMType;
    n->nmSound = (Handle)-1; /* the system alert sound */
    n->nmStr = str;
    n->nmResp = (ProcPtr)-1; /* remove it once shown */
    NMInstall(n);
}

/* Apple events ------------------------------------------------------- */

static pascal OSErr ae_quit(const AppleEvent *e, AppleEvent *reply,
                            int32_t refcon)
{
    g_quit = true;
    return noErr;
}

static pascal OSErr ae_ignore(const AppleEvent *e, AppleEvent *reply,
                              int32_t refcon)
{
    return noErr;
}

/* For tests (Scribble X): quit at once, without engine_stop, the way a
   crash ends this program. The engine's ExitToShell stub stops the engine
   (pool_quit in common/engine.c). */
static pascal OSErr ae_exit(const AppleEvent *e, AppleEvent *reply,
                            int32_t refcon)
{
    ExitToShell();
    return noErr;
}

static void install_ae(void)
{
    AEEventHandlerUPP quit = NewAEEventHandlerUPP(ae_quit);
    AEEventHandlerUPP ignore = NewAEEventHandlerUPP(ae_ignore);
    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, quit, 0,
                          false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenApplication, ignore, 0,
                          false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments, ignore, 0,
                          false);
    AEInstallEventHandler(kCoreEventClass, kAEPrintDocuments, ignore, 0,
                          false);
    AEInstallEventHandler(kEnforcerExtCreator, kEnforcerExitEvent,
                          NewAEEventHandlerUPP(ae_exit), 0, false);
}

int main(void)
{
    install_ae();

    Prefs prefs = load_prefs();
    State *s;
    long err = engine_start(&s, kModeOff, "Enforcer");
    if (err) {
        publish(kEnforcerFailed - err);
        return 0;
    }
    /* Without the kernel's owner check, protecting other applications'
       pages can hang the machine (an lwarx/stwcx. loop on a protected
       page) and kills MP tasks that store into them. Run only with it. */
    if (!s->kcb) {
        long why = s->kcb_err;
        engine_stop(s);
        publish(why == paramErr ? kEnforcerOtherKernel
                : why == kEngineNoMemory || why == kEngineNoArea
                    ? kEnforcerFailed - why
                    : kEnforcerNoKernel);
        return 0;
    }
    engine_set_mode(s, prefs.mode);
    if (prefs.sys)
        engine_set_sys(s, 1);
    s->want_sys = prefs.sys;
    engine_set_mem(s, prefs.mem);
    publish((long)s);

    unsigned long last_poll = TickCount(), shown = 0, stops_shown = 0;
    unsigned long saved_mode = prefs.mode, saved_sys = prefs.sys;
    unsigned long saved_mem = prefs.mem;
    /* The last blocked write told about: code that keeps writing (a VBL
       task) is told about once a minute. */
    char blocked_w[16] = "", blocked_t[16] = "";
    unsigned long blocked_time = 0;
    EventRecord ev;
    while (!g_quit) {
        if (WaitNextEvent(everyEvent, &ev, kPollTicks, NULL) &&
            ev.what == kHighLevelEvent)
            AEProcessAppleEvent(&ev);
        unsigned long now = TickCount();
        if (now - last_poll >= kPollTicks) {
            engine_poll(s);
            if (s->mode != saved_mode || s->sys_on != saved_sys ||
                s->mem_on != saved_mem) {
                saved_mode = s->mode;
                saved_sys = s->sys_on;
                saved_mem = s->mem_on;
                save_prefs(saved_mode, saved_sys, saved_mem);
            }
            last_poll = now;
        }
        /* Tell the user about each application stopped since last time.
           A clean stop happens at the fault, before this loop can run, so
           each one counted pairs with the next denied offense. */
        unsigned long n = s->n_offenses, stops = engine_clean_stops(s);
        if (n - shown > kOffenseLog)
            shown = n - kOffenseLog;
        for (; shown < n; shown++) {
            Offense *o = &s->offenses[shown % kOffenseLog];
            if (o->denied == kOffenseAllowed)
                continue;
            if (o->denied == kOffenseBlocked) {
                if (!strcmp(o->writer, blocked_w) &&
                    !strcmp(o->target, blocked_t) &&
                    o->time - blocked_time < 60)
                    continue;
                strcpy(blocked_w, o->writer);
                strcpy(blocked_t, o->target);
                blocked_time = o->time;
                notify(o, 0);
                continue;
            }
            int clean = stops_shown < stops;
            if (clean)
                stops_shown++;
            notify(o, clean);
        }
    }

    engine_stop(s);
    DeleteGestaltValue(kEngineGestalt);
    /* The state stays allocated: the control panel may still be reading
       it, and it says running = 0. */
    return 0;
}
