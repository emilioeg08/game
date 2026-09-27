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

## ADR-016 — Cliente gráfico con SDL3 + Dear ImGui, fijados por versión y hash

- **Contexto:** para jugar hace falta ventana, entrada y una interfaz densa (paneles, inspectores). El
  usuario aprobó descargar SDL3 y Dear ImGui.
- **Decisión:** SDL 3.4.16 (estático; ventana, entrada y render 2D sobre Direct3D 11) y Dear ImGui v1.92.9b,
  vía `FetchContent` con `URL_HASH SHA256`. Solo los enlaza `Apps/Game`. El mapa se dibuja con las draw lists
  de ImGui. `GX_BUILD_CLIENT=OFF` compila todo lo demás sin descargar nada.
- **Alternativas:** raylib (más sencilla, menos adecuada para herramientas densas) y un render propio sobre
  D3D, Vulkan o bgfx (prematuro: el mapa 2D no lo necesita). El render 3D se decidirá cuando haga falta.
- **Consecuencias:** la presentación es sustituible. La simulación no sabe que existe.

## ADR-017 — Modelo de vuelo provisional: newtoniano, impulsor de alto empuje, sin gravedad sobre naves

- **Decisión:** inercia más empuje máximo, crucero como límite del autopiloto y un controlador de velocidad
  deseada que planifica la frenada al 80 % del empuje y limita la velocidad de cierre a lo que cubre un paso
  (nunca se pasa). Sin gravedad sobre naves por ahora (DESIGN.md).
- **Verificado:** llega y se detiene con pasos de 0,1, 1 y 5 s, a menos del 15 % del tiempo ideal
  (`Space.AutopilotReachesAPointAndStops`), y mantiene posición junto a un planeta en órbita
  (`Space.AutopilotKeepsStationWithAnOrbitingBody`).
- **Corrección hecha durante el slice:** el objetivo se evalúa en el instante al que se refiere el estado de
  la nave (el paso anterior). Comparar la nave en t−dt con el objetivo en t provocaba un error fijo de ~22 km
  al mantener posición junto a un planeta a 30 km/s.

## ADR-018 — El cliente solo lee fotos y solo escribe comandos

- **Decisión:** `SystemSnapshot` es la única vista del estado para la presentación, y `PilotCommand` la
  única vía de cambio. Las naves se extrapolan para dibujarlas entre pasos gruesos.
- **Consecuencias:** la partida del cliente es idéntica a la headless (los tests del `Sandbox` cubren el
  mismo código que juega el usuario) y se puede guardar, cargar y reproducir.

## ADR-019 — Inspector de entidades generado a partir de `io`

- **Decisión:** los archivos aceptan `io("nombre", campo)`. El formato binario ignora el nombre y un
  `InspectArchive` lo pasa a un `FieldVisitor`. Los enums con `toString` muestran su nombre.
- **Consecuencias:** cada componente describe sus campos una sola vez para guardado, hash e inspector. El
  inspector del cliente (y los futuros inspectores de guardado o economía) salen gratis.

## ADR-020 — Vuelo: estados de cuerpos memorizados por paso; grain 256

- **Medición** (`sim.sandbox`, 200 cargueros): el pase de objetivos resolvía una órbita por nave y costaba
  67 µs/paso con 12 hilos. Memorizar el estado de cada cuerpo una vez por paso (bajo demanda) lo deja en
  16,6 µs/paso (×4). Con 13 naves, de 8,1 a 4,2 µs/paso.
- **Medición:** con 200 naves, repartir el pase de naves entre hilos era más lento que hacerlo en serie
  (~40 ns por nave). Se sube el grain a 256: el paralelismo empieza con miles de naves.

## ADR-021 — Escala temporal: ×1 = tiempo real; velocidades ×1, ×3 y ×10 (directiva del usuario)

- **Decisión:** el cliente ofrece solo pausa, ×1, ×3 y ×10, con ×1 como tiempo real. Todo el contenido se
  reescala para que un viaje entre planetas dure minutos a ×1: los cargueros esperan en puerto de 30 s a
  3 min, y el vuelo va a 200 ms en modo estratégico y a 50 ms con pilotaje manual. Los tests y benchmarks
  headless siguen pudiendo simular tan rápido como permita la CPU.

