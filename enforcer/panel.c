/*
 * Enforcer: the control panel for the Enforcer Extension. It is an
 * application of file type 'APPC', so it lives in the Control Panels folder
 * and opens from Apple menu > Control Panels.
 *
 * It finds the extension's state through Gestalt 'Enfr' and shows:
 *   - whether the extension is running, and why not if it is not;
 *   - "Protect each application's memory from the others": off sets the
 *     mode to off;
 *   - "Stop an application that writes into another's memory": on is
 *     enforce, off is log only;
 *   - "Also log writes into the system heap": the system heap watch (log
 *     only), available with the patched NanoKernel;
 *   - the last stores it caught, newest first;
 *   - counters, updated every second, and whether the NanoKernel's owner
 *     check is on.
 * A change sets the state's want_mode or want_sys; the extension applies
 * it within half a second and saves it in its preferences.
 */

#include <string.h>
#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <Gestalt.h>
#include <AppleEvents.h>

#include "engine.h"
#include "enforcer.h"

enum { kMenuApple = 128, kMenuFile, kMenuEdit };
enum { kFileClose = 1, kFileQuit = 3 };
enum { kRefreshTicks = 30, kShownOffenses = 6 };

/* Window layout, in window coordinates. */
static const Rect kWindowRect = {60, 100, 394, 540};
static const Rect kStatusRect = {6, 14, 28, 426};
static const Rect kProtectRect = {34, 14, 52, 426};
static const Rect kStopRect = {56, 30, 74, 426};
static const Rect kSysRect = {78, 30, 96, 426};
static const Rect kListRect = {124, 14, 286, 426};
static const Rect kFooterRect = {292, 14, 330, 426};

static WindowPtr g_win;
static ControlHandle g_protect, g_stop, g_sys;
static Boolean g_quit;
static Boolean g_stop_pref = true; /* the stop box, while protection is off */

/* Pascal strings ----------------------------------------------------- */

static void add(unsigned char *p, const char *s)
{
    while (*s && p[0] < 255)
        p[++p[0]] = (unsigned char)*s++;
}

static void add_p(unsigned char *p, const unsigned char *s)
{
    for (int i = 1; i <= s[0] && p[0] < 255; i++)
        p[++p[0]] = s[i];
}

