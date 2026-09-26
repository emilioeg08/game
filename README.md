# GalaxyEngine

Motor propio en C++20 para un sandbox 4X de simulación galáctica persistente: la galaxia existe, cambia y
produce consecuencias aunque el jugador no la esté mirando.

**Estado: M2.1 (Sistema estelar jugable).** Hay un primer juego en tiempo real (×1, ×3, ×10): un sistema
estelar generado con órbitas keplerianas, tu nave (autopiloto con salto al hiperespacio fuera de los pozos
gravitatorios, o empuje newtoniano manual), cargueros NPC que viajan solos entre puertos, guardado rápido e
inspector de entidades. Los sensores pasivos y activos (radar R, transpondedor T) crean niebla de guerra: solo
ves tu flota y lo que detectas. Por debajo están el kernel de M1 (entidades,
eventos, comandos, save/load, replay, LOD por frecuencia) y la fase 0. El diseño es provisional
([docs/DESIGN.md](docs/DESIGN.md)) hasta que exista `game.md`. El nombre y el namespace (`gx`) también son
provisionales.

## Requisitos (Windows)

- Visual Studio 2022 o posterior con la carga "Desarrollo para el escritorio con C++" (verificado con
  VS 2026, MSVC 19.51).
- Python 3 para instalar las herramientas de build fijadas:

```powershell
python -m pip install -r tools/requirements-build.txt
```

> Con Smart App Control activado, Windows puede bloquear de forma intermitente alguno de los ejecutables
> recién compilados (docs/ARCHITECTURE.md, riesgo 2).

## Compilar, probar y medir

```powershell
./scripts/build.ps1                          # release
./scripts/build.ps1 -Preset debug -Test      # debug + tests
./scripts/build.ps1 -Test -Bench             # release + tests + benchmarks
```

Presets: `debug` (sin optimizar, con asserts), `release` (optimizado, para benchmarks) y `profile`
(optimizado con asserts e información de depuración). Los binarios quedan en `build/<preset>/bin/`.

## Jugar

```powershell
./build/release/bin/gx_game.exe                 # sistema de la semilla 2400
./build/release/bin/gx_game.exe --seed 77       # otro sistema
```

Controles: rueda = zoom · arrastrar = mover · clic = seleccionar · **clic derecho = ir allí** ·
WASD = empuje manual · X = frenar · Espacio = pausa · 1/2/3 = ×1 (tiempo real)/×3/×10 · H/F = seguir ·
F5/F9 = guardar/cargar · F3 = depuración · F1 = ayuda. La primera configuración descarga SDL3 y Dear ImGui
(versiones fijadas); `-DGX_BUILD_CLIENT=OFF` compila sin el cliente.

Modo captura para comprobaciones automáticas:
`gx_game.exe --frames 60 --prerun-hours 30 --select --screenshot captura.png`.

## Ejecutar la simulación headless

```powershell
./build/release/bin/gx_headless.exe --systems 1000 --days 30
./build/release/bin/gx_headless.exe --days 2 --speed 3600               # 1 h simulada por segundo real
./build/release/bin/gx_headless.exe --days 5 --trace run.trace.json      # abrir en https://ui.perfetto.dev

# Guardar, continuar e inspeccionar una partida
./build/release/bin/gx_headless.exe --days 8 --save day8.gxsave
./build/release/bin/gx_headless.exe --days 12 --load day8.gxsave
./build/release/bin/gx_headless.exe --inspect day8.gxsave

# Grabar la entrada externa (raids diarios) y verificar que el replay da el mismo estado final
./build/release/bin/gx_headless.exe --days 20 --raids --record run.gxreplay
./build/release/bin/gx_headless.exe --replay run.gxreplay --threads 3

./build/release/bin/gx_headless.exe --help
```

## Estructura

```text
Engine/        Core, Memory, Math, Jobs, Time, Profiling, Serialization
Simulation/    Kernel (scheduler, paso, save/load), World (entidades), Events, Commands
Space/         Orbits (Kepler), Bodies, Ships (vuelo y autopiloto), Generation (sistemas estelares)
Game/          Sandbox (escenario jugable y contenido provisional), Presentation (SystemSnapshot)
Scenarios/     cargas sintéticas para tests y benchmarks (no contenido de juego)
Apps/          gx_game (cliente SDL3 + ImGui), gx_headless
ThirdParty/    SDL3 y Dear ImGui (FetchContent con hash)
Benchmarks/    gx_bench
Tests/         gx_tests (15 suites en CTest)
docs/          DESIGN.md, ARCHITECTURE.md, DECISIONS.md, BENCHMARKS.md
cmake/ scripts/ tools/
```

## Documentación

- [docs/DESIGN.md](docs/DESIGN.md): diseño de juego provisional (escala, generación, vuelo, naves, NPC,
  controles).
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): capas, tiempo, pipeline, presentación, threading,
  entidades, eventos, comandos, persistencia, determinismo, LOD, referencias, contradicciones y riesgos.
- [docs/DECISIONS.md](docs/DECISIONS.md): registro de decisiones (ADR-001 a ADR-020).
- [docs/BENCHMARKS.md](docs/BENCHMARKS.md): método y resultados medidos por hito.
