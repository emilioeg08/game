# Benchmarks

Ejecutar siempre en Release:

```powershell
./scripts/build.ps1 -Bench          # completo (~5 s)
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
  Esto justifica el LOD lógico (ARCHITECTURE.md §8): los astros deben ir sobre raíles analíticos y las
  flotas lejanas deben agregarse.
- **El paralelismo a escala media está limitado por el grain:** con 1.000 sistemas, el movimiento solo
  tiene 4 chunks (grain 4.096), así que trabajan 4 hilos y el speedup es de ×2,5. Es el primer ajuste
  medido pendiente.
- **Colas:** el paso máximo es 3–7 veces el p95 (planificación del SO). Aún no importa en headless, pero
  sí cuando exista un presupuesto de frame.
- **Aún no disponibles:** guardado, carga y eventos (hito M1).
