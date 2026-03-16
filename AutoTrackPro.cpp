// ─────────────────────────────────────────────────────────────────────────────
//  AutoTrackPro v4.0 – Auto Frame Engine
//  FFGL Plugin para Resolume Arena
//
//  Replica NVIDIA Broadcast Auto Frame:
//  · Skin detection + optical flow en GPU (shader GLSL)
//  · Blob tracking persistente con predicción de velocidad
//  · Zoom digital automático que encuadra al sujeto
//  · Seguimiento suave con SmoothDamp (igual que Unity/NVIDIA)
//  · Reencuadre cinematográfico: nunca corta la cabeza, mantiene headroom
//  · Parámetros: Zoom Pad, Smoothness, Sensitivity, Skin Tone, Lock On
//
//  by @thex_led
// ─────────────────────────────────────────────────────────────────────────────

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "AutoTrackPro.h"

#include "../../lib/ffglquickstart/FFGLParamBool.h"
#include "../../lib/ffglquickstart/FFGLParamRange.h"
#include "../../lib/ffglex/FFGLScopedFBOBinding.h"
#include "../../lib/ffglex/FFGLScopedShaderBinding.h"

using namespace ffglex;
using namespace ffglqs;

// ─────────────────────────────────────────────────────────────────────────────
//  GLSL: Vertex shader compartido
// ─────────────────────────────────────────────────────────────────────────────
static const char* VERT = R"GLSL(
#version 410 core
layout(location = 0) in vec2 aPos;
out vec2 uv;
void main() {
    uv          = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

// ─────────────────────────────────────────────────────────────────────────────
//  GLSL: Shader de copia simple (frame anterior)
// ─────────────────────────────────────────────────────────────────────────────
static const char* COPY_FS = R"GLSL(
#version 410 core
uniform sampler2D src;
in  vec2 uv;
out vec4 o;
void main() { o = texture(src, uv); }
)GLSL";

// ─────────────────────────────────────────────────────────────────────────────
//  GLSL: Shader de detección
//
//  Genera un "heat map" donde cada pixel contiene:
//    R = posición X del pixel (para calcular centroide)
//    G = posición Y del pixel
//    B = peso de detección (skin * motion)
//    A = 1.0
//
//  Lógica:
//  1. Convierte a YCbCr → detecta piel humana (rango Cb/Cr universal)
//  2. Calcula diferencia con frame anterior → detecta movimiento
//  3. Combina ambas señales → solo piel en movimiento pasa el filtro
//  4. Aplica gradiente temporal (acumulación) para reducir parpadeo
// ─────────────────────────────────────────────────────────────────────────────
static const char* DETECT_FS = R"GLSL(
#version 410 core
uniform sampler2D curFrame;
uniform sampler2D prvFrame;
uniform float     sensitivity;   // 0.0-1.0, controla umbral de movimiento
uniform float     skinTone;      // 0.0=piel clara, 1.0=piel oscura/morena

in  vec2 uv;
out vec4 o;

// Conversión RGB → YCbCr (estándar BT.601)
vec3 toYCbCr(vec3 c) {
    float Y  =  0.299*c.r + 0.587*c.g + 0.114*c.b;
    float Cb = -0.169*c.r - 0.331*c.g + 0.500*c.b + 0.5;
    float Cr =  0.500*c.r - 0.419*c.g - 0.081*c.b + 0.5;
    return vec3(Y, Cb, Cr);
}

// Máscara de piel: rango Cb/Cr calibrado para múltiples tonos de piel
// Ajustado con parámetro skinTone para mayor o menor rango de detección
float skinMask(vec3 ycbcr) {
    float Y  = ycbcr.x;
    float Cb = ycbcr.y;
    float Cr = ycbcr.z;

    // Rango base (piel clara/media)
    float cbLo = mix(0.370, 0.320, skinTone);
    float cbHi = mix(0.510, 0.560, skinTone);
    float crLo = mix(0.510, 0.470, skinTone);
    float crHi = mix(0.690, 0.730, skinTone);

    // Suavizado de bordes del rango
    float inCb = smoothstep(cbLo, cbLo+0.02, Cb) * (1.0 - smoothstep(cbHi-0.02, cbHi, Cb));
    float inCr = smoothstep(crLo, crLo+0.02, Cr) * (1.0 - smoothstep(crHi-0.02, crHi, Cr));

    // Mínimo de luminancia (ignorar negro puro)
    float lumOk = smoothstep(0.08, 0.18, Y);

    return inCb * inCr * lumOk;
}

