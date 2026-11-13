/*
 * enforcer.h: what the Enforcer Extension and the Enforcer control panel
 * share, besides the engine's State (common/engine.h).
 */

#ifndef ENFORCER_H
#define ENFORCER_H

enum {
    /* Gestalt 'Enfr' values below 0x1000 are status codes, not a State. */
    kEnforcerVMOn = 1,       /* earlier versions: virtual memory is on */
    kEnforcerNoKernel = 2,   /* the NanoKernel is not patched: not started */
    kEnforcerOtherKernel = 3, /* another release's kernel patch: run the
                                 installer again */
    kEnforcerFailed = 0x100, /* + -error: engine_start failed, or the
                                kernel's owner check (State.kcb_err) */
    kEnforcerStatusMax = 0x1000,

    kEnforcerCreator = 'Enfr', /* the control panel and the preferences */
    kEnforcerExtCreator = 'EnfX', /* the extension */
    /* Apple event kEnforcerExtCreator/kEnforcerExitEvent: the extension
       quits without stopping the engine, as a crash would (tests). */
    kEnforcerExitEvent = 'exit',
    kEnforcerPrefsMagic = 'EnfP'
};

/* "Enforcer Preferences", a Pascal string. */
#define kEnforcerPrefsName ((const unsigned char *)"\024Enforcer Preferences")

#endif
