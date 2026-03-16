#pragma once
#include "../../lib/ffglquickstart/FFGLEffect.h"
#include "../../lib/ffglex/FFGLShader.h"
#include "../../lib/ffglex/FFGLFBO.h"
#include "../../lib/ffglquickstart/FFGLScreenQuad.h"
#include <vector>
#include <algorithm>
#include <cmath>

// ─────────────────────────────────────────────
//  AutoTrackPro v3.0 – Motor Biométrico Heurístico
//  FFGL Plugin para Resolume Arena
//  by @thex_led
// ─────────────────────────────────────────────

// Número máximo de clusters y landmarks AR simultáneos
static constexpr int MAX_CLUSTERS  = 16;
static constexpr int MAX_LANDMARKS = 12;

// Resolución del buffer de análisis de movimiento
static constexpr int MOTION_BUF_SIZE = 96;  // Subido de 64 → 96 para más precisión

struct MotionCluster
{
    float x      = 0.5f;
    float y      = 0.5f;
    float weight = 0.0f;
    float life   = 0.0f;  // 0.0 = muerto, 1.0 = activo
    int   id     = -1;    // ID único para tracking persistente
    float velX   = 0.0f;  // Velocidad interna del cluster (predicción)
    float velY   = 0.0f;
};

class AutoTrackPro : public ffglqs::Effect
{
public:
    AutoTrackPro();
    ~AutoTrackPro();

    FFResult InitGL  ( const FFGLViewportStruct* vp ) override;
    FFResult DeInitGL()                               override;
    FFResult Render  ( ProcessOpenGLStruct* pGL )     override;

private:
    // ── Shaders ──────────────────────────────
    ffglex::FFGLShader finalShader;   // Render final + HUD AR
    ffglex::FFGLShader trackShader;   // Detección skin + optical flow
    ffglex::FFGLShader copyShader;    // Copia frame anterior

    // ── Frame Buffers ─────────────────────────
    ffglex::FFGLFBO    prevFrameFBO;  // Frame anterior (comparación)
    ffglex::FFGLFBO    motionFBO;     // Mapa de calor de movimiento/piel

    // ── Screen Quad ───────────────────────────
    ffglqs::ScreenQuad quad;

    // ── Viewport guardado ─────────────────────
    FFGLViewportStruct currentViewport = {};

    // ── Estado del tracking ───────────────────
    float currentX    = 0.5f;
    float currentY    = 0.5f;
    float targetX     = 0.5f;
    float targetY     = 0.5f;
    float activeFocusX = 0.5f;
    float activeFocusY = 0.5f;
    float velX        = 0.0f;
    float velY        = 0.0f;
    float currentZoom = 1.0f;
    float targetZoom  = 1.0f;
    int   lostFrames  = 0;

    // ── Landmarks AR ──────────────────────────
    float pointsX[MAX_LANDMARKS] = {};
    float pointsY[MAX_LANDMARKS] = {};
    int   pointsCount = 0;

    // ── Sistema de clustering persistente ─────
    std::vector<MotionCluster> persistentClusters;
    int  clusterIdCounter = 0;
    int  lockedClusterId  = -1;

    // ── Historia de movimiento (anti-parpadeo) ─
    std::vector<float> motionHistory;

    // ── Helpers internos ─────────────────────
    void  UpdateClusters(const std::vector<unsigned char>& pixels);
    void  ClassifyLandmarks(bool lockOn, float zoomVal, bool autoZoom);
    void  StepMotionPhysics(float inertia);
    float ClampF(float v, float lo, float hi) const { return v < lo ? lo : (v > hi ? hi : v); }
};
