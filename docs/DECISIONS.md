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

## ADR-006 — Sin ECS todavía *(sustituida por ADR-011)*

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

## ADR-011 — Entidades: almacenes densos por tipo, sin ECS genérico

- **Contexto:** M1 necesitaba identidad estable y almacenamiento de entidades. El prompt pide medir antes de
  elegir y no introducir ECS por moda.
- **Experimento** (`gx_bench --filter entity.storage`, Release, 1 hilo): actualizar 1M de entidades cuesta
  **6,6 ns/entidad** con almacén denso, frente a 14 ns con structs grandes, 39 ns con objetos virtuales en
  el heap y 123 ns con `unordered_map`. Búsqueda aleatoria por id: 30 ns frente a 110 ns del hash map.
  Churn (destruir + crear): 377 ns frente a 643 ns.
- **Decisión:** `EntityId` generacional, `EntityRegistry` con reutilización FIFO y un `ComponentStore<T>`
  (sparse set) por tipo, registrado con nombre estable. **Sin** consultas genéricas ni sistemas ECS: cada
  sistema recorre el almacén que conduce y busca los demás por id. Los datos no ligados a entidades se
  guardan como bloques de estado.
- **Consecuencias:** recorrer un almacén es lineal y paralelizable. Unir varios almacenes cuesta una
  búsqueda por id; si aparecen joins masivos, se medirá la agrupación por arquetipos.

## ADR-012 — Eventos con vida de un paso y despacho en EventResolution

- **Decisión:** eventos tipados por canal que se emiten en cualquier fase, se despachan en el hilo
  principal al comienzo de `EventResolution` (con cascadas de hasta 16 pasadas) y se descartan al final del
  paso. En emisión paralela, cada chunk tiene su buffer y se concatenan en orden de chunk.
- **Alternativa descartada:** sistemas periódicos que consultan los canales. Un evento emitido en un paso en
  el que ese sistema no toca se perdería.
- **Consecuencias:** los cambios estructurales derivados de trabajo paralelo (crear o destruir entidades)
  pasan por eventos, lo que mantiene los sistemas paralelos libres de cambios estructurales. El registro
  histórico persistente (M4) consumirá los eventos en la fase `History`.

## ADR-013 — Comandos: datos con marca de tiempo, siempre en el futuro

- **Decisión:** toda entrada externa es un comando serializado con `(executeAt, secuencia)`. Se aplica al
  comienzo de la fase `Commands` del paso en ese instante, y `executeAt` se fuerza a ser como mínimo el
  instante actual + 1 µs.
- **Motivo:** si un comando pudiera aplicarse "ahora", en vivo se ejecutaría después de los sistemas del
  instante actual y en replay antes. Obligarlo a caer en el futuro da el mismo orden en ambos casos (test
  `Commands.ReplayReproducesTheLiveRun`).
- **Consecuencias:** replay y grabación triviales, y los comandos pendientes forman parte del guardado. Los
  handlers tratan los comandos como entrada no fiable.

## ADR-014 — Formato de guardado v1: chunks versionados y compatibilidad estricta

- **Decisión:** payload en chunks (`KRNL`, `SCHD`, `CMDQ`, `WRLD`, `BLKS`), little-endian explícito,
  referencias por nombre y no por orden de registro. Fichero con *magic*, versión, hash del payload y
  escritura atómica. La carga exige las mismas registraciones; los campos añadidos al final de un chunk se
  ignoran. El hash de estado es el hash del payload.
- **Aplazado:** migración entre versiones de contenido, carga transaccional y compresión.
- **Medido:** 10.000 sistemas y 11.900 convoyes suponen 10,9 MiB, que se serializan en ~20 ms y se cargan
  en ~8 ms.

## ADR-015 — Grain del movimiento: 1024

- **Contexto:** con grain 4096, 1.000 sistemas (16.000 cuerpos) daban solo 4 chunks y el speedup era de
  ×2,5.
- **Experimento** (`gx_bench --filter sim.grain`, cuatro ejecuciones con mediana de 5): con 1.000
  sistemas, 4096 es entre un 18 % y un 58 % más lento que el mejor valor y 8192 entre un 60 % y un 120 %, en
  todas las ejecuciones. Con 100 sistemas, 1024 fue el mejor en 2 de 4 ejecuciones y en las otras quedó a
  menos del 4 % del mejor. Con 10.000 sistemas, las diferencias entre 512 y 4096 están dentro del ruido
  (±5 %) y 256 fue el peor en 3 de 4. Una primera medición en el build Profile sugería 256, pero no se
  reprodujo en Release.
- **Decisión:** `motionGrain = 1024`, el mejor o casi el mejor en todas las escalas.
- **Lección:** una sola tabla no basta en esta máquina; las decisiones de rendimiento se toman con varias
  ejecuciones (riesgo 3 de ARCHITECTURE.md).
