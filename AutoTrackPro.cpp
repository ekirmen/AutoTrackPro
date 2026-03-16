// ─────────────────────────────────────────────────────────────────────────────
//  AutoTrackPro v3.0 – Motor Biométrico Heurístico para Resolume Arena
//  FFGL Plugin · by @thex_led
//
//  MEJORAS v3.0 respecto a v2.8:
//  · Buffer de análisis subido de 64×64 a 96×96 (más precisión de cluster)
//  · Filtro de piel mejorado: rango HSV aproximado en GPU (más robusto)
//  · Predicción de posición de cluster con velocidad interna (anti-lag)
//  · Zoom suavizado independiente con targetZoom (sin saltos)
//  · Lógica de clustering refactorizada en métodos privados
//  · Shader final: grosor de líneas AR adaptativo al zoom
//  · Hard Lock: radio de aceptación de manos ajustado dinámicamente
//  · Parámetro "Skin Tone" para ajustar el umbral de detección de piel
//  · Parámetro "HUD Opacity" para controlar la intensidad del overlay AR
//  · Corrección de clamp de cuadro para evitar bordes negros en zoom alto
// ─────────────────────────────────────────────────────────────────────────────

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "AutoTrackPro.h"

#include "../../lib/ffglquickstart/FFGLParamBool.h"
#include "../../lib/ffglquickstart/FFGLParamRange.h"
#include "../../lib/ffglquickstart/FFGLParamTrigger.h"
#include "../../lib/ffglex/FFGLScopedFBOBinding.h"
#include "../../lib/ffglex/FFGLScopedShaderBinding.h"

using namespace ffglex;
using namespace ffglqs;

// ─────────────────────────────────────────────────────────────────────────────
//  SHADERS GLSL
// ─────────────────────────────────────────────────────────────────────────────

static const char* VERTEX_SRC = R"GLSL(
#version 410 core
layout(location = 0) in vec2 aPos;
out vec2 i_uv;
void main() {
    i_uv        = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

// ── Shader de copia simple ────────────────────────────────────────────────────
static const char* COPY_FS = R"GLSL(
#version 410 core
uniform sampler2D inputTex;
in  vec2 i_uv;
out vec4 fragColor;
void main() { fragColor = texture(inputTex, i_uv); }
)GLSL";

// ── Shader de tracking: skin detection + optical flow ────────────────────────
// MEJORA v3.0: filtro de piel basado en ratio R/G/B más robusto,
// tolerante a distintos tonos de piel y condiciones de iluminación escénica.
static const char* TRACK_FS = R"GLSL(
#version 410 core
uniform sampler2D currentFrame;
uniform sampler2D prevFrame;
uniform float     sensitivity;
uniform float     skinTone;      // 0.0 = piel clara, 1.0 = piel oscura

in  vec2 i_uv;
out vec4 fragColor;

// Aproximación de RGB a YCbCr para detección de piel más robusta
vec3 rgbToYCbCr(vec3 c) {
    float Y  =  0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    float Cb = -0.169 * c.r - 0.331 * c.g + 0.500 * c.b + 0.5;
    float Cr =  0.500 * c.r - 0.419 * c.g - 0.081 * c.b + 0.5;
    return vec3(Y, Cb, Cr);
}