void main() {
    vec4 cur = texture(curFrame, uv);
    vec4 prv = texture(prvFrame, uv);

    // Detección de piel
    vec3 ycbcr = toYCbCr(cur.rgb);
    float skin = skinMask(ycbcr);

    // Optical flow simplificado: diferencia temporal
    float diff = length(cur.rgb - prv.rgb);
    float motion = smoothstep(0.02, 0.15, diff * (0.3 + sensitivity * 1.4));

    // Combinar: piel con algo de movimiento, o mucho movimiento solo
    // (para detectar también ropa y cuerpo completo)
    float skinWeight   = skin * (0.4 + motion * 0.6);
    float motionWeight = motion * 0.35;
    float weight = max(skinWeight, motionWeight);

    // Umbral mínimo para reducir ruido de fondo
    weight = smoothstep(0.18, 0.40, weight);

    // Codificar posición en RG para cálculo de centroide en CPU
    o = vec4(uv.x, uv.y, weight, 1.0);
}
)GLSL";

// ─────────────────────────────────────────────────────────────────────────────
//  GLSL: Shader de render final (Auto Frame crop + zoom)
//
//  Aplica el crop/zoom calculado por la CPU:
//  · center = centro del encuadre (posición del sujeto)
//  · zoom   = nivel de zoom (1.0=sin zoom, 3.0=muy cerca)
//  · El resultado es como si la cámara se moviera y hiciera zoom
//  · Área fuera del frame original → negro (igual que NVIDIA)
// ─────────────────────────────────────────────────────────────────────────────
static const char* FRAME_FS = R"GLSL(
#version 410 core
uniform sampler2D inputTexture;
uniform vec2      center;    // Centro del encuadre [0,1]
uniform float     zoom;      // Nivel de zoom (>= 1.0)
uniform bool      mirror;    // Espejo horizontal

in  vec2 uv;
out vec4 o;

void main() {
    // UV del pixel en el espacio del frame original
    vec2 screenUV = mirror ? vec2(1.0 - uv.x, uv.y) : uv;

    // Transformar: desde el espacio de pantalla al espacio de la textura
    // con el zoom y centro del Auto Frame
    vec2 halfSize = vec2(0.5) / zoom;
    vec2 texUV    = center + (screenUV - 0.5) / zoom;

    // Área fuera del frame → negro (comportamiento idéntico a NVIDIA)
    if (texUV.x < 0.0 || texUV.x > 1.0 || texUV.y < 0.0 || texUV.y > 1.0) {
        o = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    o = texture(inputTexture, texUV);
}
)GLSL";

// ─────────────────────────────────────────────────────────────────────────────
//  Índices de parámetros
// ─────────────────────────────────────────────────────────────────────────────
enum Param {
    P_ZOOM_PAD    = 0,   // Espacio alrededor del sujeto (headroom)
    P_SMOOTHNESS  = 1,   // Suavidad del movimiento (0=rápido, 1=muy suave)
    P_SENSITIVITY = 2,   // Sensibilidad de detección de movimiento
    P_SKIN_TONE   = 3,   // Tono de piel (0=claro, 1=oscuro)
    P_MIN_ZOOM    = 4,   // Zoom mínimo (no se acerca más que esto)
    P_MAX_ZOOM    = 5,   // Zoom máximo (no se aleja más que esto)
    P_MIRROR      = 6,   // Espejo horizontal
    P_LOCK_ON     = 7,   // Bloquear en el sujeto principal
    P_ENABLED     = 8,   // Activar/desactivar Auto Frame
};

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor: registrar parámetros
// ─────────────────────────────────────────────────────────────────────────────
AutoTrackPro::AutoTrackPro()
{
    SetMinInputs(1);
    SetMaxInputs(1);

    // Parámetros principales (igual que NVIDIA Broadcast UI)
    AddParam( ParamRange::Create( "Zoom Pad",     0.30f, { 0.05f, 0.80f } ) );  // Headroom
    AddParam( ParamRange::Create( "Smoothness",   0.92f, { 0.50f, 0.99f } ) );  // Inercia
    AddParam( ParamRange::Create( "Sensitivity",  0.55f, { 0.10f, 1.00f } ) );  // Detección
    AddParam( ParamRange::Create( "Skin Tone",    0.30f, { 0.00f, 1.00f } ) );  // Tono piel
    AddParam( ParamRange::Create( "Min Zoom",     1.20f, { 1.00f, 2.00f } ) );  // Zoom mínimo
    AddParam( ParamRange::Create( "Max Zoom",     2.50f, { 1.50f, 5.00f } ) );  // Zoom máximo
    AddParam( ParamBool::Create ( "Mirror",       false ) );
    AddParam( ParamBool::Create ( "Lock On",      false ) );
    AddParam( ParamBool::Create ( "Auto Frame",   true  ) );
}

