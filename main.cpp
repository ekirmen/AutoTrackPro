#include <windows.h>
#include "../../lib/FFGLSDK.h"
#include "AutoTrackPro.h"

// Entry point for Resolume to load the AutoTrackPro DLL
static CFFGLPluginInfo g_PluginInfo(
    PluginFactory< AutoTrackPro >,
    "ATP7",                                          // Plugin unique ID (4 chars)
    "AutoTrackPro",                                  // Plugin display name
    2,                                               // API major version
    1,                                               // API minor version
    3,                                               // Plugin major version
    0,                                               // Plugin minor version (v3.0)
    FF_EFFECT,
    "Advanced Biometric AR Tracking for Resolume",   // Description
    "thex @thex_led"                                 // Author
);
