# AutoTrackPro

**Advanced Biometric AR Tracking Plugin for Resolume Arena**  
`AutoTrackPro.dll` · FFGL Effect · v2.8 · by [@thex_led](https://github.com/ekirmen)

---

## ¿Qué es?

AutoTrackPro es un plugin FFGL de una sola DLL para Resolume Arena que implementa tracking biométrico AR en tiempo real. Desarrollado como alternativa de alto rendimiento a MediaPipe, combina detección de piel por GPU, clustering de movimiento persistente y zoom cinematográfico para crear una experiencia visual de cámara inteligente sin ningún lag.

## Instalación

1. Copia `build/AutoTrackPro.dll` a tu carpeta de plugins de Resolume:
   ```
   C:\Users\<usuario>\Documents\Resolume Arena\Extra Effects\
   ```
2. Reinicia Resolume Arena.
3. Busca **AutoTrackPro** en el panel de efectos y arrástralo a cualquier capa o clip.

> **Nota:** `onnxruntime.dll` no es necesario para esta versión. Solo se necesita `AutoTrackPro.dll`.

## Parámetros

| Parámetro | Rango | Descripción |
| :--- | :--- | :--- |
| **Zoom** | 1.0 – 10.0 | Nivel de zoom del encuadre |
| **Smoothness** | 0.1 – 0.99 | Suavidad del movimiento de cámara |
| **Sensitivity** | 0.0 – 1.0 | Sensibilidad del detector de movimiento/piel |
| **X Offset** | -0.5 – 0.5 | Desplazamiento horizontal del centro |
| **Y Offset** | -0.5 – 0.5 | Desplazamiento vertical del centro |
| **AutoTrack** | On/Off | Activa el tracking automático |
| **Mirror View** | On/Off | Espeja la imagen horizontalmente |
| **Show Debug** | On/Off | Muestra el HUD AR con puntos y líneas de neón |
| **AutoZoom** | On/Off | Zoom automático basado en el peso del cluster |
| **Lock On** | On/Off | Hard Lock: fija el tracking al artista principal |

## Capacidades

- **Detección Skin-First:** filtra fondo, luces y público. Solo detecta piel humana.
- **Estética AR profesional:** HUD con puntos y líneas de neón (cian en modo normal, rojo en Hard Lock).
- **Smart Framing:** movimiento de cámara suave con física de velocidad, como un operador humano.
- **Hard Lock:** modo de bloqueo para fijar el tracking a un artista específico.
- **Clustering persistente:** agrupa puntos de movimiento con ID y vida propia para tracking estable.
- **100% GPU:** procesamiento en shaders GLSL, 60 FPS garantizados sin afectar la CPU.

## Compilación

El proyecto requiere el SDK de FFGL (FreeFrameGL) y un compilador MSVC (Visual Studio).

```
Estructura esperada:
/
├── AutoTrackPro.cpp
├── AutoTrackPro.h
├── main.cpp
├── build/
│   └── AutoTrackPro.dll   ← único archivo necesario para Resolume
└── ../../lib/             ← SDK FFGL (ffglquickstart, ffglex, FFGLSDK.h)
```

## Licencia

Proyecto privado. Todos los derechos reservados © 2026 @thex_led
