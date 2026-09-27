# Arquitectura del motor — GalaxyEngine

> Estado: **M2.1 (Sistema estelar jugable)** sobre M1 y la fase 0: ya hay juego (cliente gráfico) encima del
> motor. Este documento describe lo que existe y las reglas que el código posterior debe respetar. El diseño
> de juego provisional está en [DESIGN.md](DESIGN.md). Las secciones marcadas *(plan)* son diseño aún no
> implementado. Las decisiones y su justificación están en [DECISIONS.md](DECISIONS.md); las mediciones,
> en [BENCHMARKS.md](BENCHMARKS.md).

`GalaxyEngine` y el namespace `gx` son nombres provisionales hasta que `game.md` defina el nombre del juego.

## 1. Principios aplicados

Orden de prioridad (prompt maestro §3): correctitud → escalabilidad → determinismo → rendimiento medible →
depurabilidad → moddabilidad → presentación.

| Principio | Cómo se hace cumplir hoy |
|---|---|
| Simulación sin render | `Simulation/` no depende de nada gráfico; `gx_headless` corre, guarda, carga y reproduce la simulación completa. |
| Determinismo | Tiempo entero, RNG entero por *stream*, chunks fijos, reducciones ordenadas, eventos fusionados en orden de chunk, comandos con marca de tiempo, `/fp:precise`. Tests, hash de estado y replay lo verifican. |
| Causalidad | Los efectos viajan por mecanismos explícitos: los excedentes crean convoyes (eventos → entidades), los convoyes entregan carga al llegar y los raids la destruyen. Nada aparece "por arte de magia". |
| Medir primero | Profiler, `gx_bench` con JSON y experimentos medidos para las decisiones (almacenamiento de entidades, grain). |
| No construir todo de golpe | Solo existen la fase 0 y M1. No se crean carpetas vacías de la estructura sugerida. |
| Correctitud | `GX_CHECK` siempre activo para invariantes; `GX_ASSERT` en Debug y Profile; warnings como errores; la entrada externa se valida y nunca se da por buena. |

## 2. Capas y dependencias

```text
 Apps/Game (gx_game)          cliente gráfico: SDL3 + Dear ImGui (ThirdParty/), solo presentación y entrada
 Apps/Headless · Benchmarks · Tests                      ejecutables sin ventana
        │
 Game/     Sandbox (escenario jugable) · Presentation (SystemSnapshot) · contenido provisional
 Scenarios/                                              cargas sintéticas (NO contenido de juego)
        │
 Space/    Orbits · Bodies · Ships · Sensors · Combat · Generation  dominio espacial
        │
 Simulation/  Kernel · World · Events · Commands · Economy   estado autoritativo y bucle de pasos
        │
 Engine/  Core · Memory · Math · Jobs · Time · Profiling · Serialization
```

Reglas:

- Las dependencias solo van hacia abajo. `Engine` nunca incluye `Simulation`; `Simulation`, `Space` y
  `Game` nunca incluyen render, input ni UI. **Solo `Apps/Game` enlaza SDL3 e ImGui**: el motor, la
  simulación, el juego y los tests compilan y corren sin dependencias externas (`-DGX_BUILD_CLIENT=OFF`).
- El render futuro será un **consumidor** de la fase `PresentationSnapshot`, nunca una dependencia de la
  simulación.
- Los sistemas de juego reales vivirán en `Simulation/<Dominio>` y `Space/<Dominio>` a medida que existan.

## 3. Módulos existentes

