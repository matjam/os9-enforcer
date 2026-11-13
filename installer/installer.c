/*
 * Enforcer Installer.
 *
 * Install:
 *   1. Reads the Mac OS ROM file in the System Folder and checks that its
 *      NanoKernel is the stock 2.28 (rompatch.c).
 *   2. Writes a patched copy into the System Folder as "Mac OS ROM
 *      (Enforcer)": the data fork from rom_patch, then the resource fork
 *      with its 'cfrg' offsets moved, then the original's file type
 *      ('tbxi') and Finder flags.
 *   3. Moves the original into the folder "Enforcer Backup" at the top of
 *      the startup disk, and gives the copy the original's name. Open
 *      Firmware starts up from the file of type 'tbxi' in the System
 *      Folder, so at every step there is one.
 *   4. Copies the Enforcer Extension and the Enforcer control panel from
 *      the installer's folder into the Extensions and Control Panels
 *      folders.
 * If the kernel is already patched, Install only does step 4.
 *
 * Remove puts the original Mac OS ROM file back, and moves the patched one,
 * the extension and the control panel to the Trash.
 *
 * Mac OS keeps the Mac OS ROM file open, and the Finder will not move an
 * open file. The File Manager renames and moves it; Mac OS goes on using
 * the open file until the restart.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <Files.h>
#include <Resources.h>
#include <Gestalt.h>
#include <Processes.h>
#include <Icons.h>
#include <AppleEvents.h>
#include <MacMemory.h>

#include "rompatch.h"

enum {
    kDialog = 128,
    kInstall = 1, /* becomes Restart after a change */
    kQuit = 2,
    kRemove = 3,
    kAbout = 4,
    kStatus = 5,
    kIcon = 6,
    kIconFamily = 128,

    kOnSystemDisk = -32768,
    kRootDir = 2,
    kAttrDir = 0x10,           /* ioFlAttrib: a folder */
    kFlagInited = 0x0100,      /* Finder flags */
    kWatchCursor = 4,
    kVMPresent = 1             /* gestaltVMAttr */
};

static const char kTempName[] = "Mac OS ROM (Enforcer)";
static const char kRestoreName[] = "Mac OS ROM (restoring)";
static const char kBackupName[] = "Enforcer Backup";

static const struct {
    const char *name;
    OSType type, creator, folder;
} kPrograms[] = {
    {"Enforcer Extension", 'appe', 'EnfX', 'extn'},
    {"Enforcer", 'APPC', 'Enfr', 'ctrl'},
};
enum { kNPrograms = sizeof kPrograms / sizeof kPrograms[0] };

typedef struct {
    short vref;            /* the startup disk */
    long sys_dir;          /* the System Folder */
    int have_rom;
    FSSpec rom;            /* the Mac OS ROM file */
    FInfo rom_info;
    int kernel;            /* kKernel..., from rom_open */
    int rom_err;
    int have_backup;
    FSSpec backup;         /* Enforcer Backup:Mac OS ROM */
    short app_vref;        /* the installer's folder */
    long app_dir;
    int programs_here;     /* both programs are next to the installer */
    int programs_installed;
    int vm_on;
} Setup;

static DialogPtr g_dialog;

/* Strings ------------------------------------------------------------- */

static StringPtr pstr(Str255 out, const char *s)
{
    size_t n = strlen(s);
    if (n > 255)
        n = 255;
    out[0] = (unsigned char)n;
    memcpy(out + 1, s, n);
    return out;
}

static void set_text(short item, const char *fmt, ...)
{
    char buf[512];
    Str255 s;
    short type;
    Handle h;
    Rect r;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    GetDialogItem(g_dialog, item, &type, &h, &r);
    SetDialogItemText(h, pstr(s, buf));
}

static ControlHandle button(short item)
{
    short type;
    Handle h;
    Rect r;
    GetDialogItem(g_dialog, item, &type, &h, &r);
    return (ControlHandle)h;
}

static void enable(short item, int on)
{
    HiliteControl(button(item), on ? 0 : 255);
}

static void retitle(short item, const char *title)
{
    Str255 s;
    SetControlTitle(button(item), pstr(s, title));
}

static pascal void draw_icon(DialogPtr d, INTEGER item)
{
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, item, &type, &h, &r);
    PlotIconID(&r, 0, 0, kIconFamily);
}

