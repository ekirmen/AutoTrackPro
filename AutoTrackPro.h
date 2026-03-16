#pragma once
#include "../../lib/ffglquickstart/FFGLEffect.h"
#include "../../lib/ffglex/FFGLShader.h"
#include "../../lib/ffglex/FFGLFBO.h"
#include <vector>

struct MotionCluster
{
    float x;
    float y;
    float weight;
    float life; // Vida para persistencia
    int   id;   // Para rastrear el mismo punto
};

class AutoTrackPro : public ffglqs::Effect
{
public:
	AutoTrackPro();
	~AutoTrackPro();

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult DeInitGL() override;
	FFResult Render( ProcessOpenGLStruct* pGL ) override;

private:
	// Shaders y Buffers
	ffglex::FFGLShader finalShader;
    ffglex::FFGLShader trackShader;
    ffglex::FFGLShader copyShader;
	ffglex::FFGLFBO    prevFrameFBO;  // Buffer para comparar frames
    ffglex::FFGLFBO    motionFBO;     // Buffer donde se guarda el mapa de calor del movimiento
    
	// Tracking State
	float currentX = 0.5f;
	float currentY = 0.5f;
	float targetX  = 0.5f;
	float targetY  = 0.5f;
    float activeFocusX = 0.5f;
    float activeFocusY = 0.5f;
    float velX = 0.0f;
    float velY = 0.0f;
    float sensorRadius = 0.25f;
    float currentZoom = 1.0f;
    int   lostFrames = 0;

    // AR Landmarks
    float pointsX[12];
    float pointsY[12];
    int   pointsCount = 0;
    std::vector<MotionCluster> persistentClusters;
    int clusterIdCounter = 0;
    int lockedClusterId = -1;
};