void main() {
    vec4  col  = texture(currentFrame, i_uv);
    vec3  pCol = texture(prevFrame,    i_uv).rgb;

    // Detección de piel en espacio YCbCr (más estable que RGB puro)
    vec3  ycbcr = rgbToYCbCr(col.rgb);
    float Y     = ycbcr.x;
    float Cb    = ycbcr.y;
    float Cr    = ycbcr.z;

    // Rangos estándar de piel en YCbCr, modulados por skinTone
    float cbLo = mix(0.38, 0.32, skinTone);
    float cbHi = mix(0.50, 0.55, skinTone);
    float crLo = mix(0.52, 0.48, skinTone);
    float crHi = mix(0.68, 0.72, skinTone);

    float skinMask = step(cbLo, Cb) * step(Cb, cbHi)
                   * step(crLo, Cr) * step(Cr, crHi)
                   * step(0.1,  Y);

    // Optical flow: diferencia temporal + gradiente espacial
    float diff = length(col.rgb - pCol);
    vec2  texel = vec2(1.0 / float(MOTION_BUF), 1.0 / float(MOTION_BUF));
    float gx = texture(currentFrame, i_uv + vec2(texel.x, 0.0)).r
             - texture(currentFrame, i_uv - vec2(texel.x, 0.0)).r;
    float gy = texture(currentFrame, i_uv + vec2(0.0, texel.y)).r
             - texture(currentFrame, i_uv - vec2(0.0, texel.y)).r;
    float flow = length(vec2(gx, gy)) * diff;

    // Combinar: movimiento con peso de piel (piel pesa 85%, movimiento puro 15%)
    float mask = smoothstep(0.04, 0.30, flow * sensitivity)
               * (skinMask * 0.85 + 0.15);

    // Codificar posición UV en RG y máscara en B
    fragColor = vec4(i_uv.x, i_uv.y, mask, 1.0);
}
)GLSL";

// ── Shader final: zoom + HUD AR ───────────────────────────────────────────────
// MEJORA v3.0: grosor de líneas adaptativo al zoom, HUD opacity controlable,
// puntos con halo de brillo (glow), línea de cuello más natural.
static const char* FINAL_FS = R"GLSL(
#version 410 core
uniform sampler2D inputTexture;
uniform float     zoom;
uniform vec2      center;
uniform float     xOffset;
uniform float     yOffset;
uniform bool      showTrack;
uniform bool      mirror;
uniform bool      isLocked;
uniform float     hudOpacity;
uniform vec2      points[12];
uniform int       pointsCount;

in  vec2 i_uv;
out vec4 fragColor;

// ── Primitivas AR ─────────────────────────────────────────────────────────────
float drawPoint(vec2 p, vec2 uv, float size) {
    float d = length(uv - p);
    return smoothstep(size, size * 0.4, d);
}

float drawGlow(vec2 p, vec2 uv, float size) {
    float d = length(uv - p);
    return smoothstep(size * 3.0, 0.0, d) * 0.35;
}

float drawLine(vec2 a, vec2 b, vec2 uv, float thickness) {
    vec2  pa = uv - a;
    vec2  ba = b  - a;
    float h  = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return smoothstep(thickness, 0.0, length(pa - ba * h));
}

void main() {
    // ── Zoom + offset ─────────────────────────────────────────────────────────
    vec2 uv_eff = i_uv;
    if (mirror) uv_eff.x = 1.0 - uv_eff.x;

    vec2 halfSize    = vec2(0.5) / zoom;
    vec2 finalCenter = vec2(center.x + xOffset, center.y + yOffset);
    vec2 uv          = mix(finalCenter - halfSize, finalCenter + halfSize, uv_eff);

    vec4 color = texture(inputTexture, uv);
    // Oscurecer bordes fuera de rango (evita stretch)
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        color *= 0.08;

    // ── HUD AR ────────────────────────────────────────────────────────────────
    if (showTrack && pointsCount > 0) {
        // Color: cian normal, rojo en Hard Lock
        vec3 hudColor = isLocked ? vec3(1.0, 0.1, 0.1) : vec3(0.0, 1.0, 1.0);
        vec3 glowColor = isLocked ? vec3(0.6, 0.0, 0.0) : vec3(0.0, 0.5, 0.8);

        // Grosor adaptativo: más fino con más zoom para no saturar el frame
        float lineW = mix(0.0020, 0.0008, clamp((zoom - 1.0) / 5.0, 0.0, 1.0));
        float dotS  = mix(0.0070, 0.0040, clamp((zoom - 1.0) / 5.0, 0.0, 1.0));

        float dots  = 0.0;
        float lines = 0.0;
        float glow  = 0.0;

        // Punto principal (cara) – más grande
        dots += drawPoint(points[0], i_uv, dotS * 1.4);
        glow += drawGlow (points[0], i_uv, dotS * 1.4);

        // Puntos secundarios de cara (ojos simulados)
        if (pointsCount > 1) dots += drawPoint(points[1], i_uv, dotS * 0.8);
        if (pointsCount > 2) dots += drawPoint(points[2], i_uv, dotS * 0.8);

        // Cuello simulado
        vec2 neck = points[0] - vec2(0.0, 0.07 / zoom);
        lines += drawLine(points[0], neck, i_uv, lineW) * 0.7;

        // Manos: puntos + líneas desde cuello
        for (int i = 3; i < pointsCount; i++) {
            dots  += drawPoint(points[i], i_uv, dotS * 1.1);
            glow  += drawGlow (points[i], i_uv, dotS * 1.1);
            lines += drawLine (neck, points[i], i_uv, lineW * 0.9) * 0.6;
        }

        float hudMask = clamp(dots + lines, 0.0, 1.0);
        float glowMask = clamp(glow, 0.0, 1.0);

        vec3 composite = mix(color.rgb, hudColor,  hudMask  * hudOpacity);
             composite = mix(composite, glowColor, glowMask * hudOpacity * 0.5);

        fragColor = vec4(composite, 1.0);
    } else {
        fragColor = color;
    }
}
)GLSL";

