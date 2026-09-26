# Arquitectura del motor — GalaxyEngine

> Estado: **Fase 0 (Foundation) completada**. Este documento describe lo que existe y las reglas que el
> código posterior debe respetar. Las secciones marcadas *(plan)* son diseño aún no implementado.
> Las decisiones y su justificación están en [DECISIONS.md](DECISIONS.md); las mediciones, en
> [BENCHMARKS.md](BENCHMARKS.md).

`GalaxyEngine` y el namespace `gx` son nombres provisionales hasta que `game.md` defina el nombre del juego.

## 1. Principios aplicados

Orden de prioridad (prompt maestro §3): correctitud → escalabilidad → determinismo → rendimiento medible →
depurabilidad → moddabilidad → presentación.

| Principio | Cómo se hace cumplir hoy |
|---|---|
| Simulación sin render | `Simulation/` no depende de nada gráfico; `gx_headless` corre la simulación completa. |
| Determinismo | Tiempo entero, RNG entero por *stream*, chunks fijos, reducciones ordenadas, `/fp:precise`. Tests + hash de estado lo verifican. |
| Medir primero | Profiler desde el día 1, `gx_bench` con JSON, columna de determinismo en cada escala. |
| No construir todo de golpe | Solo existe la fase 0. No se crean carpetas vacías de la estructura sugerida. |
| Correctitud | `GX_CHECK` siempre activo para invariantes; `GX_ASSERT` en Debug/Profile; warnings como errores. |

## 2. Capas y dependencias

```text
 Apps/Headless   Benchmarks   Tests          ejecutables
        │             │          │
        └──────┬──────┴──────────┘
               ▼
          Scenarios/                          cargas sintéticas (NO contenido de juego)
               ▼
     Simulation/Kernel                        reloj autoritativo + scheduler + bucle de pasos
               ▼
 Engine/  Core · Memory · Math · Jobs · Time · Profiling
```

Reglas:

- Las dependencias solo van hacia abajo. `Engine` nunca incluye `Simulation`; `Simulation` nunca incluye
  render, input ni UI.
- El render futuro será un **consumidor** de la fase `PresentationSnapshot`, nunca una dependencia de la
  simulación.
- Los sistemas de juego reales vivirán en `Simulation/<Dominio>` y `Space/<Dominio>` a medida que existan.

## 3. Módulos existentes

| Módulo | Contenido | Notas |
|---|---|---|
| `Engine/Core` | tipos, plataforma, `GX_CHECK`/`GX_ASSERT`, logging con sinks, hash (FNV-1a, `StateHasher`), RNG (SplitMix64 + xoshiro256**) | RNG y hash verificados contra vectores de referencia publicados. |
| `Engine/Memory` | `LinearArena` (bump allocator por paso) | Scratch por paso del kernel. |
| `Engine/Math` | `Vec3d` | Solo operaciones correctamente redondeadas en rutas deterministas. |
| `Engine/Jobs` | `JobSystem`: pool fijo, `submit/wait`, `parallelFor`, `parallelReduce` | Ver §6. |
| `Engine/Time` | `SimTime`/`SimDuration` (µs enteros), `SimClock`, `TimeController`, calendario, `Stopwatch` | Ver §4. |
| `Engine/Profiling` | zonas con `GX_PROFILE_SCOPE`, agregados, exportación a Chrome trace, memoria del proceso, estadísticas (p50/p95/p99) | Ver §9. |
| `Simulation/Kernel` | `SystemScheduler` multi-rate, `Simulation` (paso, `runUntil`, presupuesto de tiempo real) | Headless por construcción. |
| `Scenarios` | `SyntheticGalaxy`: carga sintética con movimiento, economía, comercio y reducción global | Solo para tests y benchmarks. |
| `Apps/Headless` | `gx_headless`: simulación sin UI, sin ritmo o al ritmo del reloj real (`--speed`) | Traza Perfetto con `--trace`. |
| `Benchmarks` | `gx_bench` | Escalas de 1 a 10.000 sistemas. |
| `Tests` | framework mínimo propio + 8 suites registradas en CTest | |

## 4. Modelo de tiempo

