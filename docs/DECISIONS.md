# Registro de decisiones de arquitectura

Formato breve: contexto → decisión → consecuencias. Una decisión solo se cambia con otra entrada que la
sustituya y explique por qué.

## ADR-001 — Motor propio en C++ (sustituye la propuesta de Godot 4)

- **Contexto:** según el prompt maestro, `game.md` proponía Godot 4. El prompt v2 exige un motor propio
  orientado a simulación masiva, determinismo y paralelismo.
- **Decisión:** motor propio en C++20. Se admiten librerías externas cuando aporten un beneficio claro,
  pero nunca como núcleo de la simulación.
- **Consecuencias:** hay que construir herramientas que un motor comercial trae de serie (render,
  editor, UI). Se compensa porque la simulación no queda subordinada a nada externo.

## ADR-002 — Build: CMake + Ninja + MSVC, toolchain fijado

- **Decisión:** CMake ≥ 3.25 con `CMakePresets.json` (`debug`, `release`, `profile`) y Ninja. CMake y
  Ninja fijados en `tools/requirements-build.txt`; MSVC de Visual Studio 2026. Warnings `/W4` como
  errores en los presets. `scripts/build.ps1` importa el entorno de MSVC.
- **Consecuencias:** el build es reproducible y rápido. Linux/macOS deberían funcionar con los mismos
  presets y GCC/Clang, pero no está verificado.

## ADR-003 — Tiempo entero y scheduler multi-rate

- **Decisión:** `SimTime` = microsegundos en `i64`. Cada sistema tiene periodo propio y el kernel salta
  al siguiente instante debido. La aceleración nunca cambia el `dt`.
- **Alternativas descartadas:** un tick fijo global (inviable para simular años) y tiempo en `double`
  (deriva y dependencia de la plataforma).
- **Consecuencias:** el LOD lógico se expresa como frecuencia más representación. Los sistemas deben
  aceptar su `dt` real (el primer `dt` tras un desfase es más corto).

## ADR-004 — Job System simple, medido antes de optimizar

- **Decisión:** pool fijo con una cola FIFO protegida por mutex. `parallelFor` reparte los chunks con un
  contador atómico, y quien espera ejecuta trabajos.
- **Alternativa aplazada:** *work-stealing* (deques Chase-Lev). Solo se implementará si el benchmark
  muestra contención en la cola.
- **Consecuencias:** es sencillo y verificable. Medido: fork/join p50 ≈ 2 µs y escalado de ×4,7 con 6
  hilos en una CPU de 6 núcleos.

## ADR-005 — Contrato de determinismo

- **Decisión:** determinismo garantizado con el mismo binario y la misma plataforma, sea cual sea el
  número de hilos o el ritmo. Reglas en ARCHITECTURE.md §6–7: chunks fijos, RNG por *stream*,
  reducciones ordenadas, doble buffer entre fases, `/fp:precise`, nada de funciones trascendentes en el
  estado.
- **Aplazado:** determinismo entre compiladores y plataformas (requeriría punto fijo en el estado
  crítico). Se revisará si se necesita multijugador lockstep o replays multiplataforma.

## ADR-006 — Sin ECS todavía

- **Decisión:** SoA dentro de cada sistema. La elección entre ECS y tablas con *handles* generacionales se
  tomará en M1 con un experimento medido.
- **Motivo:** el prompt pide no introducir ECS por moda; todavía no hay entidades reales que modelar.

## ADR-007 — Framework de tests propio y mínimo

- **Decisión:** unas 150 líneas (`Tests/TestFramework.h`), una entrada de CTest por suite y ninguna
  descarga externa.
- **Consecuencias:** se reemplazará por doctest o Catch2 si hacen falta *fixtures*, parametrización o
  informes JUnit.

## ADR-008 — Profiler propio con exportación a Chrome trace

- **Decisión:** zonas con alcance en buffers por hilo, agregados por nombre y traza para Perfetto. Los
  nombres deben tener duración estática (`persistentName` para los dinámicos).
- **Consecuencias:** coste de ~65 ns por zona activa, así que solo se usan zonas gruesas. Tracy es una
  opción futura si hace falta profiling en vivo.

## ADR-009 — Calendario provisional con meses

- **Decisión:** 12 meses con duración gregoriana, 365 días sin bisiestos y año de época configurable.
- **Motivo:** las referencias muestran fechas con mes y año, y un año de longitud fija mantiene el
  calendario trivialmente determinista. `game.md` tiene la última palabra.

## ADR-010 — Sin carpetas vacías

- **Decisión:** de la estructura sugerida en §6 solo se crean los módulos que tienen código. Se añaden
  `Simulation/Kernel`, `Scenarios`, `Apps`, `Benchmarks` y `Tests`, que la estructura no contemplaba.
- **Motivo:** el andamiaje vacío sugiere funcionalidad que no existe.
