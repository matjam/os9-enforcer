#include "Processes.r"
#include "Finder.r"

/* The Finder's icon for the extension: the bundle maps its file type to
   icon family 128 (icon-extension.r, from tools/make-icons.py). The Finder
   reads the bundle only when the file's hasBundle flag is set, which
   tools/finder-flags.py does after the build. */
resource 'BNDL' (128) {
	'EnfX', 0,
	{
		'ICN#', { 0, 128 };
		'FREF', { 0, 128 }
	}
};

resource 'FREF' (128) {
	'appe', 0, ""
};

/* The owner resource, named for the creator. */
data 'EnfX' (0) {
	$"00"
};

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