**Tiempo entero.** `SimTime` son microsegundos en `i64` desde la época de la campaña, con un rango de
±292.000 años. No acumula deriva en campañas largas y es idéntico bit a bit en cualquier plataforma. Solo
se convierte a `double` en el borde (el `dt` de un integrador).

**Scheduler multi-rate.** Cada sistema declara su periodo y un desfase opcional, y se ejecuta sobre su
propia rejilla (`alta + offset + k·periodo`). El kernel **salta directamente al siguiente instante en que
algún sistema toca**:

- los sistemas estratégicos (economía horaria, política diaria) no cuestan nada entre ejecuciones: un año
  con solo un sistema horario son 8.760 pasos, no 31,5 millones de ticks (test `Kernel.IdleTimeIsSkipped`);
- los sistemas tácticos (combate, física) solo cuestan mientras están registrados;
- cada sistema recibe `dt` = tiempo desde su última ejecución, que siempre es exacto.

**Aceleración y pausa.** `TimeController` convierte tiempo real en un *objetivo* de tiempo simulado
(velocidad entera, pausa, arrastre de restos inferiores al µs, tope de retraso). Acelerar significa **más
pasos por segundo real, nunca un `dt` mayor**, así que cualquier velocidad, framerate o patrón de pausa
produce exactamente el mismo estado. `Simulation::runUntil(target, presupuesto)` puede cortarse por
presupuesto de tiempo real y reanudarse sin alterar el resultado (test `Kernel.ResultDoesNotDependOnHowTheRunIsSliced`).

**Calendario provisional.** 12 meses con la duración gregoriana y años de 365 días sin bisiestos, con año
de época configurable (las referencias visuales muestran fechas del tipo "January 2352"). El calendario
definitivo lo decide `game.md`.

## 5. Pipeline de un paso

Dentro de un instante, los sistemas debidos se ejecutan por fase y, dentro de cada fase, en orden de
registro. Es la secuencia del prompt maestro §9:

| Fase | Uso |
|---|---|
| `Commands` | aplicar comandos del jugador y de la IA (entrada determinista) |
| `Simulation` | actualización paralela: leer estado compartido y escribir **solo estado propio** |
| `Synchronization` | aplicar efectos cruzados guardados en buffers (p. ej., flujos de comercio) |
| `EventResolution` | resolver eventos emitidos en el paso *(plan M1)* |
| `History` | registrar acontecimientos *(plan M4)* |
| `PresentationSnapshot` | publicar estado de solo lectura para render/UI/inspectores *(plan)* |

Al final de cada paso: el arena de scratch se reinicia y el profiler recoge las zonas.

## 6. Modelo de threading

`JobSystem` usa un pool fijo (hardware − 1 workers, más el hilo que espera). Mientras un hilo espera en
`wait()`, ejecuta trabajos de la cola: el hilo principal nunca queda ocioso y el paralelismo anidado no
puede bloquearse. `parallelFor` reparte los chunks con un contador atómico, de modo que solo se encola un
trabajo auxiliar por worker y no uno por chunk.

**Reglas obligatorias para código paralelo de simulación:**

1. En la fase `Simulation`, un sistema escribe solo en índices que le pertenecen. Puede leer datos ajenos
   que nadie modifique en esa fase.
2. Los efectos sobre otras entidades se escriben en buffers y se aplican en `Synchronization`, con el
   patrón de doble buffer que ejemplifica `SyntheticGalaxy`.
3. El `grain` de `parallelFor` es una constante de configuración y **nunca se deriva del número de hilos**:
   los límites de los chunks dependen solo de `(count, grain)`.
4. La aleatoriedad sale de `Rng::forStream(seed, entidad, ejecución)`. Nunca se comparte un generador
   entre trabajos.
5. Las sumas y demás reducciones en coma flotante se hacen con `parallelReduce`, que combina los
   resultados parciales en orden de chunk.

## 7. Contrato de determinismo

**Garantizado hoy** (verificado por las suites `Determinism` y `Kernel` y por la columna `det` del
benchmark): con el mismo binario, la misma plataforma, la misma semilla, la misma configuración y la misma
secuencia de comandos, el hash del estado es idéntico, **sea cual sea** el número de hilos, la velocidad del
tiempo o la forma de trocear la ejecución.