| Módulo | Contenido |
|---|---|
| `Engine/Core` | tipos, plataforma, `GX_CHECK`/`GX_ASSERT`, logging con sinks, hash (FNV-1a, `StateHasher`, `hashBytes`), RNG (SplitMix64 + xoshiro256**) verificado contra vectores de referencia |
| `Engine/Memory` | `LinearArena` (scratch por paso) |
| `Engine/Math` | `Vec3d` |
| `Engine/Jobs` | `JobSystem`: pool fijo, `submit/wait`, `parallelFor`, `parallelReduce` |
| `Engine/Time` | `SimTime`/`SimDuration` (µs enteros), `SimClock`, `TimeController`, calendario, `Stopwatch` |
| `Engine/Profiling` | zonas, agregados, exportación a Chrome trace, memoria del proceso, estadísticas |
| `Engine/Serialization` | `BinaryWriter`/`BinaryReader` (little-endian, chunks versionados), fichero de guardado con checksum y escritura atómica |
| `Simulation/Kernel` | `SystemScheduler` multi-rate con cambio de frecuencia, `Simulation` (paso, `runUntil`, save/load, hash de estado) |
| `Simulation/World` | `EntityId` generacional, `EntityRegistry`, `ComponentStore<T>` (sparse set), `World` |
| `Simulation/Events` | `EventChannel<T>`, `EventBus` (emisión serie y paralela, despacho con cascadas) |
| `Simulation/Commands` | `CommandQueue` (entrada externa serializada, grabación para replay) |
| `Simulation/Economy` | mercados (existencias, precios, recetas, demandas), bodegas, carteras, operaciones tonelada a tonelada, libros de precios y `EconomySystem` paralelo; finanzas (`Finance.h`): libros de banco y mutua con identidades contables, y estimadores de experiencia (ADR-033) |
| `Space/Orbits` | órbitas keplerianas analíticas (solver de Kepler con Newton protegido) |
| `Space/Bodies` | `CelestialBody`, `OrbitsParent`, estado absoluto por la cadena de padres |
| `Space/Ships` | componentes de nave, autopiloto (`steer`), `FlightSystem` (dos pases, eventos `ShipArrived`), módulos y daño (`ShipModules`, `applyDamage`, `applyModuleEffects`) |
| `Space/Sensors` | firmas, detección pasiva y activa, transpondedor, imagen de sensores por facción (`SensorSystem`) |
| `Space/Combat` | armas (haz y proyectil), control de tiro sobre pistas de sensores, proyectiles, `ShipDamaged` / `ShipDestroyed` y retirada de naves destruidas (`CombatSystem`) |
| `Space/Generation` | generación procedural del sistema estelar (datos puros) y creación de entidades |
| `Game/Sandbox` | escenario jugable: jugador, cargueros NPC, `PilotCommand`, diario, LOD de vuelo |
| `Game/Presentation` | `SystemSnapshot`: foto de solo lectura para el cliente |
| `Scenarios` | `SyntheticGalaxy`: movimiento, economía, comercio, convoyes (entidades, eventos y comandos) y reducción global |
| `Apps/Headless` | `gx_headless`: simulación, `--save/--load/--inspect/--record/--replay/--raids` |
| `Apps/Game` | `gx_game`: cliente gráfico (mapa, paneles, inspector, guardado rápido, modo captura) |
| `Benchmarks` | `gx_bench`: motor, almacenamiento de entidades, escalado, grain, save/load |
| `Tests` | framework mínimo + 15 suites en CTest (104 tests) |

## 4. Modelo de tiempo

**Tiempo entero.** `SimTime` son microsegundos en `i64` desde la época de la campaña, con un rango de
±292.000 años. No acumula deriva y es idéntico bit a bit en cualquier plataforma.

**Scheduler multi-rate.** Cada sistema declara su periodo y un desfase opcional, y se ejecuta sobre su
propia rejilla. El kernel salta directamente al siguiente instante en que toca un sistema **o un comando**.
Los sistemas estratégicos no cuestan nada entre ejecuciones: un año con un sistema horario son 8.760 pasos.
Cada sistema recibe como `dt` el tiempo real transcurrido desde su última ejecución.

**Cambio de frecuencia (LOD lógico).** `Simulation::setSystemPeriod` cambia el ritmo de un sistema. La
siguiente ejecución ocurre un periodo nuevo después de la última (o un periodo nuevo después de ahora, si
eso ya pasó), y el `dt` cubre todo el intervalo: ningún microsegundo se pierde ni se cuenta dos veces (test
`Kernel.RateSwitchKeepsTimeContinuity`). Si el cambio se pide durante un paso, se aplica al final del paso.