AutoTrackPro::~AutoTrackPro() {}

// ─────────────────────────────────────────────────────────────────────────────
//  SmoothDamp: igual al algoritmo de Unity/NVIDIA
//  Mueve 'cur' hacia 'tgt' con velocidad 'vel' y suavidad 'smooth'
//  smooth: 0.0=instantáneo, 0.99=muy suave (cinematográfico)
// ─────────────────────────────────────────────────────────────────────────────
float AutoTrackPro::SmoothDamp(float cur, float tgt, float& vel, float smooth) const
{
    // Convertir smooth [0,1] a tiempo de respuesta en frames
    // smooth=0.92 → ~12 frames para llegar al 95% del objetivo
    float omega = 2.0f / (1.0f - Clamp(smooth, 0.01f, 0.999f));
    float x     = omega;
    float exp_  = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    float delta = cur - tgt;
    float temp  = (vel + omega * delta) * 1.0f;
    vel         = (vel - omega * temp) * exp_;
    return tgt + (delta + temp) * exp_;
}

// ─────────────────────────────────────────────────────────────────────────────
//  InitGL
// ─────────────────────────────────────────────────────────────────────────────
FFResult AutoTrackPro::InitGL( const FFGLViewportStruct* vp )
{
    if (detectShader.Compile(VERT, DETECT_FS) != GL_TRUE) return FF_FAIL;
    if (frameShader .Compile(VERT, FRAME_FS)  != GL_TRUE) return FF_FAIL;
    if (copyShader  .Compile(VERT, COPY_FS)   != GL_TRUE) return FF_FAIL;

    if (!quad.Initialise()) return FF_FAIL;

    if (!prevFBO.Initialise(TRACK_BUF, TRACK_BUF, GL_RGBA8)) return FF_FAIL;
    if (!heatFBO.Initialise(TRACK_BUF, TRACK_BUF, GL_RGBA8)) return FF_FAIL;

    heatPixels.resize(TRACK_BUF * TRACK_BUF * 4, 0);
    motionAcc .resize(TRACK_BUF * TRACK_BUF,     0.0f);

    // Inicializar cámara centrada
    camX = camY = tgtX = tgtY = 0.5f;
    camZoom = tgtZoom = 1.5f;

    return CFFGLPlugin::InitGL(vp);
}

// ─────────────────────────────────────────────────────────────────────────────
//  DeInitGL
// ─────────────────────────────────────────────────────────────────────────────
FFResult AutoTrackPro::DeInitGL()
{
    detectShader.FreeGLResources();
    frameShader .FreeGLResources();
    copyShader  .FreeGLResources();
    quad.Release();
    prevFBO.Release();
    heatFBO.Release();
    return FF_SUCCESS;
}