**No garantizado todavía:** el mismo resultado con compiladores o CPUs distintos. Mitigaciones activas:
`/fp:precise` y `-ffp-contract=off` (sin FMA implícito), nada de funciones trascendentes de libm en
estado determinista, RNG solo con enteros y calendario entero. Si más adelante se necesitan replays
multiplataforma o multijugador lockstep, el estado crítico deberá pasar a punto fijo. Es una decisión
aplazada (ver DECISIONS.md, ADR-005).

## 8. LOD lógico *(plan)*

El scheduler multi-rate es el mecanismo base: cada LOD es una combinación de **frecuencia** y
**representación**.

| LOD | Representación | Frecuencia orientativa |
|---|---|---|
| 0 Abstracto | organizaciones y regiones como números agregados | diaria / semanal |
| 1 Regional | sistemas y flotas agregadas (composición, no naves) | horaria |
| 2 Sistema | astros sobre **raíles keplerianos analíticos** (posición = f(t), sin integrar), naves simplificadas | minutos |
| 3 Local | naves individuales integradas (como el movimiento de `SyntheticGalaxy`) | segundos |
| 4 Tiempo real | combate, física, daño por componente | 20–50 ms |

Las transiciones deben conservar el estado. Una flota agregada guarda su composición (naves y resumen de
salud por componente) y se materializa de forma determinista a partir de ese estado y de su *stream* de
RNG. Al desmaterializarse vuelve a agregarse. **Evidencia medida:** sin LOD, 10.000 sistemas con todo
integrado cada minuto simulado dan unos 3 días simulados por segundo, así que un año simulado tarda unos 2
minutos. El LOD es imprescindible para simular años.

## 9. Espacio y precisión *(plan, fase 2)*

Se usarán coordenadas jerárquicas, que es lo que exige el zoom continuo de estrella a sistema visto en las
referencias:

- **Galaxia:** posición de cada sistema en `double`. A 10⁵ años luz (~10²¹ m) la resolución es de unos
  100 km, así que **nunca** se guardan naves en coordenadas galácticas.
- **Sistema:** metros en `double` relativos a la estrella. A 40 UA (~6·10¹² m) la resolución es del orden
  del milímetro.
- **Local/táctico:** *origin rebasing* alrededor del foco para física fina y render en `float`.

Antes de comprometer la solución se hará un experimento pequeño y medido.

## 10. Entidades y datos *(plan M1)*

No hay ECS: el patrón actual son arrays SoA dentro de cada sistema. En M1 se decidirá, con un experimento
medido, entre *handles* generacionales sobre tablas SoA por tipo o un ECS mínimo. Se separará identidad,
estado, comportamiento, representación y persistencia. El contenido (naves, módulos, bienes…) será datos
desde el principio, para que sea moddable.

## 11. Profiling y herramientas

- `GX_PROFILE_SCOPE("Nombre")`. Coste medido: unos 65 ns activado y menos de 1 ns desactivado. **Solo en
  zonas gruesas** (sistema, lote de chunks), nunca por entidad.
- `parallelFor` registra la parte de trabajo de cada hilo, de modo que la traza muestra el uso real de los
  workers.
- `gx_headless --trace x.json` → abrir en <https://ui.perfetto.dev>.
- Inspectores del §26 del prompt *(plan)*: consumirán `PresentationSnapshot` y consultas de depuración, no
  el estado vivo.

## 12. Análisis de las referencias visuales

Se recibieron 4 capturas (en el chat; la carpeta `referencias/` no existe en el repositorio). Se usan
solo como referencia conceptual: la interfaz, los nombres y los assets serán originales.

