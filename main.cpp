#include <windows.h>
#include "../../lib/FFGLSDK.h"
#include "AutoTrackPro.h"

// Entry point for Resolume to load the AutoMaskCam DLL
static CFFGLPluginInfo g_PluginInfo(
	PluginFactory< AutoTrackPro >,
	"ATP7",               // Reorden de sensibilidad
	"AutoTrackPro",      
	2,
	1,
	2,
	8,                    // v2.8
	FF_EFFECT,
	"Tracking Pro",
	"thex @thex_led"
);