/* Lets other programs run during the long compression, and shows how far
   it is. */
static void progress(size_t done, size_t n)
{
    EventRecord ev;
    set_text(kStatus, "Patching the NanoKernel: %d%% done.",
             (int)(done / (n / 100 + 1)));
    WaitNextEvent(0, &ev, 0, NULL);
}

/* Files ---------------------------------------------------------------- */

static OSErr make_spec(short vref, long dir, const char *name, FSSpec *spec)
{
    Str255 s;
    return FSMakeFSSpec(vref, dir, name ? pstr(s, name) : NULL, spec);
}

static OSErr read_fork(FSSpec *spec, int rsrc, unsigned char **buf,
                       LONGINT *len)
{
    short ref;
    OSErr err = rsrc ? FSpOpenRF(spec, fsRdPerm, &ref)
                     : FSpOpenDF(spec, fsRdPerm, &ref);
    *buf = NULL;
    if (err)
        return err;
    err = GetEOF(ref, len);
    if (!err) {
        *buf = malloc(*len ? *len : 1);
        if (!*buf)
            err = memFullErr;
    }
    if (!err && *len)
        err = FSRead(ref, len, *buf);
    FSClose(ref);
    if (err) {
        free(*buf);
        *buf = NULL;
    }
    return err;
}

static OSErr write_fork(FSSpec *spec, int rsrc, const void *buf,
                        LONGINT len)
{
    short ref;
    LONGINT n = len;
    OSErr err = rsrc ? FSpOpenRF(spec, fsRdWrPerm, &ref)
                     : FSpOpenDF(spec, fsRdWrPerm, &ref);
    if (err)
        return err;
    err = SetEOF(ref, len);
    if (!err && len)
        err = FSWrite(ref, &n, buf);
    FSClose(ref);
    return err;
}

/* The ID of the folder spec names, or 0. */
static long folder_id(const FSSpec *spec)
{
    CInfoPBRec pb;
    Str255 name;
    memcpy(name, spec->name, spec->name[0] + 1);
    memset(&pb, 0, sizeof pb);
    pb.dirInfo.ioNamePtr = name;
    pb.dirInfo.ioVRefNum = spec->vRefNum;
    pb.dirInfo.ioDrDirID = spec->parID;
    if (PBGetCatInfoSync(&pb) || !(pb.dirInfo.ioFlAttrib & kAttrDir))
        return 0;
    return pb.dirInfo.ioDrDirID;
}

/* The Mac OS ROM file: the file of type 'tbxi' in the System Folder,
   preferring one named "Mac OS ROM". */
static int find_rom(Setup *s)
{
    CInfoPBRec pb;
    Str255 name;
    FInfo fi;
    short i;
    if (make_spec(s->vref, s->sys_dir, "Mac OS ROM", &s->rom) == noErr &&
        FSpGetFInfo(&s->rom, &fi) == noErr && fi.fdType == 'tbxi') {
        s->rom_info = fi;
        return 1;
    }
    for (i = 1;; i++) {
        memset(&pb, 0, sizeof pb);
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = s->vref;
        pb.hFileInfo.ioDirID = s->sys_dir;
        pb.hFileInfo.ioFDirIndex = i;
        if (PBGetCatInfoSync(&pb))
            return 0;
        if (!(pb.hFileInfo.ioFlAttrib & kAttrDir) &&
            pb.hFileInfo.ioFlFndrInfo.fdType == 'tbxi' &&
            FSMakeFSSpec(s->vref, s->sys_dir, name, &s->rom) == noErr) {
            s->rom_info = pb.hFileInfo.ioFlFndrInfo;
            return 1;
        }
    }
}

/* Reads a Mac OS ROM file and says which kernel it has. */
static int kernel_of(FSSpec *spec, int *rom_err)
{
    unsigned char *data;
    LONGINT len;
    Rom rom;
    int kernel = kKernelUnknown;
    OSErr err = read_fork(spec, 0, &data, &len);
    if (err) {
        *rom_err = kRomNotBootInfo;
        return kKernelUnknown;
    }
    *rom_err = rom_open(&rom, data, (size_t)len);
    if (*rom_err == kRomOK)
        kernel = rom.kernel;
    rom_close(&rom);
    free(data);
    return kernel;
}

