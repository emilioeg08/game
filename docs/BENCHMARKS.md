# Benchmarks

Ejecutar siempre en Release:

```powershell
./scripts/build.ps1 -Bench          # completo (~20 s)
./scripts/build.ps1 -Bench -Quick   # hasta 1.000 sistemas
./build/release/bin/gx_bench.exe --filter sim.scaling --threads 6
```

Cada ejecución escribe `bench-results/bench-<fecha UTC>.json`, que git ignora. Las líneas base que
conviene conservar se copian aquí.

## Línea base — Fase 0 (2026-09-26)

Máquina: AMD Ryzen 5 5600X (6 núcleos / 12 hilos), 32 GB, Windows 11, MSVC 19.51, preset `release`,
12 hilos. Dos ejecuciones consecutivas: la variación entre ellas es del 5–10 % (más en 12 hilos por SMT).

### Profiler

| Métrica | Valor |
|---|---|
| Zona activa (registro) | 65–67 ns |
| Zona activa (recogida) | 7–8 ns |
| Zona desactivada | < 1 ns |

### Job System

| Métrica | media | p50 | p99 | máx |
|---|---|---|---|---|
| `parallelFor` fork/join, 12 chunks triviales | 4,5 µs | 2,0 µs | 22–28 µs | ~270 µs |
| `submit` + `wait` de 1 trabajo | 1,4 µs | 1,3 µs | 5–6 µs | 100–320 µs |

Escalado de un kernel limitado por cómputo (4M elementos):

| Hilos | 1 | 2 | 4 | 6 | 8 | 12 |
|---|---|---|---|---|---|---|
| Speedup | 1,00 | 1,95 | 3,4–3,6 | 4,73 | 5,5 | 5,3–6,0 |
| Eficiencia | 100 % | 97 % | 86–89 % | 79 % | 69 % | 44–50 % |

A partir de 6 hilos solo se gana por SMT.

### Simulación sintética, resolución completa (sin LOD)

Configuración: 16 cuerpos y 8 bienes por sistema; movimiento cada minuto simulado y economía cada hora;
se mide 1 día simulado (1.440 pasos) tras 1 hora de calentamiento.

| Sistemas | Cuerpos | Gen. ms | Estado MiB | Paso p50 µs | Paso p95 µs | Movimiento µs | Economía µs | Día ms | Días sim/s | Speedup vs 1 hilo | Determinista |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 16 | 0,03 | 0,00 | 0,4 | 0,5 | 0,2 | 0,7 | 0,7 | ~1.500 | — | sí |
| 10 | 160 | 0,01 | 0,01 | 1,3 | 1,4 | 1,3 | 2,5 | 2,2 | ~450 | — | sí |
| 100 | 1.600 | 0,09 | 0,10 | 8–10 | 13 | 10 | 20 | 15 | ~65 | ~1,0 | sí |
| 1.000 | 16.000 | 0,6–0,8 | 1,05 | 37 | 64–82 | 38 | 88 | 58 | ~17 | 2,5 | sí |
| 5.000 | 80.000 | 2,3 | 5,2 | 104 | 235–268 | 116–122 | 255–275 | 180 | ~5,5 | 4,0 | sí |
| 10.000 | 160.000 | 5,3 | 10,5 | 181–190 | 350–450 | 200–220 | 443–467 | 300–330 | ~3,1 | 4,7 | sí |

### Lectura

- **Determinismo:** el hash con 12 hilos coincide con el de 1 hilo en todas las escalas.
- **Memoria:** no es el límite (10,5 MiB para 160.000 cuerpos). El límite es el coste por paso.
- **Sin LOD, 10.000 sistemas dan ~3 días simulados por segundo**, es decir, ~2 minutos por año simulado.
  Esto justifica el LOD lógico (ARCHITECTURE.md §12): los astros deben ir sobre raíles analíticos y las
  flotas lejanas deben agregarse.
- **El paralelismo a escala media está limitado por el grain:** con 1.000 sistemas, el movimiento solo
  tiene 4 chunks (grain 4.096), así que trabajan 4 hilos y el speedup es de ×2,5. Es el primer ajuste
  medido pendiente.
- **Colas:** el paso máximo es 3–7 veces el p95 (planificación del SO). Aún no importa en headless, pero
  sí cuando exista un presupuesto de frame.
- **Aún no disponibles:** guardado, carga y eventos (hito M1).

## M1 — Simulation Kernel (2026-09-26)

