# GalaxyEngine

Motor propio en C++20 para un sandbox 4X de simulación galáctica persistente: la galaxia existe, cambia y
produce consecuencias aunque el jugador no la esté mirando.

**Estado: M1 (Simulation Kernel).** Encima de la fase 0 (core, reloj, Job System, kernel headless,
profiling) ya hay entidades y componentes, eventos, comandos, guardado y carga versionados, replay y
cambio de frecuencia de los sistemas (LOD lógico). Todavía no hay contenido de juego: la carga de trabajo es
sintética. El nombre y el namespace (`gx`) son provisionales.

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
Scenarios/     cargas sintéticas para tests y benchmarks (no contenido de juego)
Apps/          gx_headless
Benchmarks/    gx_bench
Tests/         gx_tests (13 suites en CTest)
docs/          ARCHITECTURE.md, DECISIONS.md, BENCHMARKS.md
cmake/ scripts/ tools/
```

## Documentación

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): capas, tiempo, pipeline, threading, entidades, eventos,
  comandos, persistencia, determinismo, LOD, referencias, contradicciones y riesgos.
- [docs/DECISIONS.md](docs/DECISIONS.md): registro de decisiones (ADR-001 a ADR-015).
- [docs/BENCHMARKS.md](docs/BENCHMARKS.md): método y resultados medidos por hito.
