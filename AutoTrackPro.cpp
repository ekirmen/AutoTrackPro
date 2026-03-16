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
#include <algorithm>
#include <cmath>

using namespace ffglex;
using namespace ffglqs;

inline float smoothstep(float edge0, float edge1, float x) {
    float t = (x - edge0) / (edge1 - edge0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static const char* VERTEX_SRC = R"(
#version 410 core
layout(location = 0) in vec2 aPos;
out vec2 i_uv;
void main() { 
    i_uv = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0); 
})";

static const char* COPY_FS = R"(
#version 410 core
uniform sampler2D inputTex;
in vec2 i_uv;
out vec4 fragColor;
void main() { fragColor = texture(inputTex, i_uv); }
)";
static const char* TRACK_FS = R"(
#version 410 core
uniform sampler2D currentFrame;
uniform sampler2D prevFrame;
uniform float sensitivity;
in vec2 i_uv;
out vec4 fragColor;

void main() {
    vec4 col = texture(currentFrame, i_uv);
    vec3 pCol = texture(prevFrame, i_uv).rgb;
    float r = col.r; float g = col.g; float b = col.b;
    
    // FILTRO DE PIEL MEJORADO (Más permisivo para bordes de manos)
    float isSkin = step(0.08, r - g) * step(-0.05, g - b) * step(0.25, r);
    
    float diff = length(col.rgb - pCol);
    vec2 texel = vec2(1.0/64.0, 1.0/64.0);
    float gx = texture(currentFrame, i_uv + vec2(texel.x,0)).r - texture(currentFrame, i_uv - vec2(texel.x,0)).r;
    float gy = texture(currentFrame, i_uv + vec2(0,texel.y)).r - texture(currentFrame, i_uv - vec2(0,texel.y)).r;
    
    float flow = length(vec2(gx,gy)) * diff;
    float mask = smoothstep(0.06, 0.35, flow * sensitivity) * (isSkin * 0.85 + 0.15);
    
    fragColor = vec4(i_uv.x, i_uv.y, mask, 1.0);
}
)";

static const char* FINAL_FS = R"(
#version 410 core
uniform sampler2D inputTexture;
uniform float zoom;
uniform vec2 center;
uniform float xOffset;
uniform float yOffset;
uniform bool showTrack;
uniform bool mirror;
uniform vec2 points[12];
uniform int pointsCount;
uniform bool isLocked;
in vec2 i_uv;
out vec4 fragColor;

float drawPoint(vec2 p, vec2 uv, float size) {
    float d = length(uv - p);
    return smoothstep(size, size * 0.5, d);
}

float drawLine(vec2 a, vec2 b, vec2 uv, float thickness) {
    vec2 pa = uv - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return smoothstep(thickness, 0.0, length(pa - ba * h));
}

void main() {
    vec2 uv_eff = i_uv;
    if (mirror) uv_eff.x = 1.0 - uv_eff.x;
    vec2 halfSize = vec2(0.5) / zoom;
    vec2 finalCenter = vec2(center.x + xOffset, center.y + yOffset);
    vec2 uv = mix(finalCenter - halfSize, finalCenter + halfSize, uv_eff);
    vec4 color = texture(inputTexture, uv);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) color *= 0.1;

    if (showTrack && pointsCount > 0) {
        float dots = 0.0;
        float lines = 0.0;
        // ESQUELETO PROPORCIONAL
        vec3 cyan = isLocked ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 1.0);
        vec2 neck = points[0] - vec2(0.0, 0.08); // Punto cuello simulado
        
        // Cara
        dots += drawPoint(points[0], i_uv, 0.006);
        dots += drawPoint(points[1], i_uv, 0.004);
        dots += drawPoint(points[2], i_uv, 0.004);
        lines += drawLine(points[0], neck, i_uv, 0.0012) * 0.6; // Cuello

        for(int i=3; i<pointsCount; i++) {
            dots += drawPoint(points[i], i_uv, 0.008);
            dots += drawPoint(points[i], i_uv, 0.02) * 0.4;
            // Estiramiento Mano-Cara Proporcional a través del cuello
            lines += drawLine(neck, points[i], i_uv, 0.0015) * 0.5; 
        }
        
        vec3 hud = (dots + lines) * cyan;
        fragColor = vec4(mix(color.rgb, hud, clamp(dots + lines, 0.0, 0.85)), 1.0);
    } else {
        fragColor = color;
    }
}
)";