## ADR-022 — Motor sublumínico + hiperespacio con pozos gravitatorios (sustituye parte de ADR-017)

- **Decisión:** el sublumínico newtoniano sirve para maniobrar (Correo: 50 km/s² y crucero de
  15.000 km/s; Carguero: 15 km/s² y 6.000 km/s). El hiperespacio sirve para cruzar el sistema (Correo:
  1,5 millones de km/s con 5 s de carga; Carguero: 600.000 km/s con 12 s). No se puede saltar dentro de un
  pozo gravitatorio (estrella: 20 radios; planetas: 25 radios). Se sale en el borde del pozo del destino,
  con la velocidad igualada a la suya. Las órdenes que no son de autopiloto cancelan la carga o provocan
  una salida de emergencia.
- **Verificado:** `Space.HyperspaceJumpsOutsideWellsAndDropsAtTheTargetWell` comprueba que la nave sale del
  pozo, salta una sola vez fuera de él, sale del hiperespacio en el borde del pozo del destino (±20 km) y
  llega en menos de 12 min para ~2,2 UA.

## ADR-023 — Sensores: imagen por facción, sin omnisciencia

- **Contexto:** prompt §14. Ni el jugador ni la IA deben saberlo todo; los sensores pasivos son discretos e
  imprecisos, los activos son precisos pero delatan al emisor, y hay ruido, falsos positivos,
  identificación parcial y pérdida de contacto. Las referencias muestran detección activa y pasiva, firma de
  sensores y un transpondedor que se puede apagar.
- **Decisión:**
  - Cada nave tiene una emisión = base + parte del empuje en uso + radar encendido + carga o viaje en
    hiperespacio.
  - SNR pasivo = emisión · sensibilidad / d². SNR activo = potencia · sección / d⁴.
  - La detección es segura con SNR ≥ 1 y probabilística entre 0,25 y 1.
  - Hay tres niveles de conocimiento: desconocido, clasificado (SNR ≥ 4) e identificado (SNR pasivo ≥ 25,
    SNR activo ≥ 4, o transpondedor a menos de 1 UA).
  - La posición estimada lleva un error proporcional a d/√SNR.
  - Los contactos que no se refrescan en 20 s se pierden, y hay un 4 % de fantasmas por escaneo que duran 4 s.
  - Cada facción guarda una `FactionPicture` (parte del estado guardado). El cliente dibuja solo la del
    jugador; "Mostrar la verdad" es una opción de depuración.
- **Simplificación consciente:** la asociación de pistas es perfecta (cada contacto sabe internamente a qué
  nave corresponde), pero ese dato nunca llega a la interfaz salvo cuando la nave está identificada. Ir a un
  contacto significa ir a su *posición estimada*, no a la nave real.
- **Coste:** O(observadores × objetivos de otras facciones) por escaneo (1 Hz). Con dos facciones es
  despreciable. Con muchas facciones hará falta partición espacial.

## ADR-024 — Daño localizado por módulos; estadísticas derivadas

- **Contexto:** prompt §12 y el editor de naves de las referencias, que muestra daño por módulo. Una sola
  barra de vida no permite nada interesante: ni inutilizar sin destruir, ni huir con el motor tocado, ni
  quedarse ciego.
- **Decisión:**
  - `ShipModules` es una lista de módulos (estructura, reactor, motor, hipermotor, sensores, armas, bodega,
    habitáculo), cada uno con su salud. `ShipDesignStats` guarda el rendimiento de la nave intacta.
  - `applyModuleEffects` recalcula `ShipDrive` y `SensorSuite` solo cuando cambian los módulos, nunca en
    cada tick.
  - `applyDamage` es una función pura con su propio flujo aleatorio: la estructura absorbe el 50 %, un
    módulo elegido en proporción a su tamaño recibe el 100 %, por debajo del 10 % el módulo queda fuera de
    servicio, y si el reactor queda fuera de servicio hay un 30 % de probabilidad de brecha.
