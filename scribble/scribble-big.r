#include "Processes.r"

/* Scribble Big: the same program asking for 1 MB, which with sparse
   partitions (640 KB or more) launches it into one. Scribble itself asks
   for 512 KB and keeps an ordinary partition, for the overflow tests. */
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
