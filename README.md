# GalaxyEngine

Motor propio en C++20 para un sandbox 4X de simulación galáctica persistente: la galaxia existe, cambia y
produce consecuencias aunque el jugador no la esté mirando.

**Estado: M4 (De nave a empresa: flota propia y minería).** El vertical slice del GDD (§26) está completo. Hay un
juego en tiempo real (×1, ×3, ×10, ×30):

- un sistema estelar generado con órbitas keplerianas;
- tu nave, con autopiloto y salto al hiperespacio fuera de los pozos gravitatorios, o con empuje newtoniano
  manual;
- cargueros NPC que viajan solos entre puertos;
- guardado rápido e inspector de entidades.

Los sensores pasivos y activos (radar R, transpondedor T) crean niebla de guerra: solo ves tu flota y lo que
detectas.

En combate, las armas (láser y cañón de riel) disparan a pistas de sensores, el daño es por módulos y los
piratas acechan junto a los pozos para cazar cargueros (y a ti). Hay reparaciones en las estaciones y
reapariciones.

En la economía, cada puerto produce, consume y fija precios según sus existencias, en una cadena que va de
la extracción a la refinería, la fábrica y la maquinaria. Los cargueros comercian con lo que sabe su red, y
tú compras y vendes al atracar. Las pérdidas por piratería se notan en la escasez y en los precios.

Todo tiene consecuencias. La Autoridad del sistema cobra impuestos y reparaciones y paga recompensas por
piratas, y los comerciantes pagan salarios y pueden quebrar. Tu reputación cae si los comerciantes te
identifican atacándolos (con el transpondedor apagado y a distancia pueden no saberlo), y puedes abordar
naves sin energía para llevarte su carga.

La Autoridad convierte lo que recauda en patrullas. Acuden a las llamadas de socorro de los cargueros,
persiguen piratas y van a por ti si eres hostil. Con ellas se pierden un tercio menos de cargueros.

Hay contratos que nacen de lo que pasa: suministros urgentes a puertos con escasez real (los paga el
puerto) y recompensas por piratas identificados (las paga la Autoridad). Se aceptan en las estaciones (K),
y los cargueros compiten contigo por los suministros.

El dinero tiene origen. Los cargueros ya no reaparecen gratis: se compran con ahorros y con crédito del
banco del sistema, que presta si el negocio cubre la deuda y guarda los ahorros de quien prospera. La Mutua
de Fletadores asegura los cascos y las averías con una prima que sale de las pérdidas reales. En paz la
flota crece y la escasez baja; con piratas el seguro se encarece, los comerciantes se descapitalizan y la
escasez se dispara. Las patrullas abaratan el seguro.

Y tú puedes pasar de nave a empresa. En el astillero de cualquier estación compras Mineros, Cargueros y
Escoltas con tu cuenta y con crédito del banco (hasta el 75 % del valor de tu flota), y la Mutua los asegura
con una prima que se ajusta a tu historial. Tus capitanes siguen órdenes permanentes: minar un campo del
cinturón de asteroides y vender donde más rinde el ciclo, comerciar por su cuenta, atracar o escoltar a otra
nave. También puedes tomar el mando de cualquiera y minar a mano. Cada minero baja el precio del mineral, y
los corsarios acechan los campos donde ven naves: en guerra, una escolta decide si la minería es rentable.

Por debajo están el kernel de M1 (entidades, eventos, comandos, save/load, replay, LOD por frecuencia) y la
fase 0. El diseño es provisional
([docs/DESIGN.md](docs/DESIGN.md)) hasta que exista `game.md`. El nombre y el namespace (`gx`) también son
provisionales.

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

## Publicar en Windows (Steam)

```powershell
./scripts/package.ps1                     # release + tests + dist/GalaxyEngine (el depot) + zip + prueba de humo
./scripts/steam-upload.ps1 -AppId <app> -DepotId <app+1> -Username <cuenta> -Branch beta
```

El jugador recibe un `gx_game.exe` sin consola y sin dependencias (CRT estático) y las licencias de SDL3 y
Dear ImGui. Sus partidas y su log van a `%APPDATA%\GalaxyEngine\Sandbox`. La CI de GitHub Actions compila y
prueba en Windows (MSVC) en cada push. Guía completa, y lo que falta para la tienda, en
[docs/STEAM.md](docs/STEAM.md).

## Jugar

```powershell
./build/release/bin/gx_game.exe                 # sistema de la semilla 2400
./build/release/bin/gx_game.exe --seed 77       # otro sistema
```