| Referencia | Qué muestra | Implicación técnica |
|---|---|---|
| Universo continuo | zoom desde el mapa interestelar hasta el sistema; órbitas, cinturón de asteroides con cientos de cuerpos, población por cuerpo, flotas coloreadas por facción | coordenadas jerárquicas (§9); astros sobre raíles analíticos; población agregada; lo que se ve depende de los sensores y no del estado real |
| Planetas simulados | superficies en rejilla hexagonal con biomas y depósitos de recursos | generación bajo demanda desde el *stream* del planeta y persistencia **solo de las diferencias**: no se puede tener cada superficie en memoria |
| Editor de nave | casillas de módulos con daño individual, estadísticas derivadas (masa, tripulación, firma, detección pasiva), velocidad en hiperespacio y subluminal, tripulación con modificadores y costes de reparación | daño por subsistema (§12 del prompt); estadísticas recalculadas cuando cambian y no en cada tick; módulos definidos como datos; dos regímenes de movimiento |
| Vista de sistema | planetas, colonias y estaciones con población, controles de tiempo (pausa, x1, avanzar), fecha con meses, HUD de nave con sensores activos/pasivos y transpondedor | `TimeController` (hecho); calendario con meses (hecho, provisional); identidad y transpondedor como parte del modelo de sensores |

## 13. Contradicciones y huecos detectados

| # | Contradicción o hueco | Resolución |
|---|---|---|
| 1 | Faltan `game.md`, `CLAUDE.md` y `referencias/` | La fase 0 no depende de ellos. Las decisiones de contenido (calendario, nombres, escalas, reglas) quedan marcadas como provisionales. |
| 2 | `game.md` propone Godot 4 (según el prompt) | Queda reemplazado por el motor propio en C++ (ADR-001). No se ha podido revisar el resto de la sección porque el archivo no está. |
| 3 | El "primer vertical slice" (§5) incluye economía, comercio, IA, save/load y pilotar una nave, lo que abarca las fases 1 a 4 del roadmap (§32); el orden del §4 no incluye render ni input | El slice es el objetivo de los hitos M1 a M4. El render mínimo de depuración entra en M2 como capa separada. |
| 4 | "Determinista cuando sea posible" + multithreading + `double` | Determinismo garantizado con el mismo binario y plataforma e independiente de los hilos; el multiplataforma queda aplazado (§7). |
| 5 | Tiempo acelerable a "años" + combate en tiempo real | Un tick fijo único no sirve (años a 20 Hz son unos 630 millones de ticks por año). Se resuelve con tiempo entero y scheduler multi-rate (§4). |
| 6 | La estructura sugerida (§6) incluye `Engine/ECS`, pero se pide "ECS solo si es útil" | No hay ECS; la decisión se tomará midiendo en M1 (§10). |
| 7 | Se piden métricas de guardado, carga y eventos desde el primer benchmark | No existen hasta M1; el benchmark las marca como no disponibles. |
| 8 | La estructura sugerida no tiene sitio para el kernel, las cargas sintéticas, las apps, los benchmarks ni los tests | Se añaden `Simulation/Kernel`, `Scenarios/`, `Apps/`, `Benchmarks/` y `Tests/`. No se crean carpetas vacías. |

## 14. Riesgos abiertos

1. **Documentos de autoridad ausentes.** Cualquier sistema de juego (M2+) necesita `game.md`.
2. **Repositorio dentro de OneDrive.** OneDrive sincroniza `build/` (cientos de MB) y puede bloquear
   ficheros durante el enlazado, y `.git` dentro de OneDrive es frágil. Recomendación: mover el
   repositorio a una ruta local (p. ej., `C:\dev\Game`) y usar un remoto git como copia de seguridad.
3. **Granularidad de los chunks.** Con `motionGrain = 4096`, 16.000 cuerpos son solo 4 chunks, de modo
   que se usan 4 de los 12 hilos y el speedup a 1.000 sistemas es de ×2,5. Hay que ajustar el grain por
   sistema midiendo.
4. **Cola única del Job System.** Basta con trabajos gruesos (fork/join p50 ≈ 2 µs), pero si aparecen
   miles de trabajos pequeños por paso habrá contención. Se añadirá *work-stealing* solo si el benchmark
   lo demuestra.
5. **Latencias de cola.** El paso máximo llega a ~1,4 ms con 10k sistemas (planificación del SO), frente
   a un p95 de ~0,4 ms. Hay que vigilarlo cuando haya presupuesto de frame.
6. **Carga sintética poco representativa.** La IA, los sensores y la política reales tendrán otros
   patrones de coste y memoria. Los números de la fase 0 validan el mecanismo, no el juego.
7. **Solo verificado en Windows/MSVC.** Las rutas de Linux y macOS están escritas pero no se han compilado.