Misma máquina, preset `release` y 12 hilos. **La carga cambió respecto a la fase 0**: ahora incluye
convoyes (entidades creadas por eventos, movidas cada minuto y destruidas al llegar), comandos y despacho de
eventos en cada paso, así que no es comparable celda a celda con la tabla anterior. `motionGrain` pasó de
4096 a 1024 (ADR-015). Rangos de dos ejecuciones consecutivas.

### Simulación sintética con convoyes (1 día simulado)

| Sistemas | Cuerpos | Convoyes en vuelo | Eventos/día | Paso p50 µs | Día ms | Días sim/s | Speedup vs 1 hilo | Determinista |
|---|---|---|---|---|---|---|---|---|
| 100 | 1.600 | 100 | 100 | 9 | 14 | ~70 | ~1,0 | sí |
| 1.000 | 16.000 | 953 | 965 | 26–31 | 41–57 | 17–24 | 2,6–3,3 | sí |
| 5.000 | 80.000 | 4.670 | 4.634 | 82–113 | 133–199 | 5–7,5 | 3,5–5,2 | sí |
| 10.000 | 160.000 | 9.430 | 9.367 | 169–191 | 277–325 | 3,1–3,6 | 4,4–5,1 | sí |

A pesar de la carga añadida, 1.000 sistemas escalan mejor que en la fase 0 (×2,6–3,3 frente a ×2,5) gracias
al nuevo grain.

### Guardado y carga (tras 1 día simulado; el round trip se verifica continuando 6 h)

| Sistemas | Entidades | Payload MiB | Serializar ms | Escribir fichero ms | Leer fichero ms | Cargar ms | Coincide |
|---|---|---|---|---|---|---|---|
| 1.000 | 1.196 | 1,10 | 1,8–2,7 | 2,0 | 2,2 | 0,6–1,3 | sí |
| 10.000 | 11.894 | 10,94 | 20–22 | 8–10 | 17–19 | 7–8 | sí |

### Experimento: almacenamiento de entidades (ADR-011)

Un hilo, ns por entidad, mediana de 5–11 repeticiones.

| Entidades | Denso | Structs grandes | Objetos virtuales (heap) | `unordered_map` | Búsqueda densa | Búsqueda en map | Churn World | Churn map |
|---|---|---|---|---|---|---|---|---|
| 10.000 | 1,6 | 2,5 | 3,6 | 11,8 | 2,8 | 18,5 | 43 | 196 |
| 100.000 | 1,4 | 5,3 | 24,8 | 115,2 | 2,9 | 91,1 | 282 | 528 |
| 1.000.000 | 6,6 | 14,3 | 38,9 | 122,7 | 29,7 | 110,1 | 377 | 643 |

### Experimento: grain del movimiento (ADR-015)

Día ms, mediana de 5. Dos de las cuatro ejecuciones (las otras dos, en ADR-015, muestran el mismo patrón):

| Sistemas | 256 | 512 | 1024 | 2048 | 4096 | 8192 |
|---|---|---|---|---|---|---|
| 100 | 17,1 / 14,3 | 15,9 / 11,7 | **12,6 / 10,5** | 14,8 / 14,7 | 14,3 / 13,3 | 13,8 / 13,2 |
| 1.000 | 58,9 / 38,8 | 53,8 / 49,1 | **50,8 / 59,9** | 50,8 / 55,8 | 60,2 / 61,3 | 81,7 / 87,0 |
| 10.000 | 391 / 338 | 349 / 355 | **356 / 342** | 350 / 333 | 351 / 345 | 337 / 352 |

### Lectura

- **El determinismo se mantiene** con entidades, eventos paralelos, comandos, guardado y carga, en todas
  las escalas.
- **El ruido es alto** en esta máquina (±20–30 % en algunas celdas). Las decisiones se tomaron con efectos
  consistentes entre ejecuciones.
- **Guardar es barato**: ~20 ms para 10.000 sistemas. Con autosave cada pocos minutos de juego no se nota.
- **Build usado**: la primera ejecución de Release fue bloqueada por Smart App Control y los experimentos
  iniciales se hicieron en Profile. Todas las tablas de esta sección son de Release.

## M2.1 — Sandbox jugable (2026-09-26)

`gx_bench --filter sim.sandbox`, Release: un sistema generado (semilla 2400), la nave del jugador y N
cargueros, vuelo estratégico a 1 s y comportamiento de los cargueros cada minuto. Se miden 5 días simulados
(432.000 pasos).