// ─────────────────────────────────────────────────────────────────────────────
//  DetectBlobs: lee el heat map y extrae blobs (regiones de sujeto)
//
//  Algoritmo:
//  1. Lee pixels del heatFBO (TRACK_BUF x TRACK_BUF)
//  2. Acumula temporalmente para reducir parpadeo (IIR filter)
//  3. Agrupa pixels activos en blobs por proximidad
//  4. Actualiza blobs persistentes con predicción de velocidad
//  5. Elimina blobs muertos (sin detección por varios frames)
// ─────────────────────────────────────────────────────────────────────────────
void AutoTrackPro::DetectBlobs()
{
    const int N = TRACK_BUF * TRACK_BUF;

    // Leer heat map de GPU
    glBindTexture(GL_TEXTURE_2D, heatFBO.GetTextureInfo().Handle);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, heatPixels.data());

    // Acumulación temporal IIR (reduce parpadeo, igual que NVIDIA)
    // alpha=0.4 → responde rápido pero suaviza ruido
    const float alpha = 0.40f;
    for (int i = 0; i < N; ++i) {
        float w = heatPixels[i * 4 + 2] / 255.0f;
        motionAcc[i] = motionAcc[i] * (1.0f - alpha) + w * alpha;
    }

    // Extraer blobs del frame actual
    struct RawBlob { float sx, sy, mass; int count; };
    std::vector<RawBlob> rawBlobs;
    rawBlobs.reserve(MAX_BLOBS);

    for (int i = 0; i < N; ++i) {
        float w = motionAcc[i];
        if (w < 0.22f) continue;

        float px = heatPixels[i * 4 + 0] / 255.0f;
        float py = heatPixels[i * 4 + 1] / 255.0f;

        // Intentar fusionar con blob existente (radio de fusión = 0.10)
        bool merged = false;
        for (auto& rb : rawBlobs) {
            float dx = rb.sx / rb.mass - px;
            float dy = rb.sy / rb.mass - py;
            if (dx * dx + dy * dy < 0.010f) {
                rb.sx   += px * w;
                rb.sy   += py * w;
                rb.mass += w;
                rb.count++;
                merged = true;
                break;
            }
        }
        if (!merged && (int)rawBlobs.size() < MAX_BLOBS) {
            rawBlobs.push_back({ px * w, py * w, w, 1 });
        }
    }

    // Calcular bounding box aproximado de cada raw blob
    // (necesario para calcular el zoom automático)
    struct BlobBox { float cx, cy, w, h, mass; };
    std::vector<BlobBox> boxes;
    boxes.reserve(rawBlobs.size());

    for (auto& rb : rawBlobs) {
        if (rb.mass < 0.5f) continue;
        float cx = rb.sx / rb.mass;
        float cy = rb.sy / rb.mass;

        // Calcular dispersión (tamaño del blob)
        float varX = 0.0f, varY = 0.0f;
        int   cnt  = 0;
        for (int i = 0; i < N; ++i) {
            float w = motionAcc[i];
            if (w < 0.22f) continue;
            float px = heatPixels[i * 4 + 0] / 255.0f;
            float py = heatPixels[i * 4 + 1] / 255.0f;
            float dx = px - cx, dy = py - cy;
            if (dx * dx + dy * dy < 0.025f) {
                varX += dx * dx * w;
                varY += dy * dy * w;
                cnt++;
            }
        }
        float bw = (rb.mass > 0.0f) ? std::sqrt(varX / rb.mass) * 4.0f : 0.10f;
        float bh = (rb.mass > 0.0f) ? std::sqrt(varY / rb.mass) * 4.0f : 0.15f;
        bw = Clamp(bw, 0.05f, 0.90f);
        bh = Clamp(bh, 0.08f, 0.90f);

        boxes.push_back({ cx, cy, bw, bh, rb.mass });
    }

    // Actualizar blobs persistentes con los raw blobs del frame
    for (auto& pb : blobs) pb.life -= 0.08f;

    for (auto& box : boxes) {
        bool matched = false;
        for (auto& pb : blobs) {
            float dx = pb.cx - box.cx, dy = pb.cy - box.cy;
            if (dx * dx + dy * dy < 0.030f) {
                // Actualizar posición con predicción de velocidad
                float newCx = pb.cx * 0.30f + box.cx * 0.70f;
                float newCy = pb.cy * 0.30f + box.cy * 0.70f;
                pb.vx   = newCx - pb.cx;
                pb.vy   = newCy - pb.cy;
                pb.cx   = newCx;
                pb.cy   = newCy;
                pb.w    = pb.w  * 0.50f + box.w  * 0.50f;
                pb.h    = pb.h  * 0.50f + box.h  * 0.50f;
                pb.mass = box.mass;
                pb.life = 1.0f;
                matched = true;
                break;
            }
        }
        if (!matched && (int)blobs.size() < MAX_BLOBS) {
            Blob nb;
            nb.cx   = box.cx;
            nb.cy   = box.cy;
            nb.w    = box.w;
            nb.h    = box.h;
            nb.mass = box.mass;
            nb.life = 0.5f;
            nb.id   = blobIdCounter++;
            blobs.push_back(nb);
        }
    }

    // Eliminar blobs muertos o muy pequeños
    blobs.erase(
        std::remove_if(blobs.begin(), blobs.end(),
            [](const Blob& b){ return b.life <= 0.0f || b.mass < 0.3f; }),
        blobs.end());
}

