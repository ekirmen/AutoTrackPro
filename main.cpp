// AutoTrackPro v4.0 – Auto Frame Engine
// FFGL Plugin para Resolume Arena
// Replica NVIDIA Broadcast Auto Frame
// by @thex_led

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../../lib/FFGLSDK.h"
#include "AutoTrackPro.h"

static CFFGLPluginInfo g_PluginInfo(
    PluginFactory< AutoTrackPro >,
    "ATP4",                                          // ID único 4 chars
    "AutoTrackPro",                                  // Nombre en Resolume
    2, 1,                                            // FFGL version
    4, 0,                                            // Plugin version v4.0
    FF_EFFECT,                                       // Tipo: efecto
    "Auto Frame – NVIDIA-style subject tracking",    // Descripción
    "thex @thex_led"                                 // Autor
);