| Cargueros | Hilos | µs/paso | Días simulados/s | Velocidad máxima sostenible |
|---|---|---|---|---|
| 12 | 1 | 4,2 | 2,7 | ~×236.000 |
| 12 | 12 | 4,2 | 2,7 | ~×236.000 |
| 200 | 1 | 16,6 | 0,70 | ~×60.000 |
| 200 | 12 | 16,7 | 0,69 | ~×60.000 |

Antes de optimizar (ADR-020): 8,1 µs/paso con 12 cargueros y 67 µs/paso con 200 (12 hilos).

**Lectura:** con los botones ×1 a ×100K el sandbox va a la velocidad pedida; con ×1M la UI indica
"limitado por CPU" (~×236.000 real). El siguiente salto no vendrá de más hilos (hay poco trabajo por paso)
sino de dar menos pasos: LOD de vuelo con tramos analíticos durante el crucero.

### Tras el reescalado a tiempo real (ADR-021/022)

`gx_bench --filter sim.sandbox --quick`, Release, 1 hora simulada con vuelo cada 200 ms (18.000 pasos):

| Cargueros | Hilos | µs/paso | Margen sobre el tiempo real |
|---|---|---|---|
| 12 | 12 | 5,1 | ~×38.900 |
| 200 | 12 | 17,9 | ~×11.200 |
| 2.000 | 1 | 156,1 | ~×1.280 |
| 2.000 | 12 | 124,4 | ~×1.600 |

Con ×10 como máximo, incluso 2.000 naves en un sistema usan menos del 1 % de un núcleo.

### M2.2 — con sensores (escaneo cada 1 s)

| Cargueros | µs/paso (12 hilos) | Margen sobre el tiempo real |
|---|---|---|
| 12 | 5,6 | ~×35.600 |
| 200 | 21,9 | ~×9.100 |
| 2.000 | 176,5 | ~×1.130 |

La primera versión del escaneo buscaba a los observadores recorriendo todas las naves y los contactos de
forma lineal: con 2.000 cargueros costaba 2,8 ms por escaneo (690 µs/paso de media). Con listas de
observadores por facción y un índice de contactos por objetivo baja a 176 µs/paso.

## M2.3 — Combate (2026-09-26)

`gx_bench --filter sim.sandbox`, Release, 3 horas simuladas con vuelo y combate cada 200 ms (54.000 pasos).
Hay un pirata por cada 20 cargueros (mínimo 3):

| Cargueros | Piratas | µs/paso (12 hilos) | Margen sobre el tiempo real | Disparos | Cargueros perdidos |
|---|---|---|---|---|---|
| 12 | 3 | 8,3 | ~×24.200 | 7.388 | 7 |
| 200 | 10 | 38,6 | ~×5.200 | 92.537 | 103 |
| 2.000 | 100 | 698,5 | ~×286 | 714.263 | 870 |

Zonas con 2.000 cargueros y 100 piratas:

| Zona | Coste |
|---|---|
| `Space.Sensors` | 2,36 ms por escaneo (1 Hz), el 68 % del total |
| `Space.Flight` | 130 µs/paso |
| `Space.Combat` | 73 µs/paso (proyectiles 51, armas 21) |
| `Game.Pirates` | 42 µs por decisión (1 Hz) |

**Prueba de estrés: 2.000 cargueros y 500 piratas** (build profile): ~×90. El escaneo sube a 9 ms.

**Lectura:**

- Los sensores son O(observadores × blancos) por facción. Antes, una facción grande (los cargueros) solo
  observaba al jugador. Ahora dos facciones grandes se observan entre sí, y el coste se lo lleva el
  producto.
- Con ×10 como velocidad máxima, incluso el caso de 2.000 + 100 deja un margen de ~×28.
- Hay dos mejoras claras, cuando haga falta:
  - paralelizar el cálculo de SNR (es de solo lectura; aplicar los resultados seguiría siendo serie);
  - una partición espacial de los observadores, porque el mejor observador suele ser el más cercano.
- `findContact` pasó de búsqueda lineal a binaria: las pistas están ordenadas por id.

**Equilibrio** (`gx_headless --sandbox --minutes 180`, 12 cargueros y 3 piratas):