AutoTrackPro::AutoTrackPro()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	AddParam( ParamRange::Create( "Zoom", 2.0f, { 1.0f, 10.0f } ) );
	AddParam( ParamRange::Create( "Smoothness", 0.65f, { 0.1f, 0.99f } ) );
    AddParam( ParamRange::Create( "Sensitivity", 0.5f, { 0.0f, 1.0f } ) );
    AddParam( ParamRange::Create( "X Offset", 0.0f, { -0.5f, 0.5f } ) ); 
    AddParam( ParamRange::Create( "Y Offset", 0.0f, { -0.5f, 0.5f } ) ); 

    AddParam( ParamBool::Create( "AutoTrack", true ) );
    AddParam( ParamBool::Create( "Mirror View", false ) );
    AddParam( ParamBool::Create( "Show Debug", false ) );
    AddParam( ParamBool::Create( "AutoZoom", false ) );
    AddParam( ParamBool::Create( "Lock On", false ) );
    AddParam( ParamTrigger::Create( "Creado by: @thex_led" ) );
}

AutoTrackPro::~AutoTrackPro() {}

FFResult AutoTrackPro::InitGL( const FFGLViewportStruct* vp )
{
    currentViewport = *vp;
    finalShader.Compile(VERTEX_SRC, FINAL_FS);
    trackShader.Compile(VERTEX_SRC, TRACK_FS);
    copyShader.Compile(VERTEX_SRC, COPY_FS);
    quad.Initialise();

    prevFrameFBO.Initialise(64, 64, GL_RGBA8);
    motionFBO.Initialise(64, 64, GL_RGBA8);

    return FF_SUCCESS;
}

FFResult AutoTrackPro::DeInitGL()
{
    finalShader.FreeGLResources();
    trackShader.FreeGLResources();
    copyShader.FreeGLResources();
    quad.Release();
    prevFrameFBO.Release();
    motionFBO.Release();
    return FF_SUCCESS;
}