**Aceleración y pausa.** `TimeController` convierte tiempo real en un objetivo de tiempo simulado.
Acelerar significa más pasos por segundo real, nunca un `dt` mayor. `runUntil(target, presupuesto)` puede
cortarse y reanudarse sin alterar el resultado.

**Calendario provisional.** 12 meses con la duración gregoriana, años de 365 días y año de época
configurable. El calendario definitivo lo decide `game.md`.

## 5. Pipeline de un paso

```text
avanzar el reloj al siguiente instante debido (sistema o comando)
Commands            comandos pendientes con executeAt <= ahora, luego sistemas de la fase
Simulation          sistemas paralelos: leen estado compartido, escriben solo estado propio
Synchronization     aplican efectos cruzados guardados en buffers
EventResolution     suscriptores de eventos (con cascadas), luego sistemas de la fase
History             leen los eventos del paso (registro histórico: plan M4)
PresentationSnapshot publicación de solo lectura para render/UI (plan)
fin de paso         cambios de frecuencia diferidos, descarte de eventos, recogida del profiler
```

Dentro de una fase, los sistemas se ejecutan en orden de registro.

## 5b. Presentación y cliente

- El cliente **no toca el estado vivo**: lee un `SystemSnapshot` (cuerpos, naves, rumbos) construido una vez
  por frame, y **solo cambia la simulación con comandos** (`PilotCommand`). Así la partida es igual de
  determinista, guardable y reproducible con o sin ventana.
- **Interpolación visual**: las naves se integran en pasos de 1 s (o de 100 ms), así que la foto las
  extrapola con su velocidad hasta el instante actual. Los cuerpos son exactos (órbitas analíticas).
- **Precisión**: la cámara trabaja en `double` y convierte a `float` relativo a su centro (origin rebasing
  implícito), de modo que el zoom va de metros a decenas de UA sin temblores.
- **Ritmo**: `TimeController` fija el objetivo de tiempo simulado y el cliente ejecuta `runUntil` con un
  presupuesto de 8 ms por frame. Si la CPU no llega, la UI muestra la velocidad real conseguida.
- **Modo captura** (`--frames N --screenshot x.png`): renderiza, guarda PNG y sale. Sirve para verificar la
  presentación de forma automática.

## 6. Modelo de threading

`JobSystem` usa un pool fijo (hardware − 1 workers, más el hilo que espera). Mientras un hilo espera,
ejecuta trabajos de la cola. `parallelFor` reparte los chunks con un contador atómico.

**Reglas obligatorias para código paralelo de simulación:**

1. En la fase `Simulation`, un sistema escribe solo en índices que le pertenecen. Puede leer datos ajenos
   que nadie modifique en esa fase.
2. Los efectos sobre otras entidades van por buffers (aplicados en `Synchronization`) o por eventos
   (resueltos en `EventResolution`).
3. **Los cambios estructurales** (crear o destruir entidades, añadir o quitar componentes) **solo ocurren
   en el hilo principal**: en los handlers de comandos, en los suscriptores de eventos o entre pasos. Los
   sistemas paralelos solo modifican valores in situ.
4. El `grain` de `parallelFor` es una constante de configuración y nunca se deriva del número de hilos.
5. La aleatoriedad sale de `Rng::forStream(worldSeed, entidad, ejecución)`.
6. Las reducciones en coma flotante se hacen con `parallelReduce`. Los eventos emitidos en paralelo usan
   `emitFromChunk(begin / grain, …)`.

## 7. Entidades y estado

- **`EntityId`** = índice de slot + generación. Al destruir una entidad se incrementa la generación de su
  slot, así que un id obsoleto se detecta en lugar de apuntar a otra entidad. Los slots libres se reutilizan
  en orden FIFO, lo que retrasa la reutilización y es determinista.
- **`ComponentStore<T>`** (sparse set): valores densos y contiguos, búsqueda por id en O(1) y borrado por
  intercambio con el último. El orden de iteración depende solo de la secuencia de operaciones.
- **`World`**: registro de entidades más un almacén por tipo de componente, registrado con un **nombre
  estable** (el que usan los guardados). `destroyEntity` quita la entidad de todos los almacenes.