/* Moves a file to the Trash of its disk, renamed if the name is taken
   there. */
static OSErr move_to_trash(const FSSpec *spec)
{
    short tv;
    long td;
    FSSpec trash, probe, cur = *spec;
    Str255 name;
    char base[32], try[48];
    int n;
    OSErr err = FindFolder(spec->vRefNum, 'trsh', true, &tv, &td);
    if (err)
        return err;
    err = FSMakeFSSpec(tv, td, NULL, &trash);
    if (err)
        return err;
    memcpy(base, spec->name + 1, spec->name[0]);
    base[spec->name[0] > 25 ? 25 : spec->name[0]] = 0;
    memcpy(name, spec->name, spec->name[0] + 1);
    for (n = 2; FSMakeFSSpec(tv, td, name, &probe) == noErr ||
                (n > 2 && FSMakeFSSpec(cur.vRefNum, cur.parID, name,
                                       &probe) == noErr);
         n++) {
        snprintf(try, sizeof try, "%s %d", base, n);
        pstr(name, try);
    }
    if (name[0] != spec->name[0] ||
        memcmp(name + 1, spec->name + 1, name[0]) != 0) {
        err = FSpRename(&cur, name);
        if (err)
            return err;
        err = FSMakeFSSpec(cur.vRefNum, cur.parID, name, &cur);
        if (err)
            return err;
    }
    return FSpCatMove(&cur, &trash);
}

static OSErr adjust_cfrg(FSSpec *spec, long delta)
{
    short saved = CurResFile(), ref, i, n;
    OSErr err;
    ref = FSpOpenResFile(spec, fsRdWrPerm);
    if (ref == -1)
        return ResError() ? ResError() : resFNotFound;
    UseResFile(ref);
    n = Count1Resources('cfrg');
    for (i = 1; i <= n; i++) {
        Handle h = Get1IndResource('cfrg', i);
        if (!h)
            continue;
        HLock(h);
        cfrg_adjust((unsigned char *)*h, (size_t)GetHandleSize(h), delta);
        HUnlock(h);
        ChangedResource(h);
    }
    UpdateResFile(ref);
    err = ResError();
    CloseResFile(ref);
    UseResFile(saved);
    return err;
}

/* Looking ----------------------------------------------------------------- */

static void examine(Setup *s)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    FSSpec app, spec, dir;
    LONGINT vm = 0;
    long id;
    int i;

    memset(s, 0, sizeof *s);
    s->kernel = kKernelUnknown;
    if (Gestalt(gestaltVMAttr, &vm) == noErr && (vm & kVMPresent))
        s->vm_on = 1;
    if (FindFolder(kOnSystemDisk, 'macs', false, &s->vref, &s->sys_dir))
        return;
    s->have_rom = find_rom(s);
    if (s->have_rom)
        s->kernel = kernel_of(&s->rom, &s->rom_err);

    if (make_spec(s->vref, kRootDir, kBackupName, &dir) == noErr &&
        (id = folder_id(&dir)) != 0 && s->have_rom &&
        FSMakeFSSpec(s->vref, id, s->rom.name, &s->backup) == noErr)
        s->have_backup = 1;

    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    info.processAppSpec = &app;
    GetCurrentProcess(&psn);
    if (GetProcessInformation(&psn, &info) == noErr) {
        s->app_vref = app.vRefNum;
        s->app_dir = app.parID;
    }
    s->programs_here = 1;
    for (i = 0; i < kNPrograms; i++) {
        short fv;
        long fd;
        FInfo fi;
        if (make_spec(s->app_vref, s->app_dir, kPrograms[i].name, &spec) ||
            FSpGetFInfo(&spec, &fi) || fi.fdType != kPrograms[i].type)
            s->programs_here = 0;
        if (FindFolder(kOnSystemDisk, kPrograms[i].folder, false, &fv, &fd) ==
                noErr &&
            make_spec(fv, fd, kPrograms[i].name, &spec) == noErr)
            s->programs_installed = 1;
    }
}

