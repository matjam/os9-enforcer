#include "Processes.r"

/* Retro68's SIZE (RetroPPCAPPL.r), but high-level-event aware: the X test
   sends the Enforcer Extension an Apple event, which AESend refuses
   (noPortErr) from an application that is not. */
resource 'SIZE' (-1) {
	reserved,
	ignoreSuspendResumeEvents,
	reserved,
	cannotBackground,
	needsActivateOnFGSwitch,
	backgroundAndForeground,
	dontGetFrontClicks,
	ignoreChildDiedEvents,
	is32BitCompatible,
	isHighLevelEventAware,
	onlyLocalHLEvents,
	notStationeryAware,
	dontUseTextEditServices,
	reserved,
	reserved,
	reserved,
	1024 * 1024,
	1024 * 1024
};