- No hay maquinaria genérica de consultas ni de sistemas: cada sistema recorre el almacén que conduce y
  busca los demás por id. Es una decisión medida (ADR-011).
- El estado que no es de entidades (arrays densos por sistema estelar, por ejemplo) se registra como
  **bloque de estado** con nombre (`Simulation::addStateBlock`) y se guarda igual que el resto.

## 8. Eventos

- Los eventos existen **durante un paso**: se emiten en cualquier fase, se despachan a los suscriptores al
  comienzo de `EventResolution`, las fases posteriores los pueden leer (`History`) y se descartan al final
  del paso. Nunca se guardan, porque los guardados se hacen entre pasos.
- Los suscriptores se ejecutan en el hilo principal, por canal en orden de registro y en orden de
  suscripción. Pueden emitir más eventos (cascadas), que se procesan en pasadas sucesivas. Si una cascada
  no converge en 16 pasadas, falla un `GX_CHECK`.
- En emisión paralela, cada chunk tiene su buffer y los buffers se concatenan en orden de chunk, de modo
  que el orden final no depende del número de hilos (test `Events.ParallelEmissionOrderIsIndependentOfThreadCount`).

## 9. Comandos y replay

- Un **comando** es entrada externa (jugador, UI, script, red), serializada en el momento de enviarse, con
  `executeAt` y un número de secuencia.
- Los comandos **siempre se ejecutan estrictamente en el futuro** (al menos 1 µs después del instante
  actual), al comienzo de la fase `Commands` del paso de ese instante y en orden `(executeAt, secuencia)`.
  Así el mismo registro reproduce exactamente la misma simulación, tanto en vivo como en replay.
- Los handlers **validan** los datos del comando: los comandos inválidos se rechazan y se cuentan, nunca
  abortan la simulación.
- Grabación: `commands().setRecording(true)` y luego `recorded()`. Replay: `submitRecord()` de cada
  registro sobre una simulación nueva. `gx_headless --record/--replay` lo hace desde la línea de comandos y
  compara el hash final.

## 10. Persistencia

- **Payload** (`Simulation::saveState`): chunks `KRNL` (semilla, reloj, contadores), `SCHD` (estado de
  cada sistema por nombre: periodo, desfase, última ejecución, siguiente vencimiento), `CMDQ` (comandos
  pendientes), `WRLD` (registro de entidades más un chunk por almacén de componentes) y `BLKS` (bloques de
  estado).
- **Fichero**: *magic* + cabecera (versión del formato, versión del motor, descripción) + payload protegido
  por tamaño y hash. Se escribe en `.tmp` y se renombra (escritura atómica). `gx_headless --inspect`
  muestra la cabecera y los chunks.
- **Compatibilidad**: la carga exige las **mismas registraciones** (sistemas, componentes, tipos de
  comando, bloques). Cualquier diferencia se rechaza con un mensaje claro. Dentro de un chunk, los campos
  añadidos al final por versiones más nuevas se saltan (compatibilidad hacia delante). La migración entre
  versiones de contenido está pendiente (§15).
- **Hash de estado** = hash del payload. Si dos simulaciones tienen el mismo hash, son la misma simulación.
  Esto garantiza que el hash cubre exactamente lo que se guarda.
- Verificado: guardar, cargar y continuar da el mismo hash que una ejecución continua, y cargar y volver a
  guardar produce los mismos bytes (suite `SaveLoad`; `gx_headless` de extremo a extremo; benchmark
  `sim.saveload`).

## 11. Contrato de determinismo

**Garantizado hoy** (suites `Determinism`, `Kernel`, `Events`, `Commands`, `SaveLoad` y `Sandbox`; columna `det` del
benchmark; `--replay`): con el mismo binario, la misma plataforma, la misma semilla y configuración y la
misma secuencia de comandos, el hash del estado es idéntico **sea cual sea** el número de hilos, la
velocidad del tiempo, el troceado de la ejecución o si hubo un guardado y una carga por el camino.

**No garantizado todavía:** el mismo resultado con compiladores o CPUs distintos (ADR-005).

## 12. LOD lógico

