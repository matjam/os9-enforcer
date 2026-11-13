#include "Processes.r"
#include "Finder.r"
#include "Dialogs.r"

/* 2.8 MB for the Mac OS ROM file's data fork, 4 MB for its ROM image,
   up to 4.7 MB for the image compressed again, and 2.8 MB for the new data
   fork. */
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
	24 * 1024 * 1024,
	20 * 1024 * 1024
};

resource 'DLOG' (128) {
	{0, 0, 262, 450}, movableDBoxProc, invisible, noGoAway, 0x0, 128,
	"Enforcer Installer", centerMainScreen
};

/* The texts are set by installer.c. */
resource 'DITL' (128) {
	{
		{228, 352, 248, 434}, Button { enabled, "Install" };
		{228, 168, 248, 250}, Button { enabled, "Quit" };
		{228, 260, 248, 342}, Button { enabled, "Remove" };
		{16, 66, 112, 434}, StaticText { disabled, "" };
		{120, 66, 214, 434}, StaticText { disabled, "" };
		{16, 18, 48, 50}, UserItem { disabled };
	}
};

/* Appearance: the theme's background and controls. */
data 'dlgx' (128) {
	$"0000 0000 0009"
};

/* The Finder's icon: the bundle maps the file type to icon family 128,
   the control panel's (../enforcer/icon-panel.r). */
resource 'BNDL' (128) {
	'EnfI', 0,
	{
		'ICN#', { 0, 128 };
		'FREF', { 0, 128 }
	}
};

resource 'FREF' (128) {
	'APPL', 0, ""
};

data 'EnfI' (0) {
	$"00"
};