// ─────────────────────────────────────────────────────────────────────────────
//  ÍNDICES DE PARÁMETROS (para GetFloatParameter)
// ─────────────────────────────────────────────────────────────────────────────
enum ParamIdx {
    P_ZOOM       = 0,
    P_SMOOTHNESS = 1,
    P_SENSITIVITY= 2,
    P_X_OFFSET   = 3,
    P_Y_OFFSET   = 4,
    P_SKIN_TONE  = 5,
    P_HUD_OPACITY= 6,
    P_AUTOTRACK  = 7,
    P_MIRROR     = 8,
    P_SHOW_HUD   = 9,
    P_AUTOZOOM   = 10,
    P_LOCK_ON    = 11,
};

// ─────────────────────────────────────────────────────────────────────────────
//  CONSTRUCTOR
// ─────────────────────────────────────────────────────────────────────────────
AutoTrackPro::AutoTrackPro()
{
    SetMinInputs(1);
    SetMaxInputs(1);

    // Parámetros de control de cámara
    AddParam( ParamRange::Create( "Zoom",        2.0f, { 1.0f, 10.0f  } ) );
    AddParam( ParamRange::Create( "Smoothness",  0.70f,{ 0.1f,  0.99f } ) );
    AddParam( ParamRange::Create( "Sensitivity", 0.50f,{ 0.0f,  1.0f  } ) );
    AddParam( ParamRange::Create( "X Offset",    0.0f, {-0.5f,  0.5f  } ) );
    AddParam( ParamRange::Create( "Y Offset",    0.0f, {-0.5f,  0.5f  } ) );

    // Parámetros de detección (NUEVOS en v3.0)
    AddParam( ParamRange::Create( "Skin Tone",   0.3f, { 0.0f,  1.0f  } ) );  // 0=claro 1=oscuro
    AddParam( ParamRange::Create( "HUD Opacity", 0.85f,{ 0.0f,  1.0f  } ) );

    // Toggles
    AddParam( ParamBool::Create( "AutoTrack",  true  ) );
    AddParam( ParamBool::Create( "Mirror View",false  ) );
    AddParam( ParamBool::Create( "Show HUD",   false  ) );
    AddParam( ParamBool::Create( "AutoZoom",   false  ) );
    AddParam( ParamBool::Create( "Lock On",    false  ) );

    AddParam( ParamTrigger::Create( "AutoTrackPro v3.0 by @thex_led" ) );
}

AutoTrackPro::~AutoTrackPro() {}