- **Consecuencias:** las naves se pueden inutilizar sin destruirlas (base para el abordaje y el botín de
  M3), y el control de daños y las reparaciones son reglas de juego sobre salud de módulos. El formato de
  guardado cambia: las partidas anteriores a M2.3 no cargan (se rechazan por componentes distintos, sin
  corromper nada).

## ADR-025 — Combate: se dispara a pistas de sensores; proyectiles simulados; mismo ritmo que el vuelo

- **Contexto:** prompt §14 (sin omnisciencia). Si las armas apuntaran a entidades, los sensores no
  servirían de nada en combate.
- **Decisión:**
  - `CombatControl.targetTrack` es un id de pista de la imagen de la facción. Para disparar hace falta
    fijar la pista con los sensores propios (SNR ≥ 1). A un fantasma o a una pista perdida no se puede
    disparar.
  - El error de puntería es la medida (el mismo modelo que los sensores), más el error del arma, más la
    aceleración real del blanco durante una latencia de 50 ms.
  - Los haces son instantáneos. Los proyectiles se simulan en línea recta, con prueba de máxima
    aproximación contra cada casco durante el paso, y pueden alcanzar a otra nave.
  - `FlightMode::Pursue` sigue una pista extrapolada: el autopiloto persigue la estimación, no la nave.
  - El combate corre justo después del vuelo y con su mismo periodo: 200 ms en modo estratégico y 50 ms en
    táctico (con pilotaje manual, disparando, persiguiendo o tras recibir un impacto en los últimos 30 s).
    Las posiciones se refieren así al mismo instante.
  - Las naves destruidas se eliminan en `EventResolution` (`Space.Combat.Cleanup`), después de que todos
    los suscriptores hayan visto `ShipDestroyed`.
  - Los proyectiles forman parte del estado guardado. Los haces y explosiones recientes son solo
    presentación.
- **Alternativas descartadas:**
  - Apuntar a la entidad y tirar un dado de acierto: sería omnisciente, y sin física.
  - Proyectiles como entidades del mundo: con decenas en vuelo, un vector en el sistema es más simple y
    más barato.
- **Verificado:** los tests `Combat.*` cubren:
  - el radar hace preciso el tiro a 700 km sobre un blanco silencioso (acierta más del 95 %; en pasivo,
    menos del 20 %);
  - un blanco que acelera esquiva los proyectiles a 2.000 km;
  - sin fijación o fuera de alcance no hay disparos;
  - guardar con un proyectil en vuelo y continuar da el mismo estado.
- **Bug encontrado al medir:** si un impacto destruía el hipermotor durante la carga, la nave entraba en
  hiperespacio a velocidad 0 y no salía nunca. Ahora perder el hipermotor aborta la carga y provoca una
  salida de emergencia (test `Space.LosingTheHyperdriveAbortsAJump`).

## ADR-026 — Piratas, reapariciones y mantenimiento del sistema

- **Decisión:**
  - La IA pirata es una máquina de estados simple (acecho, caza, huida) que solo lee la imagen de sensores
    de su facción. Los parámetros viven en `Content.h`.
  - Reparaciones, control de daños, reapariciones y salida de los piratas que huyen van en `Game.Upkeep`
    (1 s, fase `EventResolution`, porque crea y destruye entidades).
  - El sistema no es cerrado: los cargueros y los piratas perdidos se reponen con retraso, y el jugador
    recibe una nave nueva en la estación principal.
- **Mediciones que cambiaron el diseño:**
  1. Con emboscadas en planetas elegidos al azar, los piratas pasaban la mayor parte del tiempo en
     hiperespacio: hasta 20 UA, más de 40 minutos por viaje. Ahora eligen entre los 3 planetas más
     cercanos.
  2. Con el daño inicial, un corsario destruía al jugador en 5 s. Se redujo el daño unas 6 veces para
     duelos de alrededor de un minuto.
  3. Tres piratas persiguieron sin fin a una presa inalcanzable (el bug del hipermotor). Además del arreglo,
     las cacerías caducan a los 3 minutos.

## ADR-027 — Economía: mercados por existencias, recetas y transporte físico

- **Contexto:** prompt §18 (oferta, demanda, producción, consumo, almacenamiento, comercio, logística) y
  §30 (causalidad: si cae una ruta deben cambiar de verdad precios y disponibilidad). El primer vertical
  slice (§5) pide economía mínima y comercio.