Controles: rueda = zoom · arrastrar = mover · clic = seleccionar · **clic derecho = ir allí** ·
WASD = empuje manual · X = frenar · E = atacar el contacto seleccionado · C = alto el fuego · B = abordar · K = contratos ·
R/T = radar/transpondedor · ventana Mercado al atracar · L = flota y empresa (astillero, órdenes, cuentas) ·
Espacio = pausa · 1/2/3/4 = ×1 (tiempo real)/×3/×10/×30 · H/F = seguir ·
F5/F9 = guardar/cargar · F11 = pantalla completa · Esc = deseleccionar o pausa · F3 = depuración · F1 = ayuda.
El juego empieza en el menú principal (continuar, nueva partida con semilla, opciones); las opciones se
guardan en `%APPDATA%\GalaxyEngine\Sandbox\settings.ini`. Idiomas: español e inglés, según el del sistema o
el que elijas en Opciones (catálogos en `data/lang`, ver ADR-036; `python tools/i18n.py check data/lang/en.po`
comprueba que todo esté traducido). La primera configuración descarga SDL3 y Dear ImGui
(versiones fijadas); `-DGX_BUILD_CLIENT=OFF` compila sin el cliente.

Modo captura para comprobaciones automáticas:
`gx_game.exe --frames 60 --prerun-hours 30 --select --screenshot captura.png` (`--no-help` cierra la ayuda;
`--menu main|pause|options` captura los menús; `--demo-fleet` compra una flota de ejemplo y
`--fleet-tab fleet|books|yards` abre la ventana de la empresa; `--select-field` selecciona un campo de
asteroides; `--data-dir` usa otra carpeta de datos). Hay un combate reproducible
con `gx_game.exe --seed 1 --fly-to 1 --prerun-hours 0.0745 --engage-nearest --zoom 1100 --frames 20
--screenshot combate.png`.

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

# El sandbox jugable sin ventana: estadísticas de tráfico y combate, estado de los piratas y diario
./build/release/bin/gx_headless.exe --sandbox --minutes 180 --seed 7
./build/release/bin/gx_headless.exe --sandbox --minutes 20 --seed 1 --fly-to 1 --journal
./build/release/bin/gx_headless.exe --sandbox --minutes 600 --pirates 0 --markets   # economía por puerto
./build/release/bin/gx_headless.exe --sandbox --minutes 600 --patrols 0 --dump      # sin patrullas; estado final
./build/release/bin/gx_headless.exe --sandbox --minutes 480 --no-finance            # M3.4: reemplazos gratis
./build/release/bin/gx_headless.exe --sandbox --minutes 120 --journal --lang en     # el diario, en inglés
# La empresa del jugador (M4): mineros, cargueros y escoltas con órdenes; cuentas por nave al final
./build/release/bin/gx_headless.exe --sandbox --minutes 480 --pirates 0 --miners 2 --company-credits 20000
./build/release/bin/gx_headless.exe --sandbox --minutes 480 --miners 3 --escorts 1 --company-credits 30000

./build/release/bin/gx_headless.exe --help
```

## Estructura

```text
Engine/        Core, Memory, Math, Jobs, Time, Profiling, Serialization
Simulation/    Kernel (scheduler, paso, save/load), World (entidades), Events, Commands, Economy
Space/         Orbits (Kepler), Bodies, Ships (vuelo, autopiloto, módulos), Sensors, Combat, Generation
Game/          Sandbox (escenario jugable, empresa y flota del jugador, contenido provisional), Presentation
Scenarios/     cargas sintéticas para tests y benchmarks (no contenido de juego)
Apps/          gx_game (cliente SDL3 + ImGui), gx_headless
ThirdParty/    SDL3 y Dear ImGui (FetchContent con hash)
Benchmarks/    gx_bench
Tests/         gx_tests (20 suites en CTest)
docs/          DESIGN.md, ARCHITECTURE.md, DECISIONS.md, BENCHMARKS.md
cmake/ scripts/ tools/
```

## Documentación

- [docs/DESIGN.md](docs/DESIGN.md): diseño de juego provisional (escala, generación, vuelo, naves, sensores,
  combate, economía, NPC, controles).
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): capas, tiempo, pipeline, presentación, threading,
  entidades, eventos, comandos, persistencia, determinismo, LOD, referencias, contradicciones y riesgos.
- [docs/DECISIONS.md](docs/DECISIONS.md): registro de decisiones (ADR-001 a ADR-036).
- [docs/BENCHMARKS.md](docs/BENCHMARKS.md): método y resultados medidos por hito.
- [docs/STEAM.md](docs/STEAM.md): empaquetado para Windows, subida a Steam y lo que falta para la tienda.