// ─────────────────────────────────────────────────────────────────────────────
//  INIT GL
// ─────────────────────────────────────────────────────────────────────────────
FFResult AutoTrackPro::InitGL( const FFGLViewportStruct* vp )
{
    currentViewport = *vp;

    // Inyectar la constante del tamaño del buffer en el shader de tracking
    // Reemplazamos el placeholder MOTION_BUF por el valor real
    std::string trackSrc = TRACK_FS;
    auto replaceAll = [](std::string& s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll(trackSrc, "MOTION_BUF", std::to_string(MOTION_BUF_SIZE));

    finalShader.Compile(VERTEX_SRC, FINAL_FS);
    trackShader.Compile(VERTEX_SRC, trackSrc.c_str());
    copyShader .Compile(VERTEX_SRC, COPY_FS);
    quad.Initialise();

    prevFrameFBO.Initialise(MOTION_BUF_SIZE, MOTION_BUF_SIZE, GL_RGBA8);
    motionFBO   .Initialise(MOTION_BUF_SIZE, MOTION_BUF_SIZE, GL_RGBA8);

    motionHistory.assign(MOTION_BUF_SIZE * MOTION_BUF_SIZE, 0.0f);

    return FF_SUCCESS;
}

// ─────────────────────────────────────────────────────────────────────────────
//  DEINIT GL
// ─────────────────────────────────────────────────────────────────────────────
FFResult AutoTrackPro::DeInitGL()
{
    finalShader.FreeGLResources();
    trackShader.FreeGLResources();
    copyShader .FreeGLResources();
    quad.Release();
    prevFrameFBO.Release();
    motionFBO   .Release();
    return FF_SUCCESS;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ACTUALIZAR CLUSTERS desde píxeles del motionFBO
// ─────────────────────────────────────────────────────────────────────────────
void AutoTrackPro::UpdateClusters(const std::vector<unsigned char>& pixels)
{
    const int pixelsCount = MOTION_BUF_SIZE * MOTION_BUF_SIZE;

    // 1. Detectar clusters en el frame actual
    std::vector<MotionCluster> frameClusters;
    frameClusters.reserve(MAX_CLUSTERS);

    for (int i = 0; i < pixelsCount; ++i) {
        float rawMotion = pixels[i * 4 + 2] / 255.0f;
        // Suavizado temporal: exponential moving average
        motionHistory[i] = motionHistory[i] * 0.65f + rawMotion * 0.35f;
        float m = motionHistory[i];

        if (m < 0.28f) continue;

        float px = pixels[i * 4 + 0] / 255.0f;
        float py = pixels[i * 4 + 1] / 255.0f;

        bool merged = false;
        for (auto& c : frameClusters) {
            float dx = c.x - px, dy = c.y - py;
            if (dx * dx + dy * dy < 0.012f) {  // Radio de merge más fino
                float total = c.weight + m;
                c.x      = (c.x * c.weight + px * m) / total;
                c.y      = (c.y * c.weight + py * m) / total;
                c.weight = total;
                merged   = true;
                break;
            }
        }
        if (!merged && (int)frameClusters.size() < MAX_CLUSTERS)
            frameClusters.push_back({ px, py, m, 1.0f, 0, 0.0f, 0.0f });
    }

    // 2. Persistencia: actualizar clusters existentes o crear nuevos
    for (auto& pc : persistentClusters) pc.life -= 0.12f;

    for (auto& cf : frameClusters) {
        bool matched = false;
        for (auto& pc : persistentClusters) {
            float dx = pc.x - cf.x, dy = pc.y - cf.y;
            if (dx * dx + dy * dy < 0.025f) {
                // Predicción con velocidad interna del cluster
                float newX = pc.x * 0.35f + cf.x * 0.65f;
                float newY = pc.y * 0.35f + cf.y * 0.65f;
                pc.velX  = newX - pc.x;
                pc.velY  = newY - pc.y;
                pc.x     = newX;
                pc.y     = newY;
                pc.weight = cf.weight;
                pc.life   = 1.0f;
                matched   = true;
                break;
            }
        }
        if (!matched && (int)persistentClusters.size() < MAX_CLUSTERS) {
            cf.id   = clusterIdCounter++;
            cf.life = 0.5f;
            persistentClusters.push_back(cf);
        }
    }

    // 3. Limpiar clusters muertos o muy débiles
    persistentClusters.erase(
        std::remove_if(persistentClusters.begin(), persistentClusters.end(),
            [](const MotionCluster& c){ return c.life <= 0.0f || c.weight < 0.8f; }),
        persistentClusters.end());
}

// ─────────────────────────────────────────────────────────────────────────────
//  CLASIFICAR LANDMARKS (cara + manos)
// ─────────────────────────────────────────────────────────────────────────────
void AutoTrackPro::ClassifyLandmarks(bool lockOn, float zoomVal, bool autoZoom)
{
    pointsCount = 0;
    if (persistentClusters.empty()) return;

    // Seleccionar target
    MotionCluster* target = nullptr;

    if (lockOn && lockedClusterId != -1) {
        for (auto& pc : persistentClusters)
            if (pc.id == lockedClusterId) { target = &pc; break; }
    }

    if (!target) {
        // Ordenar por peso descendente y tomar el más prominente
        std::sort(persistentClusters.begin(), persistentClusters.end(),
            [](const MotionCluster& a, const MotionCluster& b){ return a.weight > b.weight; });
        target = &persistentClusters[0];
        if (!lockOn) lockedClusterId = target->id;
    }

    if (!target) return;

    auto& main = *target;

    // CARA: punto central + dos puntos laterales (ojos simulados)
    pointsX[0] = main.x;           pointsY[0] = main.y;
    pointsX[1] = main.x - 0.022f;  pointsY[1] = main.y + 0.035f;
    pointsX[2] = main.x + 0.022f;  pointsY[2] = main.y + 0.035f;
    pointsCount = 3;

    // ZOOM: target suave independiente
    if (autoZoom) {
        targetZoom = ClampF(1.3f + main.weight * 0.04f, 1.1f, 3.0f);
    } else {
        targetZoom = zoomVal;
    }

    // MANOS: clusters cercanos al target, radio dinámico según zoom
    float handRadiusMax = lockOn ? ClampF(0.5f / zoomVal, 0.2f, 0.55f) : 0.65f;
    int   hands = 0;

    for (auto& c : persistentClusters) {
        if (pointsCount >= MAX_LANDMARKS || hands >= 2) break;
        if (c.id == main.id) continue;

        float dx   = c.x - main.x, dy = c.y - main.y;
        float dist = sqrtf(dx * dx + dy * dy);

        if (dist > 0.07f && dist < handRadiusMax) {
            pointsX[pointsCount] = c.x;
            pointsY[pointsCount] = c.y;
            pointsCount++;
            hands++;
        }
    }

    // Actualizar target de movimiento de cámara
    targetX = main.x;
    targetY = main.y - 0.03f;  // Ligero offset hacia arriba para encuadrar cara
    lostFrames = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  FÍSICA DE MOVIMIENTO DE CÁMARA
// ─────────────────────────────────────────────────────────────────────────────
void AutoTrackPro::StepMotionPhysics(float inertia)
{
    float baseInertia = std::max(0.72f, inertia);
    float accel       = (1.0f - baseInertia) * 0.12f;

    velX += (targetX - currentX) * accel;
    velY += (targetY - currentY) * accel;

    // Fricción cinematográfica
    velX *= 0.72f;
    velY *= 0.72f;

    // Velocidad máxima limitada para evitar saltos
    const float maxSpeed = 0.035f;
    velX = ClampF(velX, -maxSpeed, maxSpeed);
    velY = ClampF(velY, -maxSpeed, maxSpeed);

    currentX += velX;
    currentY += velY;

    // Zoom suavizado independiente
    currentZoom = currentZoom * 0.97f + targetZoom * 0.03f;

    activeFocusX = currentX;
    activeFocusY = currentY;
}

// ─────────────────────────────────────────────────────────────────────────────
//  RENDER
// ─────────────────────────────────────────────────────────────────────────────
FFResult AutoTrackPro::Render( ProcessOpenGLStruct* pGL )
{
    if (!pGL || pGL->numInputTextures < 1) return FF_FAIL;

    GLuint inputTex    = pGL->inputTextures[0]->Handle;
    float  zoomVal     = GetFloatParameter(P_ZOOM);
    float  inertia     = GetFloatParameter(P_SMOOTHNESS);
    float  sensitivity = GetFloatParameter(P_SENSITIVITY);
    float  xOff        = GetFloatParameter(P_X_OFFSET);
    float  yOff        = GetFloatParameter(P_Y_OFFSET);
    float  skinTone    = GetFloatParameter(P_SKIN_TONE);
    float  hudOpacity  = GetFloatParameter(P_HUD_OPACITY);
    bool   autoOn      = (GetFloatParameter(P_AUTOTRACK)  > 0.5f);
    bool   isMirror    = (GetFloatParameter(P_MIRROR)     > 0.5f);
    bool   showHUD     = (GetFloatParameter(P_SHOW_HUD)   > 0.5f);
    bool   autoZoom    = (GetFloatParameter(P_AUTOZOOM)   > 0.5f);
    bool   lockOn      = (GetFloatParameter(P_LOCK_ON)    > 0.5f);

    // ── PASO 1: Generar mapa de movimiento/piel ──────────────────────────────
    if (autoOn || showHUD) {
        glViewport(0, 0, MOTION_BUF_SIZE, MOTION_BUF_SIZE);

        {
            ScopedFBOBinding    fboBind   (motionFBO.GetGLID(), ScopedFBOBinding::RB_REVERT);
            ScopedShaderBinding shaderBind(trackShader.GetGLID());

            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, inputTex);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, prevFrameFBO.GetTextureInfo().Handle);

            trackShader.Set("currentFrame", 0);
            trackShader.Set("prevFrame",    1);
            trackShader.Set("sensitivity",  sensitivity * 28.0f);
            trackShader.Set("skinTone",     skinTone);
            quad.Draw();
        }

        // ── PASO 2: Leer píxeles y actualizar clusters ───────────────────────
        {
            const int N = MOTION_BUF_SIZE * MOTION_BUF_SIZE;
            std::vector<unsigned char> pixels(N * 4);
            glBindTexture(GL_TEXTURE_2D, motionFBO.GetTextureInfo().Handle);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            UpdateClusters(pixels);
        }

        // ── PASO 3: Clasificar landmarks ─────────────────────────────────────
        if (!persistentClusters.empty()) {
            ClassifyLandmarks(lockOn, zoomVal, autoZoom);
        } else {
            lostFrames++;
            if (lostFrames > 18) {
                if (!lockOn) lockedClusterId = -1;
                targetX    = currentX;
                targetY    = currentY;
                targetZoom = std::max(1.0f, currentZoom * 0.985f);
            }
        }

        // ── PASO 4: Física de movimiento ─────────────────────────────────────
        StepMotionPhysics(inertia);

        // ── PASO 5: Copiar frame actual a prevFrame ───────────────────────────
        glViewport(0, 0, MOTION_BUF_SIZE, MOTION_BUF_SIZE);
        {
            ScopedFBOBinding    fboBind   (prevFrameFBO.GetGLID(), ScopedFBOBinding::RB_REVERT);
            ScopedShaderBinding shaderBind(copyShader.GetGLID());
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, inputTex);
            copyShader.Set("inputTex", 0);
            quad.Draw();
        }
    }

    // ── PASO 6: Clamp de seguridad de cuadro ────────────────────────────────
    float hS     = 0.5f / std::max(1.001f, currentZoom);
    float margin = 0.04f;
    currentX = ClampF(currentX, hS + margin, 1.0f - hS - margin);
    currentY = ClampF(currentY, hS + margin, 1.0f - hS - margin);

    // ── PASO 7: Render final con zoom + HUD ─────────────────────────────────
    if (pGL->HostFBO) glBindFramebuffer(GL_FRAMEBUFFER, pGL->HostFBO);
    glViewport(0, 0, currentViewport.width, currentViewport.height);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    {
        ScopedShaderBinding shaderBind(finalShader.GetGLID());
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, inputTex);

        finalShader.Set("inputTexture", 0);
        finalShader.Set("zoom",         currentZoom);
        finalShader.Set("center",       currentX, currentY);
        finalShader.Set("xOffset",      xOff);
        finalShader.Set("yOffset",      yOff);
        finalShader.Set("showTrack",    showHUD);
        finalShader.Set("mirror",       isMirror);
        finalShader.Set("isLocked",     lockOn && (lockedClusterId != -1));
        finalShader.Set("hudOpacity",   hudOpacity);
        finalShader.Set("pointsCount",  pointsCount);

        for (int i = 0; i < pointsCount; i++) {
            char name[20];
            sprintf(name, "points[%d]", i);
            finalShader.Set(name, pointsX[i], pointsY[i]);
        }

        quad.Draw();
    }

    return FF_SUCCESS;
}