static void add_num(unsigned char *p, long n)
{
    Str255 s;
    NumToString(n, s);
    add_p(p, s);
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

static StringPtr pstr(Str255 out, const char *s)
{
    out[0] = 0;
    add(out, s);
    return out;
}

/* The extension ------------------------------------------------------ */

enum { kNotRunning, kStatusCode, kStopped, kRunning };

typedef struct {
    int kind;
    long value; /* the Gestalt value */
    State *s;   /* when kRunning */
} Engine;

static void find_engine(Engine *e)
{
    LONGINT v;
    e->s = 0;
    e->value = 0;
    if (Gestalt(kEngineGestalt, &v) != noErr) {
        e->kind = kNotRunning;
        return;
    }
    e->value = v;
    if ((unsigned long)v < kEnforcerStatusMax) {
        e->kind = kStatusCode;
        return;
    }
    State *s = (State *)v;
    if (s->magic != kEngineMagic || !s->running) {
        e->kind = kStopped;
        return;
    }
    e->kind = kRunning;
    e->s = s;
}

static void status_text(const Engine *e, Str255 out)
{
    out[0] = 0;
    switch (e->kind) {
    case kNotRunning:
        add(out, "The Enforcer Extension is not running. Install it and "
                 "restart.");
        break;
    case kStatusCode:
        if (e->value == kEnforcerVMOn) {
            add(out, "Virtual memory is on. Turn it off and restart.");
        } else if (e->value == kEnforcerNoKernel) {
            add(out, "Enforcer is off: the Mac OS ROM file is not patched. "
                     "Open Enforcer Installer and click Install.");
        } else if (e->value == kEnforcerOtherKernel) {
            add(out, "Enforcer is off: the Mac OS ROM file has another "
                     "version's patch. Open Enforcer Installer and click "
                     "Install.");
        } else {
            add(out, "Enforcer could not start (error ");
            add_num(out, kEnforcerFailed - e->value);
            add(out, ").");
        }
        break;
    case kStopped:
        add(out, "The Enforcer Extension has stopped.");
        break;
    default:
        if (e->s->mode == kModeOff) {
            add(out, "Protection is off.");
        } else {
            add(out, e->s->mode == kModeEnforce ? "Protecting "
                                                : "Logging writes between ");
            add_num(out, engine_live_domains(e->s));
            add(out, " applications.");
        }
    }
}

static void sync_controls(const Engine *e)
{
    if (e->kind != kRunning || FrontWindow() != g_win) {
        HiliteControl(g_protect, 255);
        HiliteControl(g_stop, 255);
        HiliteControl(g_sys, 255);
        if (e->kind != kRunning) {
            SetControlValue(g_protect, 0);
            SetControlValue(g_stop, 0);
            SetControlValue(g_sys, 0);
        }
        return;
    }
    unsigned long m = e->s->want_mode;
    SetControlValue(g_protect, m != kModeOff);
    SetControlValue(g_stop, m == kModeOff ? g_stop_pref : m == kModeEnforce);
    SetControlValue(g_sys, e->s->want_sys != 0);
    HiliteControl(g_protect, 0);
    HiliteControl(g_stop, m == kModeOff ? 255 : 0);
    /* The system heap watch needs the patched NanoKernel. */
    HiliteControl(g_sys, m == kModeOff || !e->s->kcb ? 255 : 0);
}

/* Drawing ------------------------------------------------------------ */

static const char *source_name(unsigned long src)
{
    return (src & kSrcMask) == kSrc68k ? "68k" : "native";
}

static void draw_list(const Engine *e)
{
    Rect r = kListRect;
    Str255 s, t;
    EraseRect(&r);
    FrameRect(&r);
    TextFont(kFontIDGeneva);
    TextSize(9);
    int x = r.left + 6, y = r.top + 13;
    unsigned long n = e->s ? e->s->n_offenses : 0;
    if (!n) {
        MoveTo(x, y);
        DrawString(pstr(s, e->s ? "Nothing caught yet." : ""));
        return;
    }
    for (unsigned long k = 0; k < kShownOffenses && k < n && k < kOffenseLog;
         k++) {
        const Offense *o = &e->s->offenses[(n - 1 - k) % kOffenseLog];
        IUTimeString(o->time, true, t);
        s[0] = 0;
        add_p(s, t);
        add(s, "   ");
        add(s, o->denied == kOffenseStopped   ? "Stopped "
               : o->denied == kOffenseBlocked ? "Blocked "
                                              : "Logged ");
        add(s, o->writer);
        MoveTo(x, y);
        TextFace(bold);
        DrawString(s);
        TextFace(normal);
        s[0] = 0;
        add(s, "wrote into ");
        add(s, o->target);
        add(s, " at ");
        add_hex(s, o->addr);
        add(s, " (");
        add(s, source_name(o->source));
        add(s, " code at ");
        add_hex(s, o->pc);
        add(s, ")");
        MoveTo(x + 12, y + 12);
        DrawString(s);
        y += 26;
    }
}

static void draw_footer(const Engine *e)
{
    Rect r = kFooterRect;
    Str255 s;
    EraseRect(&r);
    if (!e->s)
        return;
    const Counters *c = &e->s->c;
    unsigned long system = 0;
    for (int i = 0; i < kSources; i++)
        system += c->foreign[i] - c->app_code[i];
    TextFont(kFontIDGeneva);
    TextSize(9);
    s[0] = 0;
    add(s, "Writes into other applications by system code (allowed): ");
    add_num(s, system);
    MoveTo(r.left, r.top + 10);
    DrawString(s);
    s[0] = 0;
    add(s, "Application switches: ");
    add_num(s, c->switches);
    add(s, "    Caught: ");
    add_num(s, e->s->n_offenses);
    MoveTo(r.left, r.top + 22);
    DrawString(s);
    s[0] = 0;
    const KernelCB *k = e->s->kcb;
    if (k) {
        add(s, "NanoKernel owner check on: ");
        add_num(s, k->seq + k->self);
        add(s, " writes checked, ");
        add_num(s, engine_clean_stops(e->s));
        add(s, " stopped, ");
        add_num(s, k->skipped + e->s->skips);
        add(s, " blocked");
    } else {
        add(s, "NanoKernel owner check off (error ");
        add_num(s, e->s->kcb_err);
        add(s, ")");
    }
    MoveTo(r.left, r.top + 34);
    DrawString(s);
}

static void draw(void)
{
    Engine e;
    Str255 s;
    find_engine(&e);
    SetPort(g_win);
    EraseRect(&g_win->portRect);
    TextFont(0);
    TextSize(12);
    status_text(&e, s);
    MoveTo(kStatusRect.left, kStatusRect.top + 16);
    DrawString(s);
    sync_controls(&e);
    DrawControls(g_win);
    TextFont(kFontIDGeneva);
    TextSize(9);
    TextFace(bold);
    MoveTo(kListRect.left, kListRect.top - 5);
    DrawString(pstr(s, "Caught"));
    TextFace(normal);
    draw_list(&e);
    draw_footer(&e);
}

/* Events ------------------------------------------------------------- */

static void click_content(Point where)
{
    ControlHandle c;
    SetPort(g_win);
    GlobalToLocal(&where);
    if (!FindControl(where, g_win, &c) || !TrackControl(c, where, NULL))
        return;
    Engine e;
    find_engine(&e);
    if (e.kind != kRunning)
        return;
    unsigned long m = e.s->want_mode;
    if (c == g_protect) {
        m = m == kModeOff ? (g_stop_pref ? kModeEnforce : kModeLog)
                          : kModeOff;
    } else if (c == g_stop && m != kModeOff) {
        m = m == kModeEnforce ? kModeLog : kModeEnforce;
        g_stop_pref = m == kModeEnforce;
    } else if (c == g_sys && m != kModeOff && e.s->kcb) {
        e.s->want_sys = !e.s->want_sys;
    }
    e.s->want_mode = m;
    sync_controls(&e);
}

static void do_menu(long choice)
{
    short menu = choice >> 16, item = choice & 0xFFFF;
    if (menu == kMenuApple) {
        Str255 name;
        GetMenuItemText(GetMenuHandle(kMenuApple), item, name);
        OpenDeskAcc(name);
    } else if (menu == kMenuFile) {
        if (item == kFileClose || item == kFileQuit)
            g_quit = true;
    }
    HiliteMenu(0);
}

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

static void setup_menus(void)
{
    Str255 s;
    MenuHandle m = NewMenu(kMenuApple, (ConstStringPtr) "\001\024");
    AppendResMenu(m, 'DRVR');
    InsertMenu(m, 0);
    m = NewMenu(kMenuFile, pstr(s, "File"));
    AppendMenu(m, pstr(s, "Close/W;(-;Quit/Q"));
    InsertMenu(m, 0);
    m = NewMenu(kMenuEdit, pstr(s, "Edit"));
    AppendMenu(m, pstr(s, "(Undo/Z;(-;(Cut/X;(Copy/C;(Paste/V;(Clear"));
    InsertMenu(m, 0);
    DrawMenuBar();
}

int main(void)
{
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);

    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication,
                          NewAEEventHandlerUPP(ae_quit), 0, false);
    AEEventHandlerUPP ignore = NewAEEventHandlerUPP(ae_ignore);
    AEInstallEventHandler(kCoreEventClass, kAEOpenApplication, ignore, 0,
                          false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments, ignore, 0,
                          false);
    AEInstallEventHandler(kCoreEventClass, kAEPrintDocuments, ignore, 0,
                          false);
    setup_menus();

    Str255 s;
    g_win = NewCWindow(NULL, &kWindowRect, pstr(s, "Enforcer"), true,
                       noGrowDocProc, (WindowPtr)-1, true, 0);
    SetPort(g_win);
    g_protect = NewControl(g_win, &kProtectRect,
                           pstr(s, "Protect each application\xD5s memory "
                                   "from the others"),
                           true, 0, 0, 1, checkBoxProc, 0);
    g_stop = NewControl(g_win, &kStopRect,
                        pstr(s, "Stop an application that writes into "
                                "another\xD5s memory"),
                        true, 0, 0, 1, checkBoxProc, 0);
    g_sys = NewControl(g_win, &kSysRect,
                       pstr(s, "Also log writes into the system heap"), true,
                       0, 0, 1, checkBoxProc, 0);

    unsigned long last = 0, signature = 0;
    EventRecord ev;
    while (!g_quit) {
        if (WaitNextEvent(everyEvent, &ev, 15, NULL)) {
            WindowPtr w;
            switch (ev.what) {
            case mouseDown:
                switch (FindWindow(ev.where, &w)) {
                case inMenuBar:
                    do_menu(MenuSelect(ev.where));
                    break;
                case inSysWindow:
                    SystemClick(&ev, w);
                    break;
                case inDrag:
                    DragWindow(w, ev.where, &qd.screenBits.bounds);
                    break;
                case inGoAway:
                    if (TrackGoAway(w, ev.where))
                        g_quit = true;
                    break;
                case inContent:
                    if (w != FrontWindow())
                        SelectWindow(w);
                    else
                        click_content(ev.where);
                    break;
                }
                break;
            case keyDown:
                if (ev.modifiers & cmdKey)
                    do_menu(MenuKey(ev.message & charCodeMask));
                break;
            case updateEvt:
                BeginUpdate((WindowPtr)ev.message);
                draw();
                EndUpdate((WindowPtr)ev.message);
                break;
            case activateEvt:
            case osEvt: {
                Engine e;
                find_engine(&e);
                SetPort(g_win);
                sync_controls(&e);
                break;
            }
            case kHighLevelEvent:
                AEProcessAppleEvent(&ev);
                break;
            }
        }
        if (TickCount() - last >= kRefreshTicks) {
            last = TickCount();
            Engine e;
            find_engine(&e);
            unsigned long sig = e.kind ^ e.value;
            if (e.s)
                sig ^= e.s->mode << 4 ^ e.s->want_mode << 6 ^
                       e.s->want_sys << 7 ^ e.s->n_offenses << 8 ^
                       engine_live_domains(e.s) << 20;
            SetPort(g_win);
            if (sig != signature) {
                signature = sig;
                InvalRect(&g_win->portRect);
            } else {
                draw_footer(&e);
            }
        }
    }
    return 0;
}
