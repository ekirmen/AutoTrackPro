#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  AutoTrackPro v4.0 – Auto Frame Engine
//  FFGL Plugin para Resolume Arena
//  Replica el comportamiento de NVIDIA Broadcast Auto Frame:
//  · Detecta cuerpo/rostro con skin detection + motion heuristics
//  · Zoom digital suave centrado en el sujeto
//  · Seguimiento predictivo con inercia cinematográfica
//  · Reencuadre automático estilo operador humano
//  by @thex_led
// ─────────────────────────────────────────────────────────────────────────────

#include "../../lib/ffglquickstart/FFGLEffect.h"
#include "../../lib/ffglex/FFGLShader.h"
#include "../../lib/ffglex/FFGLFBO.h"
#include "../../lib/ffglquickstart/FFGLScreenQuad.h"
#include <vector>
#include <algorithm>
#include <cmath>
#include <deque>

// Resolución del buffer de análisis (más alto = más precisión, más lento)
static constexpr int  TRACK_BUF  = 128;
// Máximo de blobs de sujeto rastreados simultáneamente
static constexpr int  MAX_BLOBS  = 8;
// Frames de historia para suavizado de posición (filtro de media móvil)
static constexpr int  HIST_LEN   = 12;

// ─── Blob: región de interés detectada ───────────────────────────────────────
struct Blob
{
    float cx     = 0.5f;   // Centro X normalizado [0,1]
    float cy     = 0.5f;   // Centro Y normalizado [0,1]
    float w      = 0.15f;  // Ancho del bounding box normalizado
    float h      = 0.20f;  // Alto del bounding box normalizado
    float mass   = 0.0f;   // Masa acumulada (confianza)
    float life   = 0.0f;   // Vida: 1.0=activo, 0.0=muerto
    int   id     = -1;
    float vx     = 0.0f;   // Velocidad X (predicción)
    float vy     = 0.0f;   // Velocidad Y (predicción)
};

// ─── AutoTrackPro: clase principal del plugin ─────────────────────────────────
class AutoTrackPro : public ffglqs::Effect
{
public:
    AutoTrackPro();
    ~AutoTrackPro();

    FFResult InitGL  ( const FFGLViewportStruct* vp ) override;
    FFResult DeInitGL()                               override;
    FFResult Render  ( ProcessOpenGLStruct* pGL )     override;

private:
    // ── Shaders ──────────────────────────────────────────────────────────────
    ffglex::FFGLShader detectShader;   // Detección skin+motion → mapa de calor
    ffglex::FFGLShader frameShader;    // Render final: crop+zoom+follow
    ffglex::FFGLShader copyShader;     // Copia frame anterior

    // ── FBOs ─────────────────────────────────────────────────────────────────
    ffglex::FFGLFBO    prevFBO;        // Frame anterior para optical flow
    ffglex::FFGLFBO    heatFBO;        // Mapa de calor skin+motion

    // ── Screen Quad ───────────────────────────────────────────────────────────
    ffglqs::ScreenQuad quad;

    // ── Viewport ──────────────────────────────────────────────────────────────
    FFGLViewportStruct currentViewport = {};

    // ── Estado del Auto Frame ─────────────────────────────────────────────────
    // Posición actual del encuadre (lo que ve la "cámara virtual")
    float camX        = 0.5f;   // Centro X actual del crop
    float camY        = 0.5f;   // Centro Y actual del crop
    float camZoom     = 1.5f;   // Zoom actual (1.0=sin zoom, 3.0=muy cerca)

    // Posición objetivo (donde quiere ir la cámara)
    float tgtX        = 0.5f;
    float tgtY        = 0.5f;
    float tgtZoom     = 1.5f;

    // Velocidad de la cámara virtual (inercia cinematográfica)
    float camVX       = 0.0f;
    float camVY       = 0.0f;
    float camVZ       = 0.0f;   // Velocidad de zoom

    // ── Blobs detectados ──────────────────────────────────────────────────────
    std::vector<Blob>  blobs;
    int                blobIdCounter = 0;
    int                lockedBlobId  = -1;  // ID del sujeto bloqueado
    int                lostFrames    = 0;   // Frames sin detección

    // ── Historia de posición (suavizado tipo Kalman simplificado) ─────────────
    std::deque<float>  histX;
    std::deque<float>  histY;
    std::deque<float>  histW;   // Ancho del sujeto (para calcular zoom)
    std::deque<float>  histH;   // Alto del sujeto

    // ── Buffer de pixels del heat map ─────────────────────────────────────────
    std::vector<unsigned char> heatPixels;
    std::vector<float>         motionAcc;   // Acumulador temporal anti-parpadeo

    // ── Métodos internos ──────────────────────────────────────────────────────
    void  DetectBlobs   ();
    void  SelectTarget  (bool lockOn, float zoomPad, bool autoZoom, float manualZoom);
    void  StepCamera    (float smoothness);
    float Lerp          (float a, float b, float t) const { return a + (b - a) * t; }
    float Clamp         (float v, float lo, float hi) const { return v < lo ? lo : (v > hi ? hi : v); }
    float SmoothDamp    (float cur, float tgt, float& vel, float smooth) const;
};