Implementado: el mecanismo de **frecuencia** (scheduler multi-rate más `setSystemPeriod` con continuidad
temporal) y su **primer uso real**: el vuelo pasa de 1 s a 100 ms cuando el jugador pilota a mano y vuelve a
1 s al dejarlo, por comando y de forma determinista. *(Plan)* las **representaciones**:

| LOD | Representación | Frecuencia orientativa |
|---|---|---|
| 0 Abstracto | organizaciones y regiones como números agregados | diaria / semanal |
| 1 Regional | sistemas y flotas agregadas (composición, no naves) | horaria |
| 2 Sistema | astros sobre **raíles keplerianos analíticos** (posición = f(t)), naves simplificadas | minutos |
| 3 Local | naves individuales integradas | segundos |
| 4 Tiempo real | combate, física, daño por componente | 20–50 ms |

Una flota agregada guardará su composición y se materializará de forma determinista a partir de ese estado
y de su *stream* de RNG. **Evidencia medida:** sin LOD, 10.000 sistemas con todo integrado cada minuto dan
unos 3 días simulados por segundo, así que un año simulado tarda unos 2 minutos.

## 13. Espacio y precisión *(plan, fase 2)*

Coordenadas jerárquicas, como exige el zoom continuo visto en las referencias:

- **Galaxia:** posición de cada sistema en `double`. A 10⁵ años luz la resolución es de unos 100 km, así
  que nunca se guardan naves en coordenadas galácticas.
- **Sistema:** metros en `double` relativos a la estrella. A 40 UA la resolución es de ~1 mm.
- **Local/táctico:** *origin rebasing* alrededor del foco para física fina y render en `float`.

## 14. Profiling y herramientas

- `GX_PROFILE_SCOPE("Nombre")`: unos 65 ns activado y menos de 1 ns desactivado. Solo en zonas gruesas.
- `gx_headless --trace x.json` → <https://ui.perfetto.dev>.
- **Save Inspector** (mínimo): `gx_headless --inspect partida.gxsave`.
- Inspectores del §26 del prompt *(plan)*: consumirán `PresentationSnapshot` y consultas de depuración.

## 15. Análisis de las referencias visuales

Se recibieron 4 capturas (en el chat; la carpeta `referencias/` no existe en el repositorio). Se usan
solo como referencia conceptual: la interfaz, los nombres y los assets serán originales.

| Referencia | Qué muestra | Implicación técnica |
|---|---|---|
| Universo continuo | zoom desde el mapa interestelar hasta el sistema; órbitas, cinturón de asteroides con cientos de cuerpos, población por cuerpo, flotas coloreadas por facción | coordenadas jerárquicas (§13); astros sobre raíles analíticos; población agregada; lo que se ve depende de los sensores y no del estado real |
| Planetas simulados | superficies en rejilla hexagonal con biomas y depósitos de recursos | generación bajo demanda desde el *stream* del planeta y persistencia **solo de las diferencias** |
| Editor de nave | casillas de módulos con daño individual, estadísticas derivadas, hiperespacio y subluminal, tripulación con modificadores | daño por subsistema; estadísticas recalculadas cuando cambian y no en cada tick; módulos como datos; dos regímenes de movimiento |
| Vista de sistema | colonias y estaciones con población, controles de tiempo, fecha con meses, HUD con sensores y transpondedor | `TimeController` y calendario con meses (hechos); identidad y transpondedor dentro del modelo de sensores |

## 16. Contradicciones y huecos detectados