static void show(const Setup *s)
{
    char text[400];
    int can_install = 0, can_remove = s->have_backup || s->programs_installed;
    if (!s->have_rom) {
        snprintf(text, sizeof text,
                 "Your System Folder has no Mac OS ROM file. Enforcer works "
                 "only on Macs that start up from one: Power Macs with USB.");
    } else if (s->kernel == kKernelStock) {
        snprintf(text, sizeof text,
                 "Your Mac OS ROM file has NanoKernel 2.28 as Apple made it. "
                 "To install Enforcer, click Install, then restart.");
        can_install = 1;
    } else if (s->kernel == kKernelPatched) {
        snprintf(text, sizeof text,
                 "Enforcer is installed. To put back the original Mac OS ROM "
                 "file and remove Enforcer, click Remove. To update the "
                 "extension and control panel, click Install.");
        can_install = 1;
    } else {
        snprintf(text, sizeof text,
                 "Enforcer cannot be installed on this Mac: its Mac OS ROM "
                 "file %s.", rom_error_text(s->rom_err));
    }
    if (can_install && !s->programs_here) {
        snprintf(text, sizeof text,
                 "To install, put the Enforcer Extension and the Enforcer "
                 "control panel in the same folder as this installer.");
        can_install = 0;
    }
    if (s->vm_on && strlen(text) < 200)
        strcat(text, " Virtual memory is on: Enforcer starts only when it "
                     "is off (Memory control panel).");
    set_text(kStatus, "%s", text);
    enable(kInstall, can_install);
    enable(kRemove, can_remove);
}

/* Changing ------------------------------------------------------------- */

static int fail(const char *what, OSErr err)
{
    set_text(kStatus, "%s (error %d). Nothing else was changed.", what,
             (int)err);
    return 0;
}

/* Writes the patched copy, "Mac OS ROM (Enforcer)", into the System
   Folder. */
static OSErr write_rom(const Setup *s, const unsigned char *data, size_t len,
                       const unsigned char *rsrc, LONGINT rlen, long delta,
                       FSSpec *tmp)
{
    FInfo fi;
    OSErr err = make_spec(s->vref, s->sys_dir, kTempName, tmp);
    if (err == noErr)
        err = FSpDelete(tmp); /* left from an install that failed */
    if (err && err != fnfErr)
        return err;
    err = FSpCreate(tmp, 'chrp', 'TEMP', smSystemScript);
    if (err)
        return err;
    err = write_fork(tmp, 0, data, (LONGINT)len);
    if (!err)
        err = write_fork(tmp, 1, rsrc, rlen);
    if (!err)
        err = adjust_cfrg(tmp, delta);
    if (!err) {
        fi = s->rom_info;
        fi.fdFlags &= ~kFlagInited;
        err = FSpSetFInfo(tmp, &fi);
    }
    if (err)
        FSpDelete(tmp);
    return err;
}

/* Moves the original into Enforcer Backup and puts the copy in its
   place. */
static OSErr swap_in(Setup *s, FSSpec *tmp)
{
    FSSpec dir, old;
    long id;
    OSErr err = make_spec(s->vref, kRootDir, kBackupName, &dir);
    if (err == fnfErr)
        err = FSpDirCreate(&dir, smSystemScript, &id);
    else if (err == noErr && !(id = folder_id(&dir)))
        err = dupFNErr; /* a file has the folder's name */
    if (err)
        return err;
    if (FSMakeFSSpec(s->vref, id, s->rom.name, &old) == noErr) {
        err = FSpDelete(&old); /* an older backup */
        if (err)
            return err;
    }
    err = FSpCatMove(&s->rom, &dir);
    if (err)
        return err;
    err = FSpRename(tmp, s->rom.name);
    FlushVol(NULL, s->vref);
    return err;
}

