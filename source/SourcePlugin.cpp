/*
	The generator registration.

	**This file is listed directly in the plugin target, not in an archive.**
	`CFFGLPluginInfo` registers itself from a file-scope constructor and nothing
	ever references it by name, so in a STATIC library the linker is entitled to
	drop the whole translation unit -- giving a bundle that loads, exports
	plugMain, and cheerfully reports that it contains no plugins.

		nm -gU Resodoom.bundle/Contents/MacOS/Resodoom | grep plugMain
*/
#include "Plugin.h"

#include <FFGLSDK.h>

static CFFGLPluginInfo PluginInfo(
	PluginFactory< resodoom::ResodoomPlugin >,       // Create method
	"RD01",                                          // Unique ID, max 4 chars
	"SW Resodoom",                                   // Plugin name, max 16 chars
	2,                                               // API major version
	1,                                               // API minor version
	0,                                               // Plugin major version
	1,                                               // Plugin minor version
	FF_SOURCE,                                       // Plugin type
	"Doom as a live Resolume source.\n\nPoint it at a WAD and the game becomes a layer: composite it, key it, run it through other effects, and MIDI-map the controls onto whatever is already on the desk.\n\nWith nothing mapped it plays Doom's own attract demos forever, which is usually what you want from a layer nobody is holding a controller for.\n\nNo game data is shipped with this plugin and none ever will be. You supply your own WAD, exactly as you would with any other source port. Freedoom is free, complete and a good place to start.",
	"Resodoom FFGL source"                           // About
);