- **Decisión:**
  - `Simulation/Economy` es genérico, sin dependencia del espacio. Tiene:
    - `Market` (bienes con existencias, objetivo y capacidad; recetas; demandas);
    - `CargoHold` y `Wallet`;
    - el precio en función de existencias / objetivo (curva exponencial acotada);
    - operaciones tonelada a tonelada;
    - `EconomySystem`, que ejecuta todos los mercados cada 10 s en paralelo (son independientes entre
      operaciones).
  - Hay dos clases de insumo: los esenciales limitan la producción; los de mantenimiento, si faltan, la
    bajan al 35 %. Así la cadena se degrada en lugar de bloquearse.
  - Los créditos son enteros (`i64`) y el precio de cada tonelada se redondea. Las existencias son `f64`
    porque la producción es continua; la carga va en toneladas enteras.
  - Los bienes solo se mueven en bodegas. Una nave destruida pierde su carga, que queda contabilizada.
  - El contenido (bienes, perfiles de puerto y reparto de capacidad al generar) está en `Content.h`.
- **Verificado:**
  - Conservación exacta: existencias iniciales + producido − consumido − perdido = existencias + carga,
    con 0,000 t de diferencia en todas las ejecuciones medidas.
  - El test `Economy.ParallelMarketsAreDeterministic` da el mismo estado con 0, 3 y 7 hilos.
  - El guardado y la carga siguen dando el mismo hash (`Sandbox.SaveLoadContinuesIdentically`).
- **Consecuencias:** el formato de guardado cambia otra vez. Faltan salarios, impuestos, crédito y
  quiebra (§18), y la población no tiene todavía efectos más allá de su consumo.

## ADR-028 — Conocimiento de precios y comerciantes que planifican

- **Contexto:** prompt §14 y §17: ni la IA ni el jugador son omniscientes, y la IA actúa según objetivos,
  riesgos y conocimiento disponible.
- **Decisión:**
  - `PriceBook` es un libro por facción con la edad de cada observación. Se actualiza al atracar, y las
    estaciones publican el de los comerciantes. Guarda también la profundidad del mercado (su objetivo).
  - El planificador del carguero es una utilidad explícita: beneficio / segundos, descontado por la edad
    del dato y por el peligro del puerto.
  - Estima los ingresos siguiendo la curva de precios real, contando las entregas que la red ya tiene en
    camino y la capacidad del mercado.
  - Tiene tres recursos: comprar aquí, reposicionarse vacío o explorar.
- **Mediciones que cambiaron el diseño:**
  1. La primera versión solo cargaba en el puerto donde estaba: había puertos sin agua a 57 cr mientras
     otros la acumulaban a 5 cr. De ahí el reposicionamiento.
  2. Sin piratas, los comerciantes perdían dinero (de 36.000 a 29.765 cr en 3 h): varios elegían la
     misma ruta con el mismo libro y hundían un mercado poco profundo. Se añadieron las entregas en camino
     y la estimación sobre la curva, y los mercados pasaron a 4 horas de existencias. Ahora ganan: de
     36.000 a 317.253 cr en 10 h.
  3. En los sistemas sin planeta oceánico había hambre estructural permanente. De ahí el reparto de
     capacidad al generar (20 % de margen sobre el consumo total, recorriendo la cadena hacia arriba y
     repitiendo hasta estabilizar).
  4. Un bug de herramienta, no de diseño: un parche hecho con PowerShell 5.1 guardó texto con la
     codificación rota ("BoletÃ­n"). El test del boletín lo detectó.

## ADR-029 — Consecuencias: Autoridad, salarios, recompensas, reputación con testigos y abordaje

- **Contexto:** prompt §18 (salarios, impuestos, quiebra), §30 (causalidad) y el primer slice (§5):
  combatir y comerciar deben tener consecuencias.