| # | Contradicción o hueco | Resolución |
|---|---|---|
| 1 | Faltan `game.md`, `CLAUDE.md` y `referencias/` | La fase 0 y M1 no dependen de ellos. Las decisiones de contenido quedan marcadas como provisionales. |
| 2 | `game.md` propone Godot 4 (según el prompt) | Queda reemplazado por el motor propio en C++ (ADR-001). |
| 3 | El "primer vertical slice" (§5) abarca las fases 1 a 4 del roadmap; el orden del §4 no incluye render ni input | El slice es el objetivo de los hitos M1 a M4. El render mínimo de depuración entra en M2 como capa separada. |
| 4 | "Determinista cuando sea posible" + multithreading + `double` | Determinismo garantizado con el mismo binario y plataforma; el multiplataforma queda aplazado (§11). |
| 5 | Tiempo acelerable a "años" + combate en tiempo real | Tiempo entero y scheduler multi-rate (§4). |
| 6 | La estructura sugerida incluye `Engine/ECS`, pero se pide "ECS solo si es útil" | Almacenes densos por tipo en `Simulation/World`, sin maquinaria ECS genérica; decisión medida (ADR-011). |
| 7 | Métricas de guardado, carga y eventos desde el primer benchmark | Disponibles desde M1 (`sim.saveload` y columna `events`). |
| 8 | La estructura sugerida no tiene sitio para el kernel, las cargas sintéticas, las apps, los benchmarks ni los tests | Se añaden `Simulation/Kernel`, `Scenarios/`, `Apps/`, `Benchmarks/` y `Tests/`. |

## 17. Riesgos abiertos

1. **Documentos de autoridad ausentes.** Cualquier sistema de juego (M2+) necesita `game.md`.
2. **Smart App Control bloquea binarios recién compilados.** Windows bloquea de forma intermitente algunos
   ejecutables locales sin firmar (le ha ocurrido a `gx_bench` en Release y a `gx_tests` en Debug y
   Profile). Cada compilación puede recibir un veredicto distinto. Hay que decidir cómo desarrollar con esta
   protección (es una configuración de seguridad del usuario).
3. **Ruido de medición.** Entre ejecuciones del benchmark hay variaciones de hasta un ±30 % en algunas
   celdas (SMT, frecuencia de la CPU, sincronización de OneDrive). Las decisiones deben basarse en efectos
   consistentes entre varias ejecuciones, no en una sola tabla.
4. **Repositorio dentro de OneDrive.** Sincroniza `build/` y `.git`, lo que añade ruido y riesgo de
   bloqueos. Recomendación: mover el repositorio a una ruta local.
5. **Compatibilidad de guardados estricta.** Cualquier cambio de registraciones invalida las partidas
   anteriores. Hará falta un mecanismo de migración antes de tener contenido real.
6. **La carga no es transaccional.** Si `loadState` falla a mitad, la simulación queda inconsistente y se
   debe descartar (documentado en la API; `gx_headless` sale).
7. **Cola única del Job System.** Suficiente con trabajos gruesos; *work-stealing* solo si se mide
   contención.
8. **Carga sintética poco representativa.** La IA, los sensores y la política reales tendrán otros
   patrones de coste.
9. **Plataformas.** Windows x64 es la plataforma de lanzamiento (Steam, ADR-034): la CI compila y prueba
   ahí con MSVC en cada push, y arranca el paquete. El cliente gráfico solo está verificado en Windows. El motor, la simulación, el
   headless y los tests también compilan sin avisos y pasan en Linux con GCC 13 y Clang 18 (desde M3.5), y
   las trayectorias medidas coinciden con las de Windows. La igualdad entre plataformas no está garantizada
   (§11): es una observación, no un contrato.
10. **Escala en tiempo real (ADR-021).** Con ×10 como máximo, el cliente va sobrado de CPU, pero la
    economía, la política y la sucesión deberán diseñarse para avanzar a ritmo real. El universo lejano
    (otros sistemas) tendrá que simularse de forma agregada (LOD 0–1) para no pagar su coste en tiempo real.
11. **Dependencias descargadas al configurar.** SDL3 e ImGui se descargan de GitHub (versiones y SHA-256
    fijados) la primera vez que se configura cada preset. Sin red, usar `-DGX_BUILD_CLIENT=OFF`; sin acceso
    a GitHub, apuntar `FETCHCONTENT_SOURCE_DIR_SDL3` y `FETCHCONTENT_SOURCE_DIR_IMGUI` a copias locales de las
    mismas versiones (en Linux sin X11, SDL con `-DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF`
    y `SDL_VIDEO_DRIVER=offscreen` para capturas).
12. **Diseño provisional.** Todo el diseño de juego es mío hasta que exista `game.md` (DESIGN.md).
