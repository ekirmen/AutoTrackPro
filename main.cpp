#include <windows.h>
#include "../../lib/FFGLSDK.h"
#include "AutoTrackPro.h"

// Entry point for Resolume to load the AutoTrackPro DLL
static CFFGLPluginInfo g_PluginInfo(
	PluginFactory< AutoTrackPro >,
	"ATP7",               // Plugin unique ID (4 chars)
	"AutoTrackPro",      
	2,
	1,
	2,
	8,                    // v2.8
	FF_EFFECT,
	"Advanced Biometric AR Tracking for Resolume",
	"thex @thex_led"
);