- **Decisión:**
  - La Autoridad del sistema tiene tesorería. Cobra el 3 % de cada operación (`buyGoods`/`sellGoods`
    con `taxRate`) y las reparaciones en estación (3 cr por punto), y paga recompensas por piratas al
    jugador.
  - Los comerciantes pagan salarios cada minuto (`Game.Payroll`). Si están en números rojos al salir de
    puerto, quiebran y se retiran en `Game.Upkeep`.
  - La reputación del jugador solo cambia por hechos que la red de comerciantes ha **visto**: al atacar a
    un carguero, cuenta si su `FactionPicture` tiene al jugador *identificado*. Los golpes repetidos a la
    misma nave cuentan una vez por minuto.
  - Con reputación hostil, los puertos niegan comercio, reparaciones y boletín.
  - El abordaje es un comando (`BoardCommand`, validado): la nave objetivo debe estar sin energía, a menos
    de 5 km y con la velocidad igualada. La carga pasa a tu bodega (lo que no cabe se contabiliza como
    perdido) y el casco se retira en la fase de comandos.
  - **Cambio en ADR-023:** la identificación pasiva pasa de SNR 25 a 400. Con 25, un carguero identificaba
    a un Correo quieto a 4.470 km, más allá del alcance de cualquier arma: el anonimato era imposible y la
    regla de testigos no significaba nada. Con 400 hay una ventana real (cañón de riel a 1.100–3.000 km,
    con mala puntería sin radar).
- **Verificado:**
  - impuesto exacto por tonelada;
  - reparaciones cobradas y denegadas sin fondos;
  - quiebra con reemplazo;
  - recompensa y reputación al destruir un pirata;
  - reputación solo con testigo: −10 con el transpondedor encendido a 150 km, 0 con él apagado a 2.000 km;
  - abordaje (20 t tomadas, 30 perdidas, nave con energía rechazada) y hostilidad tras dos abordajes;
  - conservación de bienes intacta.
- **Medido:** a 4 h, 6 piratas dan *más* entregas que ninguno: el capital nuevo de los cargueros de
  reemplazo lo enmascara. A 8 h la relación es robusta (12.192 frente a 9.090 t), y la prueba causal pasó
  a 8 h.

## ADR-030 — Patrullas de la Autoridad: presupuesto explícito, sensores propios y llamadas de socorro

- **Contexto:** la tesorería de ADR-029 solo acumulaba. Prompt §30: el dinero debe tener consecuencias,
  y la seguridad de las rutas debe depender de algo que se pueda romper.
- **Decisión:**
  - Hay una cuarta facción, la **Autoridad**, con la clase Patrullero.
  - `Game.Authority` (cada 2 min, en `EventResolution`, porque crea y destruye entidades) paga el
    mantenimiento, pone en servicio un patrullero si tiene su precio más 4 h de mantenimiento de la flota
    ampliada, y da de baja uno si la tesorería queda en negativo.
  - `Game.Patrols` (1 s) es una máquina de estados (patrulla, persecución, reparación) que solo lee la
    imagen de sensores de la Autoridad.
  - Los cargueros atacados emiten **llamadas de socorro** (`DistressCall`: posición e instante, guardadas
    en el estado) por la red de comerciantes. Acude el patrullero libre más cercano.
  - Los piratas evitan las patrullas.
  - La muerte de un jugador hostil salda su cuenta.
- **Medición que cambió el diseño:** en la primera versión las patrullas solo hacían rondas por los
  planetas con más pérdidas. Con 4 semillas no cambiaban nada: 56 frente a 58 cargueros perdidos, y 13
  piratas abatidos en 40 h. La causa es de información: un pirata al acecho es silencioso (solo
  clasificable a menos de ~32.000 km), y la ronda llega tarde a donde ya no está. Con llamadas de socorro,
  las pérdidas bajan un 41 % en las mismas 4 semillas (56 → 33).
- **Lección sobre la medición:** con 5 perturbaciones mínimas de la misma semilla, las entregas varían
  entre 9.800 y 14.400 t. Una sola ejecución no basta para decidir. Las decisiones de equilibrio se toman
  con varias variantes, y las pruebas causales comparan una trayectoria fija con un margen amplio (8 h,
  16 frente a 8 pérdidas).
- **Consecuencias:** el diario distingue jugador, noticias y tráfico (`JournalKind`). El tráfico se borra
  primero al llenarse y el cliente lo oculta por defecto.
