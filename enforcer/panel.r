#include "Processes.r"
#include "Finder.r"

/* The Finder's icon for the control panel: the bundle maps its file type
   to icon family 128 (icon-panel.r, from tools/make-icons.py). The Finder
   reads the bundle only when the file's hasBundle flag is set, which
   tools/finder-flags.py does after the build. */
resource 'BNDL' (128) {
	'Enfr', 0,
	{
		'ICN#', { 0, 128 };
		'FREF', { 0, 128 }
	}
};

resource 'FREF' (128) {
	'APPC', 0, ""
};

/* The owner resource, named for the creator. */
data 'Enfr' (0) {
	$"00"
};

resource 'SIZE' (-1) {
	reserved,
	acceptSuspendResumeEvents,
	reserved,
	canBackground,
	doesActivateOnFGSwitch,
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
	512 * 1024,
	384 * 1024
};