| Semilla | Cacerías | Cargueros perdidos | Piratas perdidos |
|---|---|---|---|
| 2400 | 30 | 7 | 0 |
| 7 | 61 | 4 | 0 |
| 99 | 30 | 3 | 0 |
| 12345 | 36 | 3 | 0 |

## M3 — Economía (2026-09-27)

`gx_bench --filter sim.economy`, Release. Mercados solos (un mercado tipo planeta con receta,
mantenimiento y dos demandas), 1 hora simulada en ticks de 10 s:

| Mercados | 1 hilo: µs/tick | 12 hilos: µs/tick | ns por mercado (12 hilos) |
|---|---|---|---|
| 1.000 | 25,9 | 20,8 | 20,8 |
| 10.000 | 269 | 72,9 | 7,3 |
| 100.000 | 10.691 | 3.631 | 36,3 |

**Lectura:**

- Un tick cada 10 s simulados con 100.000 mercados cuesta 3,6 ms. A ×10 son 3,6 ms por segundo real.
- Por encima de 10.000 mercados, el coste por mercado sube de 27 a 107 ns (1 hilo): cada `Market` guarda
  sus vectores en el heap y los fallos de caché dominan. Para la fase masiva conviene una disposición
  plana (SoA por bien).
- En el sandbox, la economía cuesta 3 µs por tick. El planificador de 2.000 comerciantes, 162 µs cada
  10 s.
- `sim.sandbox` no cambia de forma apreciable respecto a M2.3 (12 cargueros: ~×23.200; 2.000: ~×250,
  dentro del ruido). Los sensores siguen siendo el 68 % en el caso grande.

**Equilibrio y causalidad** (`gx_headless --sandbox --minutes 600`, Release, 12 cargueros):

| Semilla | Piratas | Entregado | Escasez de agua | Escasez de alimentos | Cargueros perdidos | Créditos de los comerciantes |
|---|---|---|---|---|---|---|
| 2400 | 0 | 16.949 t | 256 t | 238 t | 0 | 317.253 |
| 2400 | 3 | 11.680 t | 472 t | 634 t | 24 | 180.511 |
| 7 | 0 | 14.977 t | 132 t | 58 t | 0 | 264.221 |
| 7 | 3 | 11.476 t | 712 t | 167 t | 22 | 148.010 |

El balance de bienes es exacto (±0,000 t) en todas las ejecuciones.

## M3.1 — Consecuencias (2026-09-27)

Flujos de dinero (`gx_headless --sandbox --minutes 600`, Release, semilla 2400, 12 cargueros):

| Piratas | Entregado | Tesorería final | Impuestos | Reparaciones | Salarios | Quiebras | Cargueros perdidos |
|---|---|---|---|---|---|---|---|
| 0 | 16.361 t | 67.526 | 47.526 | 0 | 36.000 | 0 | 0 |
| 3 | 13.301 t | 93.996 | 44.500 | 29.496 | 35.890 | 3 | 16 |
| 6 | 12.264 t | 81.505 | 42.561 | 18.944 | 35.800 | 1 | 38 |

El coste de simulación no cambia de forma apreciable: los salarios son un sistema por minuto, y los
impuestos y las reparaciones van dentro de operaciones que ya existían.

## M3.2 — Patrullas (2026-09-27)

`gx_headless --sandbox --minutes 600`, Release, 12 cargueros y 3 piratas; hasta 3 patrullas:

| Semilla | Perdidos sin patrullas | Perdidos con patrullas | Piratas abatidos por patrullas | Socorros atendidos |
|---|---|---|---|---|
| 2400 | 16 | 8 | 8 | 12 de 25 |
| 7 | 21 | 7 | 6 | 18 de 38 |
| 99 | 9 | 8 | 0 | 8 de 22 |
| 12345 | 10 | 10 | 6 | 19 de 31 |
| **Total** | **56** | **33 (−41 %)** | 20 | |

**Variabilidad entre trayectorias** (misma semilla 2400, el jugador vuela a puertos distintos):

| Patrullas | Entregas | Cargueros perdidos |
|---|---|---|
| 0 | 11.220–13.301 t | 13–25 |
| 3 | 9.793–14.449 t | 8–16 |

Es una advertencia de método: una sola ejecución no separa efectos de ±20 %.

Coste: las patrullas añaden una facción observadora con 3 naves a los sensores. `Game.Patrols` y
`Game.Authority` no aparecen entre las zonas relevantes.

## M3.3 — Contratos (2026-09-27)

`gx_headless --sandbox --minutes 480`, Release, 12 cargueros y 3 piratas:

| Semilla | Perdidos sin patrullas | Perdidos con patrullas | Patrullas compradas | Contratos publicados (con patrullas) |
|---|---|---|---|---|
| 2400 | 16 | 8 | 2 | 43 |
| 7 | 11 | 6 | 3 | 43 |
| 99 | 9 | 7 | 3 | 43 |
| 12345 | 10 | 10 | 3 | 18 |
| **Total** | **46** | **31 (−33 %)** | | |

Sin patrullas se publican 41–51 contratos en 8 h. La mayoría son suministros, porque en headless nadie los
acepta y caducan.

## M3.4 — Comerciantes con contratos (2026-09-27)

Comparación A/B, `gx_headless --sandbox --minutes 480` con y sin `--no-trader-contracts`, Release:

| | Escasez total | Entregas |
|---|---|---|
| Solo el jugador acepta contratos | 2.942 t | 31.084 t |
| Los comerciantes también | 2.046 t (−30 %) | 35.241 t (+13 %) |

Por semillas, en `docs/DECISIONS.md` (ADR-032). Sin coste de simulación apreciable: el planificador
recorre como mucho 6 contratos abiertos.

## M3.5 — Finanzas (2026-09-27)

Medido por primera vez en **Linux** (contenedor de 4 hilos, GCC 13.3, Release). Las cifras sin finanzas
coinciden exactamente con las de M3.4 medidas en Windows/MSVC (por ejemplo, semilla 2400: 8.959 t
entregadas y 888 t de escasez): en estos escenarios, la simulación es reproducible entre plataformas y
compiladores.

`gx_headless --sandbox --minutes 480` con y sin `--no-finance`, 3 piratas (por defecto):

| Semilla | Entregas (sin / con) | Escasez (sin / con) | Perdidos (sin / con) | Flota final (pico) | Cascos comprados | Prima final |
|---|---|---|---|---|---|---|
| 2400 | 8.959 / 11.917 t | 888 / 1.187 t | 12 / 10 | 24 (24) | 22 | 655 cr/h |
| 7 | 10.417 / 10.138 t | 325 / 853 t | 5 / 5 | 23 (23) | 17 | 513 cr/h |
| 99 | 7.602 / 3.919 t | 826 / 1.633 t | 9 / 11 | 17 (17) | 16 | 872 cr/h |
| 12345 | 8.263 / 7.183 t | 7 / 371 t | 7 / 4 | 10 (12) | 2 | 736 cr/h |

Una sola trayectoria no basta: la semilla 99 con finanzas da entre 3.919 y 7.605 t según la
perturbación (`--fly-to 1..4`), y sin ellas entre 5.056 y 7.602 t. Por eso la tabla principal promedia 4
semillas × 3 trayectorias (`--fly-to 1, 2, 4`). Con 0 piratas, las trayectorias coinciden: el jugador no
perturba nada sin combate.

| Configuración | Entregas | Escasez | Cargueros perdidos | Flota final | Prima |
|---|---|---|---|---|---|
| Sin finanzas, 0 piratas | 9.330 t | 448 t | 0 | 12 | — |
| Con finanzas, 0 piratas | 10.722 t | 300 t | 0 | 20,0 | 253 cr/h |
| Sin finanzas, 3 piratas | 8.746 t | 527 t | 6,8 | 12 | — |
| Con finanzas, 3 piratas | 9.052 t | 771 t | 8,8 | 20,0 | 720 cr/h |
| Con finanzas, 3 piratas, sin patrullas | 8.254 t | 914 t | 10,4 | 19,2 | 847 cr/h |
| Sin finanzas, 6 piratas | 7.713 t | 784 t | 16,6 | 12 | — |
| Con finanzas, 6 piratas | 6.207 t | 1.525 t | 12,7 | 18,9 | 921 cr/h |

Dirección por trayectoria (de 0 a 6 piratas): con finanzas, menos entregas y más escasez en 12 de 12; sin
ellas, en 11 y 9 de 12. Las patrullas mejoran la media, pero por trayectoria solo en 5 de 12.

**Coste:** `Game.Finance` recorre los cargueros una vez por minuto. A igual flota (tope de 12), 8 h de
sandbox tardan 1,92 s con finanzas y 1,95 s sin ellas (3 ejecuciones cada una): diferencia dentro del ruido.
Una flota de 24 cargueros cuesta un ~7 % más de tiempo de simulación.