static OSErr copy_program(const Setup *s, int i)
{
    FSSpec src, dst;
    FInfo fi;
    unsigned char *buf;
    LONGINT len;
    short fv;
    long fd;
    int fork;
    OSErr err = make_spec(s->app_vref, s->app_dir, kPrograms[i].name, &src);
    if (!err)
        err = FSpGetFInfo(&src, &fi);
    if (!err)
        err = FindFolder(kOnSystemDisk, kPrograms[i].folder, true, &fv, &fd);
    if (err)
        return err;
    err = make_spec(fv, fd, kPrograms[i].name, &dst);
    if (err == noErr) {
        err = FSpDelete(&dst);
        if (err == fBsyErr) /* running: Mac OS has it open */
            err = move_to_trash(&dst);
        if (err)
            return err;
        make_spec(fv, fd, kPrograms[i].name, &dst);
    } else if (err != fnfErr) {
        return err;
    }
    err = FSpCreate(&dst, fi.fdCreator, fi.fdType, smSystemScript);
    for (fork = 0; fork < 2 && !err; fork++) {
        err = read_fork(&src, fork, &buf, &len);
        if (!err)
            err = write_fork(&dst, fork, buf, len);
        free(buf);
    }
    if (!err) {
        fi.fdFlags &= ~kFlagInited;
        fi.fdLocation.h = fi.fdLocation.v = 0;
        err = FSpSetFInfo(&dst, &fi);
    }
    return err;
}

static int install(Setup *s)
{
    unsigned char *data, *rsrc, *out;
    LONGINT dlen, rlen;
    size_t olen;
    long delta;
    FSSpec tmp;
    Rom rom;
    OSErr oe;
    int err, i;

    if (s->kernel == kKernelStock) {
        set_text(kStatus, "Reading the Mac OS ROM file.");
        oe = read_fork(&s->rom, 0, &data, &dlen);
        if (oe)
            return fail("Enforcer could not read the Mac OS ROM file", oe);
        oe = read_fork(&s->rom, 1, &rsrc, &rlen);
        if (oe) {
            free(data);
            return fail("Enforcer could not read the Mac OS ROM file", oe);
        }
        err = rom_open(&rom, data, (size_t)dlen);
        if (!err)
            err = rom_patch(&rom, &out, &olen, &delta, progress);
        rom_close(&rom);
        free(data);
        if (err) {
            free(rsrc);
            set_text(kStatus, "Enforcer could not patch the Mac OS ROM file: "
                              "%s. Nothing was changed.", rom_error_text(err));
            return 0;
        }
        set_text(kStatus, "Writing the patched Mac OS ROM file.");
        oe = write_rom(s, out, olen, rsrc, rlen, delta, &tmp);
        free(out);
        free(rsrc);
        if (oe)
            return fail("Enforcer could not write the patched Mac OS ROM "
                        "file", oe);
        oe = swap_in(s, &tmp);
        if (oe) {
            set_text(kStatus, "Enforcer could not replace the Mac OS ROM file "
                              "(error %d). Your System Folder may now hold "
                              "\xD2%s\xD3: you can still restart.",
                     (int)oe, kTempName);
            return 0;
        }
    }
    for (i = 0; i < kNPrograms; i++) {
        set_text(kStatus, "Installing %s.", kPrograms[i].name);
        oe = copy_program(s, i);
        if (oe) {
            set_text(kStatus, "Enforcer could not install %s (error %d).",
                     kPrograms[i].name, (int)oe);
            return 0;
        }
    }
    set_text(kStatus, "Enforcer is installed. To start it, restart your "
                      "Mac. The original Mac OS ROM file is in the folder "
                      "\xD2%s\xD3 on your startup disk.", kBackupName);
    return 1;
}

static int remove_enforcer(Setup *s)
{
    FSSpec restoring, sys, dir;
    Str255 name;
    OSErr oe;
    int rom_err, i;

    if (s->have_backup && s->kernel != kKernelStock) {
        if (kernel_of(&s->backup, &rom_err) != kKernelStock) {
            set_text(kStatus, "The Mac OS ROM file in \xD2%s\xD3 is not the "
                              "original, so Enforcer did not put it back.",
                     kBackupName);
            return 0;
        }
        /* The original goes into the System Folder under another name,
           the patched file to the Trash, then the original takes its
           name back. */
        oe = FSpRename(&s->backup, pstr(name, kRestoreName));
        if (!oe)
            oe = FSMakeFSSpec(s->backup.vRefNum, s->backup.parID, name,
                              &restoring);
        if (!oe)
            oe = FSMakeFSSpec(s->vref, s->sys_dir, NULL, &sys);
        if (!oe)
            oe = FSpCatMove(&restoring, &sys);
        if (oe)
            return fail("Enforcer could not put back the original Mac OS "
                        "ROM file", oe);
        make_spec(s->vref, s->sys_dir, kRestoreName, &restoring);
        oe = move_to_trash(&s->rom);
        if (!oe)
            oe = FSpRename(&restoring, s->rom.name);
        FlushVol(NULL, s->vref);
        if (oe) {
            set_text(kStatus, "Enforcer could not finish putting back the "
                              "original Mac OS ROM file (error %d). It is in "
                              "your System Folder as \xD2%s\xD3.",
                     (int)oe, kRestoreName);
            return 0;
        }
        if (make_spec(s->vref, kRootDir, kBackupName, &dir) == noErr)
            FSpDelete(&dir); /* only if empty */
    }
    for (i = 0; i < kNPrograms; i++) {
        FSSpec spec;
        short fv;
        long fd;
        if (FindFolder(kOnSystemDisk, kPrograms[i].folder, false, &fv, &fd) ||
            make_spec(fv, fd, kPrograms[i].name, &spec))
            continue;
        oe = move_to_trash(&spec);
        if (oe) {
            set_text(kStatus, "Enforcer could not remove %s (error %d).",
                     kPrograms[i].name, (int)oe);
            return 0;
        }
    }
    set_text(kStatus, "Enforcer is removed. To finish, restart your Mac.");
    return 1;
}