// ─────────────────────────────────────────────────────────────────────────────
//  SelectTarget: elige el blob objetivo y calcula tgtX, tgtY, tgtZoom
//
//  Lógica igual a NVIDIA Auto Frame:
//  · Si hay Lock On: mantiene el sujeto bloqueado aunque otros aparezcan
//  · Si no hay Lock On: sigue al sujeto más grande/cercano al centro
//  · El zoom se calcula para que el sujeto ocupe ~(1-zoomPad) del frame
//  · Se añade headroom arriba (la cabeza nunca se corta)
//  · Si se pierde el sujeto: zoom out gradual hasta volver a verlo
// ─────────────────────────────────────────────────────────────────────────────
void AutoTrackPro::SelectTarget(bool lockOn, float zoomPad, bool autoZoom, float manualZoom)
{
    if (blobs.empty()) {
        lostFrames++;
        // Sin sujeto: zoom out gradual (igual que NVIDIA)
        if (lostFrames > 20) {
            tgtZoom = std::max(1.0f, tgtZoom * 0.985f);
            // Volver al centro suavemente
            tgtX = tgtX * 0.98f + 0.5f * 0.02f;
            tgtY = tgtY * 0.98f + 0.5f * 0.02f;
            if (lostFrames > 60) lockedBlobId = -1;
        }
        return;
    }

    lostFrames = 0;

    // Seleccionar blob objetivo
    Blob* target = nullptr;

    if (lockOn && lockedBlobId != -1) {
        for (auto& b : blobs)
            if (b.id == lockedBlobId) { target = &b; break; }
    }

    if (!target) {
        // Ordenar por masa (confianza) y proximidad al centro
        // Igual que NVIDIA: prefiere el sujeto más prominente y centrado
        std::sort(blobs.begin(), blobs.end(), [](const Blob& a, const Blob& b) {
            float da = (a.cx-0.5f)*(a.cx-0.5f) + (a.cy-0.5f)*(a.cy-0.5f);
            float db = (b.cx-0.5f)*(b.cx-0.5f) + (b.cy-0.5f)*(b.cy-0.5f);
            // Score: masa alta + distancia al centro baja
            float sa = a.mass * 2.0f - da * 3.0f;
            float sb = b.mass * 2.0f - db * 3.0f;
            return sa > sb;
        });
        target = &blobs[0];
        lockedBlobId = target->id;
    }

    // ── Calcular posición objetivo ────────────────────────────────────────────
    // NVIDIA añade headroom: el centro del encuadre está ligeramente
    // por debajo del centro del sujeto (para no cortar la cabeza)
    float headroom = 0.04f;  // 4% hacia arriba = espacio sobre la cabeza
    tgtX = target->cx;
    tgtY = Clamp(target->cy + headroom, 0.1f, 0.9f);

    // ── Calcular zoom objetivo ────────────────────────────────────────────────
    if (autoZoom) {
        // El zoom se calcula para que el sujeto ocupe (1-zoomPad) del frame
        // Si el sujeto es grande → zoom out; si es pequeño → zoom in
        float subjectSize = std::max(target->w, target->h * 0.75f);
        float desiredSize = 1.0f - zoomPad;  // Fracción del frame que debe ocupar

        // zoom = desiredSize / subjectSize
        float z = desiredSize / std::max(subjectSize, 0.05f);
        tgtZoom = Clamp(z, manualZoom * 0.8f, manualZoom * 1.8f);
    } else {
        tgtZoom = manualZoom;
    }

    // Guardar historia de posición para suavizado adicional
    histX.push_back(tgtX);
    histY.push_back(tgtY);
    histW.push_back(target->w);
    histH.push_back(target->h);
    if ((int)histX.size() > HIST_LEN) { histX.pop_front(); histY.pop_front(); histW.pop_front(); histH.pop_front(); }

    // Media móvil de la posición objetivo (reduce jitter)
    if (!histX.empty()) {
        float sx = 0, sy = 0;
        for (float v : histX) sx += v;
        for (float v : histY) sy += v;
        tgtX = sx / histX.size();
        tgtY = sy / histY.size();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  StepCamera: mueve la cámara virtual hacia el objetivo con SmoothDamp
//
//  Esto es el corazón del Auto Frame:
//  · SmoothDamp = movimiento suave con inercia (igual que Unity/NVIDIA)
//  · La cámara "persigue" al sujeto con amortiguación
//  · A mayor smoothness → más inercia → movimiento más cinematográfico
//  · Clamp de seguridad: el encuadre nunca sale del frame original
// ─────────────────────────────────────────────────────────────────────────────
void AutoTrackPro::StepCamera(float smoothness)
{
    // Mover cámara hacia objetivo con SmoothDamp
    camX    = SmoothDamp(camX,    tgtX,    camVX, smoothness);
    camY    = SmoothDamp(camY,    tgtY,    camVY, smoothness);
    camZoom = SmoothDamp(camZoom, tgtZoom, camVZ, smoothness * 0.85f);

    // Zoom mínimo absoluto
    camZoom = std::max(camZoom, 1.001f);

    // Clamp de seguridad: el encuadre no puede salir del frame
    // (igual que NVIDIA: no hay bordes negros a menos que el sujeto
    //  esté en el borde del frame físico)
    float halfW = 0.5f / camZoom;
    float halfH = 0.5f / camZoom;
    float margin = 0.01f;

    camX = Clamp(camX, halfW + margin, 1.0f - halfW - margin);
    camY = Clamp(camY, halfH + margin, 1.0f - halfH - margin);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Render: loop principal por frame
// ─────────────────────────────────────────────────────────────────────────────
FFResult AutoTrackPro::Render( ProcessOpenGLStruct* pGL )
{
    if (!pGL || pGL->numInputTextures < 1 || !pGL->inputTextures[0]) return FF_FAIL;

    GLuint inputTex  = pGL->inputTextures[0]->Handle;
    bool   enabled   = GetFloatParameter(P_ENABLED)     > 0.5f;
    float  smoothness= GetFloatParameter(P_SMOOTHNESS);
    float  sensitivity=GetFloatParameter(P_SENSITIVITY);
    float  skinTone  = GetFloatParameter(P_SKIN_TONE);
    float  zoomPad   = GetFloatParameter(P_ZOOM_PAD);
    float  minZoom   = GetFloatParameter(P_MIN_ZOOM);
    float  maxZoom   = GetFloatParameter(P_MAX_ZOOM);
    bool   mirror    = GetFloatParameter(P_MIRROR)      > 0.5f;
    bool   lockOn    = GetFloatParameter(P_LOCK_ON)     > 0.5f;

    // Clamp: maxZoom siempre >= minZoom
    maxZoom = std::max(maxZoom, minZoom + 0.1f);

    if (enabled) {
        // ── Paso 1: Generar heat map en GPU ──────────────────────────────────
        glViewport(0, 0, TRACK_BUF, TRACK_BUF);
        {
            ScopedFBOBinding    fbo  (heatFBO.GetGLID(), ScopedFBOBinding::RB_REVERT);
            ScopedShaderBinding shd  (detectShader.GetGLID());
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, inputTex);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, prevFBO.GetTextureInfo().Handle);
            detectShader.Set("curFrame",    0);
            detectShader.Set("prvFrame",    1);
            detectShader.Set("sensitivity", sensitivity);
            detectShader.Set("skinTone",    skinTone);
            quad.Draw();
        }

        // ── Paso 2: Detectar blobs (CPU) ─────────────────────────────────────
        DetectBlobs();

        // ── Paso 3: Seleccionar objetivo y calcular encuadre ─────────────────
        SelectTarget(lockOn, zoomPad, true, Lerp(minZoom, maxZoom, 0.5f));

        // ── Paso 4: Aplicar límites de zoom ──────────────────────────────────
        tgtZoom = Clamp(tgtZoom, minZoom, maxZoom);

        // ── Paso 5: Mover cámara virtual (SmoothDamp) ────────────────────────
        StepCamera(smoothness);

        // ── Paso 6: Copiar frame actual a prevFBO ────────────────────────────
        glViewport(0, 0, TRACK_BUF, TRACK_BUF);
        {
            ScopedFBOBinding    fbo  (prevFBO.GetGLID(), ScopedFBOBinding::RB_REVERT);
            ScopedShaderBinding shd  (copyShader.GetGLID());
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, inputTex);
            copyShader.Set("src", 0);
            quad.Draw();
        }
    } else {
        // Auto Frame desactivado: centrar y sin zoom
        tgtX = camX = 0.5f;
        tgtY = camY = 0.5f;
        tgtZoom = camZoom = 1.0f;
        camVX = camVY = camVZ = 0.0f;
    }

    // ── Paso 7: Render final con crop+zoom ───────────────────────────────────
    if (pGL->HostFBO) glBindFramebuffer(GL_FRAMEBUFFER, pGL->HostFBO);
    glViewport(0, 0, currentViewport.width, currentViewport.height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    {
        ScopedShaderBinding shd(frameShader.GetGLID());
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, inputTex);
        frameShader.Set("inputTexture", 0);
        frameShader.Set("center",       camX, camY);
        frameShader.Set("zoom",         camZoom);
        frameShader.Set("mirror",       mirror);
        quad.Draw();
    }

    return FF_SUCCESS;
}
