# OutRun 2006: Coast 2 Coast — PS Vita Port

Port nativo de **OutRun 2006: Coast 2 Coast** (versión PC / Steam) para **PlayStation Vita**, basado en el proyecto [`outrun2-decomp`](https://github.com/r4dius/outrun2-decomp).

Ejecuta el motor decompilado del juego de PC de forma nativa sobre **ARM Cortex-A9 + vitaGL (PowerVR SGX543MP4+)** a resolución nativa **960×544** y **30 FPS estables en carrera** (hasta 60 FPS en menús).

---

## Estado actual — Qué funciona

- **Resolución nativa de PS Vita (`960×544`, 16:9).**
- **30 FPS bloqueados en carrera** con 2 ticks de simulación de 60 Hz por frame (velocidad real 1×, sin cámara lenta) y **60 FPS opcionales en menús** (`Options > Settings > MENU FRAME RATE`).
- **Arquitectura multi-núcleo en PS Vita:**
  - **Core 0:** hilo dedicado de `vitaGL` / GPU (command buffer doble asíncrono).
  - **Core 1:** lógica del juego, física y recorrido de escena.
  - **Core 2:** audio, trabajos paralelos de culling y descompresión asíncrona de archivos `.sz` (evita congelamientos al cargar tramos o tráfico).
- **Modos jugables:** OutRun, Time Attack, Heart Attack (OutRun 2), Coast 2 Coast (carreras estándar / rivales), Showroom, Car Select, Rankings, Licencias y Opciones.
- **Audio y música:** efectos de sonido y música del juego original (soporte Vorbis `.ogg`).
- **Cámara de inicio con el Flagman**, guardado de partidas/licencias en `ux0:data/outrun2006/SaveGame/`, y configuración mediante `options.ini` (`sd_logs=0` por defecto para no escribir logs a la SD).

---

## Qué falta por completar

- **Modo Coast 2 Coast — Misiones de Clarissa (`variant 5` / Request Mode):**
  - Las 82 funciones de lógica bulk ya están compiladas en `vita/source/bulk_vita_tr.cpp`, pero faltan por conectar ~51 servicios externos en `src/platform/race_traffic_runtime.cpp` (14 de mates matriz/vector, 3 de CRT y ~34 getters/helpers de misiones y HUD). Actualmente entra a la carrera pero `race_traffic` se bloquea al llegar al primer servicio sin cablear (`0x48B140`).
- **Heart Attack en OutRun 2 SP:**
  - La carrera funciona, pero faltan por mostrar en pantalla las instrucciones de misión y el contador de corazones (en OutRun 2 normal sí aparecen).
- **Multijugador online / LAN:**
  - No implementado/probado en PS Vita.

---

## Limitaciones técnicas en PS Vita (qué no se puede hacer o no es viable)

- **60 FPS estables en carrera:** el cuello de botella está en CPU (ARM Cortex-A9 a 444 MHz ejecutando el código x86 traducido + física + recorrido de escena). A 30 FPS con 2 ticks de 60 Hz queda margen estable (~5–8 ms libres); intentar 60 FPS en carrera provoca caídas constantes.
- **Reflejos dinámicos en cubemap (6 caras por frame) y shaders DX9 originales:**
  - La versión de PC renderiza un cubemap de entorno de 6 caras cada frame (`0x414340`) y usa pixel/vertex shaders DirectX 9 (`vs_1_1` / `ps_1_4`).
  - En PS Vita se usa el pipeline fijo de `vitaGL` para mantener el rendimiento, y el cubemap de 6 caras está desactivado porque multiplicaría las pasadas de escena y hundiría los FPS.
  - Por la misma limitación de `vitaGL`, mezclar `glUseProgram` con el pipeline fijo corrompe los draws de la carretera, y activar `GL_LIGHTING` por coche penaliza el rendimiento en Core 0. Para añadir reflejos/especular reales en GPU haría falta migrar todo el render 3D a un único shader Cg/GLSL unificado.
- **Sombras volumétricas stencil en coches (`car_shadows`):** desactivadas en Vita por el alto coste de fillrate/draw calls en PowerVR SGX543 (se mantiene la sombra proyectada bajo el coche).
- **Partículas `D3DPT_POINTLIST`:** ignoradas en `vita_gl_d3d9.cpp` y tope de 64 partículas simultáneas para mantener estables los 30 FPS.

---

## Instalación en PS Vita

### Requisitos
1. PS Vita o PS TV con **HENkaku / Enso** y **VitaShell**.
2. Una copia original de **OutRun 2006: Coast 2 Coast** de PC (Steam/retail), incluyendo `OR2006C2C.EXE` y todas sus carpetas (`BK`, `Cars`, `Chr`, `Common`, `Driver`, `Media`, `OSO`, `Scripts`, `Sound`, `Sprani`, `Sprite`, `Stage`, `Text`, etc.).
3. ~1.5 GB libres en `ux0:`.
4. **Overclock recomendado** (ej. PSVshell a 444/500 MHz CPU, 222 MHz GPU) para mantener 30 FPS constantes en todos los circuitos.

### Pasos
1. Instala `OutRun2006.vpk` desde **VitaShell** (acepta los permisos extendidos).
2. Crea la carpeta `ux0:data/outrun2006/` y copia dentro **todo** el contenido del juego de PC de modo que exista:
   ```text
   ux0:data/outrun2006/OR2006C2C.EXE
   ```
   junto con todas las carpetas de datos.
3. Abre **OutRun 2006 C2C** desde el LiveArea. En el primer arranque generará `OR2006C2C.cache` y `options.ini` automáticamente.

---

## Configuración (`ux0:data/outrun2006/options.ini`)

Ejemplo de opciones disponibles:

```ini
# OutRun 2006 PS Vita options v2 (edit or change in Options > Settings)
widescreen=1
framerate=60
lod_culling=6
lod_bias=1
sd_logs=0
```

- `framerate`: `60` o `30` — solo aplica a los **menús** (también configurable en `Options > Settings > MENU FRAME RATE`); las carreras siempre van bloqueadas a 30 FPS.
- `lod_culling`: distancia de recorte por tamaño proyectado (`7` = PC original, `6` = por defecto, `5` o `4` = recorta más objetos lejanos).
- `lod_bias`: nivel de detalle geométrico (`0` = PC original, `1` = por defecto, `2..3` = mallas más simples antes).
- `sd_logs`: `0` = apagado (por defecto, cero escritura de logs en la SD); `1` = activa los logs de telemetría/profiler (`profile_*.log`, `render_bottleneck_*.log`) para depuración.

---

## Controles en PS Vita

| Botón PS Vita | Acción |
| :--- | :--- |
| **Stick Izquierdo / Cruceta** | Dirección |
| **Gatillo R** | Acelerar / Subir marcha |
| **Gatillo L** | Frenar / Derrapar / Bajar marcha |
| **Cruz (X)** | Confirmar / Seleccionar |
| **Círculo (O)** | Atrás / Cancelar |
| **Triángulo** | Cambiar cámara |
| **Start** | Pausa / Confirmar |
| **Select** | Mostrar/Ocultar overlay de estadísticas |

---

## Cómo compilar desde código fuente (Linux / WSL)

### Requisitos de compilación
- **VitaSDK** instalado (`$VITASDK` configurado en el entorno).
- **CMake >= 3.16**, `make` y compilador `arm-vita-eabi-g++`.
- Librerías de VitaSDK: `SDL2`, `libvorbis` / `libogg`, `vitaGL`, `vitashark`, `mathneon`, `zlib`, `pthread`.

### Compilar el `.vpk` y `eboot.bin`
```bash
export VITASDK=/ruta/a/tu/vitasdk
export PATH=$VITASDK/bin:$PATH

cmake -S vita -B vita/build
cmake --build vita/build -j$(nproc)
```

Los artefactos generados estarán en:
- `vita/build/OutRun2006.vpk` (instalador completo para VitaShell)
- `vita/build/eboot.bin` (ejecutable para actualizar instalaciones existentes en `ux0:app/OR2006C2C/eboot.bin`)

*(Opcional: añade `-DOR2_VITA_HEAVY_PROFILE=ON` al comando `cmake` si deseas habilitar el conteo detallado de polígonos por modelo junto con `sd_logs=1`.)*

---

## Créditos y avisos legales

- Basado en [`outrun2-decomp`](https://github.com/r4dius/outrun2-decomp) por **r4dius**.
- Compilado con **VitaSDK** y **vitaGL**.
- Este repositorio **no incluye** archivos, ejecutables ni assets con copyright de *OutRun 2006: Coast 2 Coast* (SEGA / Sumo Digital). Debes aportar tus propios archivos del juego original de PC.
- Ver [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) para licencias de terceros.