/* Asks the Finder to restart, which quits every program first. */
static void restart(void)
{
    AEAddressDesc finder;
    AppleEvent ev, reply;
    OSType sig = 'MACS';
    if (AECreateDesc(typeApplSignature, &sig, sizeof sig, &finder))
        return;
    if (AECreateAppleEvent('FNDR', 'rest', &finder, kAutoGenerateReturnID,
                           kAnyTransactionID, &ev) == noErr) {
        AESend(&ev, &reply, kAENoReply, kAENormalPriority, kAEDefaultTimeout,
               NULL, NULL);
        AEDisposeDesc(&ev);
    }
    AEDisposeDesc(&finder);
}

static pascal OSErr ae_quit(const AppleEvent *e, AppleEvent *reply,
                            LONGINT refcon)
{
    ExitToShell();
    return noErr;
}

int main(void)
{
    Setup s;
    INTEGER item;
    short type;
    Handle h;
    Rect r;
    int changed = 0;

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

    g_dialog = GetNewDialog(kDialog, NULL, (WindowPtr)-1);
    if (!g_dialog)
        return 1;
    SetPort((GrafPtr)g_dialog);
    SetDialogDefaultItem(g_dialog, kInstall);
    SetDialogCancelItem(g_dialog, kQuit);
    GetDialogItem(g_dialog, kIcon, &type, &h, &r);
    SetDialogItem(g_dialog, kIcon, type, (Handle)NewUserItemUPP(draw_icon),
                  &r);
    set_text(kAbout,
             "Enforcer stops an application that writes into the memory of "
             "another. Install patches the NanoKernel in your Mac OS ROM "
             "file, keeps the original in the folder \xD2%s\xD3 on your "
             "startup disk, and adds the Enforcer Extension and control "
             "panel.", kBackupName);
    set_text(kStatus, "Reading the Mac OS ROM file.");
    ShowWindow((WindowPtr)g_dialog);
    SetCursor(*GetCursor(kWatchCursor));
    examine(&s);
    show(&s);
    InitCursor();

    for (;;) {
        ModalDialog(NULL, &item);
        if (item == kQuit)
            break;
        if (item == kInstall && changed) {
            restart();
            break;
        }
        if (item != kInstall && item != kRemove)
            continue;
        SetCursor(*GetCursor(kWatchCursor));
        enable(kInstall, 0);
        enable(kRemove, 0);
        enable(kQuit, 0);
        if (item == kInstall ? install(&s) : remove_enforcer(&s)) {
            changed = 1;
            retitle(kInstall, "Restart");
            enable(kInstall, 1);
        } else {
            Setup again;
            char keep[256];
            short t;
            Handle th;
            Rect tr;
            Str255 st;
            /* Show the error, with the buttons for whatever is there now. */
            GetDialogItem(g_dialog, kStatus, &t, &th, &tr);
            GetDialogItemText(th, st);
            memcpy(keep, st + 1, st[0]);
            keep[st[0]] = 0;
            examine(&again);
            show(&again);
            set_text(kStatus, "%s", keep);
            s = again;
        }
        enable(kQuit, 1);
        InitCursor();
    }
    return 0;
}
