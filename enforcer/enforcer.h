/*
 * enforcer.h: what the Enforcer Extension and the Enforcer control panel
 * share, besides the engine's State (common/engine.h).
 */

#ifndef ENFORCER_H
#define ENFORCER_H

enum {
    /* Gestalt 'Enfr' values below 0x1000 are status codes, not a State. */
    kEnforcerVMOn = 1,       /* virtual memory is on: not started */
    kEnforcerFailed = 0x100, /* + -error: engine_start failed */
    kEnforcerStatusMax = 0x1000,

    kEnforcerCreator = 'Enfr', /* the control panel and the preferences */
    kEnforcerPrefsMagic = 'EnfP'
};

/* "Enforcer Preferences", a Pascal string. */
#define kEnforcerPrefsName ((const unsigned char *)"\024Enforcer Preferences")

#endif
