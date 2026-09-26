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
