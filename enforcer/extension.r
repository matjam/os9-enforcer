#include "Processes.r"

/* A background-only application: no windows, no menu bar, launched at
   startup from the Extensions folder. High-level-event aware, so it gets
   the quit Apple event at shutdown and can clean up. */
resource 'SIZE' (-1) {
	reserved,
	ignoreSuspendResumeEvents,
	reserved,
	canBackground,
	needsActivateOnFGSwitch,
	onlyBackground,
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
	256 * 1024,
	256 * 1024
};
