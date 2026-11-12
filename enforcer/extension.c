/*
 * Enforcer Extension: the protection engine (common/engine.c) as a
 * background-only application. Mac OS launches it at startup from the
 * Extensions folder (file type 'appe'), so every application is protected
 * from the start, without a window.
 *
 * It publishes its state under the Gestalt selector 'Enfr' for the
 * Enforcer control panel. Values below 0x1000 are status codes, not a
 * state:
 *   kEnforcerVMOn      virtual memory is on, so it did not start;
 *   kEnforcerFailed+n  engine_start failed with error -n.
 *
 * The mode (off, log only, enforce) is kept in "Enforcer Preferences" in
 * the Preferences folder. The control panel requests a mode change through
 * the state's want_mode; this program applies it and saves it.
 *
 * When it stops an application, it posts a notification naming the
 * application, the one whose memory it wrote into, and the address.
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

typedef struct {
    unsigned long magic, mode;
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

static unsigned long load_mode(void)
{
    FSSpec spec;
    int16_t ref;
    Prefs p;
    LONGINT n = sizeof p;
    if (prefs_spec(&spec) || FSpOpenDF(&spec, fsRdPerm, &ref))
        return kModeEnforce;
    OSErr err = FSRead(ref, &n, &p);
    FSClose(ref);
    if (err || n != sizeof p || p.magic != kEnforcerPrefsMagic ||
        p.mode > kModeEnforce)
        return kModeEnforce;
    return p.mode;
}

static void save_mode(unsigned long mode)
{
    FSSpec spec;
    int16_t ref;
    Prefs p = {kEnforcerPrefsMagic, mode};
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

/* "Enforcer stopped “Scribble”: its own code wrote into the memory of
   “Finder” at address 1EA34100. The write was blocked, so “Finder” was
   not changed." (Mac Roman curly quotes.) */
static void notify(const Offense *o)
{
    NMRec *n = &g_nm[g_nm_next];
    unsigned char *str = g_nm_str[g_nm_next];
    g_nm_next = (g_nm_next + 1) % kNotes;
    NMRemove(n); /* in case it is still queued; an error otherwise */

    str[0] = 0;
    add(str, "Enforcer stopped \xD2");
    add(str, o->writer);
    add(str, "\xD3: its own code wrote into the memory of \xD2");
    add(str, o->target);
    add(str, "\xD3 at address ");
    add_hex(str, o->addr);
    add(str, ". The write was blocked, so \xD2");
    add(str, o->target);
    add(str, "\xD3 was not changed.");

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
}

int main(void)
{
    install_ae();

    LONGINT vm = 0;
    if (Gestalt(gestaltVMAttr, &vm) == noErr && (vm & gestaltVMPresent)) {
        publish(kEnforcerVMOn);
        return 0;
    }

    unsigned long mode = load_mode();
    State *s;
    long err = engine_start(&s, mode, "Enforcer");
    if (err) {
        publish(kEnforcerFailed - err);
        return 0;
    }
    publish((long)s);

    unsigned long last_poll = TickCount(), shown = 0;
    EventRecord ev;
    while (!g_quit) {
        if (WaitNextEvent(everyEvent, &ev, kPollTicks, NULL) &&
            ev.what == kHighLevelEvent)
            AEProcessAppleEvent(&ev);
        unsigned long now = TickCount();
        if (now - last_poll >= kPollTicks) {
            unsigned long before = s->mode;
            engine_poll(s);
            if (s->mode != before)
                save_mode(s->mode);
            last_poll = now;
        }
        /* Tell the user about each application stopped since last time. */
        unsigned long n = s->n_offenses;
        if (n - shown > kOffenseLog)
            shown = n - kOffenseLog;
        for (; shown < n; shown++) {
            Offense *o = &s->offenses[shown % kOffenseLog];
            if (o->denied)
                notify(o);
        }
    }

    engine_stop(s);
    DeleteGestaltValue(kEngineGestalt);
    /* The state stays allocated: the control panel may still be reading
       it, and it says running = 0. */
    return 0;
}
