# GalaxyEngine

Motor propio en C++20 para un sandbox 4X de simulación galáctica persistente: la galaxia existe, cambia y
produce consecuencias aunque el jugador no la esté mirando.

**Estado: Fase 0 (Foundation).** Core, reloj de simulación, Job System, kernel headless,
logging/asserts/profiling, benchmarks y tests. Todavía no hay contenido de juego. Nombre y namespace
(`gx`) provisionales.

## Requisitos (Windows)

- Visual Studio 2022 o posterior con la carga "Desarrollo para el escritorio con C++" (verificado con
  VS 2026, MSVC 19.51).
- Python 3 para instalar las herramientas de build fijadas:

```powershell
python -m pip install -r tools/requirements-build.txt
```

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
./build/release/bin/gx_headless.exe --days 2 --speed 3600          # 1 h simulada por segundo real
./build/release/bin/gx_headless.exe --days 5 --trace run.trace.json # abrir en https://ui.perfetto.dev
./build/release/bin/gx_headless.exe --help
```

## Estructura

```text
Engine/        Core, Memory, Math, Jobs, Time, Profiling
Simulation/    Kernel: scheduler multi-rate y bucle de pasos
Scenarios/     cargas sintéticas para tests y benchmarks (no contenido de juego)
Apps/          gx_headless
Benchmarks/    gx_bench
Tests/         gx_tests (8 suites en CTest)
docs/          ARCHITECTURE.md, DECISIONS.md, BENCHMARKS.md
cmake/ scripts/ tools/
```

## Documentación

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): capas, tiempo, threading, determinismo, LOD, análisis
  de referencias, contradicciones y riesgos.
- [docs/DECISIONS.md](docs/DECISIONS.md): registro de decisiones.
- [docs/BENCHMARKS.md](docs/BENCHMARKS.md): método y línea base medida.