FFResult AutoTrackPro::Render( ProcessOpenGLStruct* pGL )
{
    if (!pGL || pGL->numInputTextures < 1) return FF_FAIL;
    
    GLuint inputTex = pGL->inputTextures[0]->Handle;
    float zoomVal   = GetFloatParameter(0);
    float inertia   = GetFloatParameter(1);
    float sensitivity = GetFloatParameter(2);
    float xOff      = GetFloatParameter(3);
    float yOff      = GetFloatParameter(4);
    bool  autoOn    = (GetFloatParameter(5) > 0.5f);
    bool  isMirror  = (GetFloatParameter(6) > 0.5f);
    bool  showT     = (GetFloatParameter(7) > 0.5f);
    bool  autoZoom  = (GetFloatParameter(8) > 0.5f);
    bool  lockOn    = (GetFloatParameter(9) > 0.5f);

    if (autoOn || showT) {
        glViewport(0, 0, 64, 64);
        {
            ScopedFBOBinding fboBind(motionFBO.GetGLID(), ScopedFBOBinding::RB_REVERT);
            ScopedShaderBinding shaderBind(trackShader.GetGLID());
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, inputTex);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, prevFrameFBO.GetTextureInfo().Handle);
            trackShader.Set("currentFrame", 0);
            trackShader.Set("prevFrame", 1);
            
            float adaptiveSensitivity = sensitivity * (0.8f + sensorRadius * 0.6f);
            trackShader.Set("sensitivity", adaptiveSensitivity * 30.0f);
            quad.Draw();
        }

        // --- MOTOR BIOMÉTRICO (Persistencia + Hands + Face + AutoZoom) ---
        {
            const int size = 64;
            const int pixelsCount = size * size;
            static std::vector<float> motionHistory(pixelsCount, 0.0f);

            std::vector<unsigned char> pixels(pixelsCount * 4);
            glBindTexture(GL_TEXTURE_2D, motionFBO.GetTextureInfo().Handle);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

            // 1. Detección de clusters en el frame actual
            std::vector<MotionCluster> currentFrameClusters;
            for (int i = 0; i < pixelsCount; ++i) {
                float rawMotion = pixels[i * 4 + 2] / 255.0f;
                motionHistory[i] = motionHistory[i] * 0.7f + rawMotion * 0.3f;
                float m = motionHistory[i];
                if (m > 0.30f) {
                    float px = pixels[i * 4 + 0] / 255.0f;
                    float py = pixels[i * 4 + 1] / 255.0f;
                    bool added = false;
                    for (auto& c : currentFrameClusters) {
                        float dx = c.x - px; float dy = c.y - py;
                        if (dx * dx + dy * dy < 0.015f) { // Umbral de clustering
                            c.x = (c.x * c.weight + px * m) / (c.weight + m);
                            c.y = (c.y * c.weight + py * m) / (c.weight + m);
                            c.weight += m; added = true; break;
                        }
                    }
                    if (!added) currentFrameClusters.push_back({ px, py, m, 1.0f, 0 });
                }
            }

            // 2. Sistema de Persistencia (Anti-parpadeo)
            for(auto& pc : persistentClusters) pc.life -= 0.15f; 
            for(auto& cf : currentFrameClusters) {
                bool matched = false;
                for(auto& pc : persistentClusters) {
                    float dx = pc.x - cf.x; float dy = pc.y - cf.y;
                    if (dx*dx + dy*dy < 0.03f) {
                        pc.x = pc.x * 0.4f + cf.x * 0.6f;
                        pc.y = pc.y * 0.4f + cf.y * 0.6f;
                        pc.weight = cf.weight; pc.life = 1.0f; 
                        matched = true; break;
                    }
                }
                if (!matched && persistentClusters.size() < 12) {
                    cf.id = clusterIdCounter++;
                    cf.life = 0.5f; persistentClusters.push_back(cf);
                }
            }
            persistentClusters.erase(
                std::remove_if(persistentClusters.begin(), persistentClusters.end(), 
                               [](const MotionCluster& c){ return c.life <= 0.0f || c.weight < 1.0f; }),
                persistentClusters.end());

            // 3. Selección y Clasificación Biométrica (Con Lock On "Hard")
            pointsCount = 0;
            if (!persistentClusters.empty()) {
                MotionCluster* target = nullptr;
                // Si está bloqueado, buscamos exclusivamente a esa persona
                if (lockOn && lockedClusterId != -1) {
                    for(auto& pc : persistentClusters) {
                        if(pc.id == lockedClusterId) { target = &pc; break; }
                    }
                }

                // Si no hay bloqueo activo, buscamos al sujeto más prominente
                if(!target && !lockOn) {
                    std::sort(persistentClusters.begin(), persistentClusters.end(), 
                        [](const MotionCluster& a, const MotionCluster& b){ return a.weight > b.weight; });
                    target = &persistentClusters[0];
                    lockedClusterId = target->id;
                }

                if (target) {
                    auto& main = *target;
                    
                    // CARA
                    pointsX[0] = main.x; pointsY[0] = main.y;
                    pointsX[1] = main.x - 0.025f; pointsY[1] = main.y + 0.04f;
                    pointsX[2] = main.x + 0.025f; pointsY[2] = main.y + 0.04f;
                    pointsCount = 3;

                    // AUTO-ZOOM vs MANUAL SNAPPY ZOOM
                    if (autoZoom) {
                        float targetZ = 1.25f + (main.weight * 0.045f);
                        targetZ = std::max(1.1f, std::min(2.8f, targetZ));
                        currentZoom = currentZoom * 0.99f + targetZ * 0.01f;
                    } else {
                        // Manual mucho más rápido para que el usuario sienta el control (0.85 smooth)
                        currentZoom = currentZoom * 0.85f + zoomVal * 0.15f; 
                    }

                    // MANOS EXCLUSIVAS (Solo clusters cercanos a la persona bloqueada)
                    int hands = 0;
                    for (size_t i = 0; i < persistentClusters.size() && pointsCount < 12 && hands < 2; i++) {
                        auto& c = persistentClusters[i];
                        if (c.id == main.id) continue;
                        float dist = sqrtf(powf(c.x - main.x, 2) + powf(c.y - main.y, 2));
                        
                        // Si estamos en LOCK, solo aceptamos puntos en un radio cercano (su propio cuerpo)
                        if (lockOn && dist > 0.45f) continue;

                        if (dist > 0.08f) {
                            hands++;
                            pointsX[pointsCount] = c.x; pointsY[pointsCount] = c.y; pointsCount++;
                        }
                    }

                    targetX = main.x;
                    targetY = main.y - 0.04f;
                    lostFrames = 0;
                }
            } else {
                lostFrames++;
                if (lostFrames > 15) {
                    if(!lockOn) lockedClusterId = -1; // Reset lock solo si no está forzado
                    targetX = currentX; targetY = currentY;
                    currentZoom = currentZoom * 0.98f + 1.0f * 0.02f;
                }
            }
        }

        glViewport(0, 0, 64, 64);
        {
            ScopedFBOBinding fboBind(prevFrameFBO.GetGLID(), ScopedFBOBinding::RB_REVERT);
            ScopedShaderBinding shaderBind(copyShader.GetGLID());
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, inputTex);
            copyShader.Set("inputTex", 0);
            quad.Draw();
        }

        // --- MOTOR DE MOVIMIENTO PREDICTIVO ULTRA-SUAVE ---
        // Aumentamos la amortiguación (accel y fricción) para un look de cámara de cine
        float baseInertia = std::max(0.75f, inertia); // Aseguramos un mínimo de suavizado
        float accel = (1.0f - baseInertia) * 0.15f; // Aceleración más lenta
        velX += (targetX - currentX) * accel;
        velY += (targetY - currentY) * accel;
        
        // Fricción más alta (0.75) para evitar el rebote o brusquedad
        velX *= 0.75f;
        velY *= 0.75f;
        
        float maxSpeed = 0.04f; // Velocidad punta reducida a la mitad
        velX = std::max(-maxSpeed, std::min(maxSpeed, velX));
        velY = std::max(-maxSpeed, std::min(maxSpeed, velY));
        
        currentX += velX;
        currentY += velY;
        
        activeFocusX = currentX;
        activeFocusY = currentY;
    }

    // SEGURIDAD DE CUADRO
    float hS = 0.5f / std::max(1.001f, zoomVal);
    float margin = 0.05f; 
    auto clamp = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    currentX = clamp(currentX, hS - xOff - margin, (1.0f - hS) - xOff + margin);
    currentY = clamp(currentY, hS - yOff - margin, (1.0f - hS) - yOff + margin);
    currentX = clamp(currentX, 0.0f, 1.0f);
    currentY = clamp(currentY, 0.0f, 1.0f);

	if( pGL->HostFBO ) glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
    glViewport(0, 0, currentViewport.width, currentViewport.height);
    
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);

    ScopedShaderBinding shaderBind(finalShader.GetGLID());
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, inputTex);
    
    finalShader.Set("inputTexture", 0);
    finalShader.Set("zoom", currentZoom);
    finalShader.Set("center", currentX, currentY);
    finalShader.Set("xOffset", xOff);
    finalShader.Set("yOffset", yOff);
    finalShader.Set("showTrack", showT);
    finalShader.Set("mirror", isMirror);
    finalShader.Set("isLocked", lockOn && (lockedClusterId != -1));
    finalShader.Set("activeFocus", activeFocusX, activeFocusY);
    finalShader.Set("focusRadius", sensorRadius);
    
    // Pasar los puntos al shader (Loop manual para máxima compatibilidad)
    finalShader.Set("pointsCount", pointsCount);
    for(int i=0; i<pointsCount; i++) {
        char name[16];
        sprintf(name, "points[%d]", i);
        finalShader.Set(name, pointsX[i], pointsY[i]);
    }

    quad.Draw();

	return FF_SUCCESS;
}
