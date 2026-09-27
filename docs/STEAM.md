# Publicar en Steam (Windows)

El juego se publica para **Windows x64** en Steam. Este documento explica cómo se construye lo que reciben
los jugadores, cómo se sube y qué falta todavía para poder lanzar en la tienda (ADR-034).

## 1. Lo que reciben los jugadores

`./scripts/package.ps1` compila en Release, ejecuta los tests, instala el componente `client` y arranca el
ejecutable empaquetado una vez (60 fotogramas y una captura) para comprobar que funciona:

```text
dist/GalaxyEngine/                        ← el depot de Steam: esto es lo que se instala
    gx_game.exe                           ← sin consola, CRT estático: no hace falta el redistribuible de VC++
    licenses/SDL3.txt, licenses/DearImGui.txt
dist/symbols/gx_game.pdb                  ← para leer volcados de errores; NUNCA se sube al depot
dist/GalaxyEngine-<versión>-windows-x64.zip
```

- **Requisitos del jugador:** Windows 10 u 11 de 64 bits y una GPU con Direct3D 11 (SDL3 recurre a otros
  renderizadores si no la hay).
- **Datos del jugador:** `%APPDATA%\GalaxyEngine\Sandbox\` guarda `saves\quicksave.gxsave`,
  `logs\gx_game.log` (la última sesión, para informes de errores) e `imgui.ini`. Nada se escribe junto al
  ejecutable: la carpeta de instalación puede ser de solo lectura.
- `--data-dir <carpeta>` cambia esa carpeta (útil para pruebas o para una versión portable).
- **Rutas con acentos:** perfiles como `C:\Users\José` funcionan. Las rutas se construyen desde UTF-8 y el
  manifiesto fija UTF-8 como página de códigos del proceso (hay test).
- **Pantalla completa:** F11 o Alt+Intro (ventana sin bordes a la resolución del escritorio).

**CI.** El workflow `Windows` (GitHub Actions, `windows-2022`, MSVC) hace todo lo anterior en cada push y
guarda el paquete como artefacto `GalaxyEngine-windows-x64`. Un paquete que no arranca no llega a Steam.

## 2. Una sola vez: dar de alta el juego en Steamworks

1. Crea una cuenta de socio en <https://partner.steamgames.com>: datos bancarios y fiscales, y verificación
   de identidad.
2. Paga la tarifa de **Steam Direct** por el juego (100 USD, recuperable a partir de 1.000 USD de ingresos).
   Recibes un **App ID**.
3. En *App Admin → SteamPipe → Depots*, el depot de Windows es el App ID + 1 (el que crea Steamworks por
   defecto). Márcalo como Windows y 64 bits.
4. En *Installation → General*, añade la opción de arranque: ejecutable `gx_game.exe`, sistema operativo
   Windows y arquitectura 64 bits.
5. **Steam Cloud** (opcional, recomendado): en *Steam Cloud → Auto-Cloud*, una ruta raíz `WinAppDataRoaming`
   con el subdirectorio `GalaxyEngine/Sandbox/saves`, el patrón `*.gxsave` y sin recursividad. Esa carpeta
   **no puede cambiar** una vez publicado el juego: la fijan `kDataOrganization` y `kDataApplication` en
   `Apps/Game/GameApp.cpp`. Hay que decidir el nombre definitivo del juego antes del primer lanzamiento
   (ver 5).
6. Crea una cuenta de Steam **solo para subir builds**, con permiso de *Edit App Metadata* y *Publish App
   Changes To Steam* únicamente para esta app. No uses tu cuenta personal en la CI.

## 3. Subir una build

**Desde tu PC** (con [SteamCMD](https://developer.valvesoftware.com/wiki/SteamCMD) en el PATH):

```powershell
./scripts/package.ps1
./scripts/steam-upload.ps1 -AppId <app> -DepotId <app+1> -Username <cuenta_de_builds> -Branch beta
./scripts/steam-upload.ps1 -AppId <app> -DepotId <app+1> -Username <cuenta_de_builds> -Preview  # solo comprueba
```

SteamCMD pide la contraseña y el código de Steam Guard: nunca pasan por el script. Las plantillas están en
`tools/steam/`, y los `.pdb` quedan excluidos del depot.

**Desde GitHub Actions** (workflow manual `Steam upload`): compila, prueba y hace el smoke test en Windows, y
después sube el paquete con `game-ci/steam-deploy`. Hay que configurarlo una vez en el repositorio:

- variable `STEAM_APP_ID`;
- secretos `STEAM_USERNAME` y `STEAM_CONFIG_VDF`: el `config.vdf` de SteamCMD tras iniciar sesión con la
  cuenta de builds, codificado en base64, según las instrucciones de `game-ci/steam-deploy`;
- entorno `steam`, con aprobación manual si quieres un segundo control.

**La rama por defecto no se publica desde aquí:** Valve no lo permite desde SteamCMD. Sube a `beta`, pruébala
desde el cliente de Steam (Propiedades → Betas) y publícala en *SteamPipe → Builds*.

## 4. Antes de publicar una build

- [ ] El workflow `Windows` está en verde en ese commit: tests en debug y release y smoke test del paquete.
- [ ] Probado desde Steam en la rama beta, en un PC limpio (sin Visual Studio), incluido un usuario con
      acentos en el nombre.
- [ ] Guardar (F5), salir, volver a entrar y cargar (F9); con Steam Cloud, en un segundo PC.
- [ ] `dist/symbols/gx_game.pdb` archivado junto a la versión, para poder leer los volcados de ese build.

## 5. Lo que falta para lanzar en la tienda

Lo técnico está resuelto: se compila, se empaqueta, arranca y se sube. Lo que falta es producto:

| Área | Estado | Qué hace falta |
|---|---|---|
| Nombre y marca | "GalaxyEngine" es provisional (es el nombre del motor) | nombre definitivo, comprobado en marcas registradas y en Steam; fijar la carpeta de datos antes del primer lanzamiento |
| Menú principal y opciones | se entra directo al sandbox | menú, opciones de vídeo (ventana, pantalla completa, escala de UI) y de controles, salir con confirmación |
| Audio | no hay | música y efectos (y su volumen) |
| Idiomas | solo español | inglés como mínimo para el mercado de Steam; los textos deben salir del código a tablas |
| Tutorial | ventana de controles (F1) | primeros pasos guiados |
| Varias partidas | un solo guardado rápido | varias ranuras, autoguardado |
| Mando / Steam Deck | solo teclado y ratón | se puede publicar sin ello; indicarlo en la tienda |
| Steamworks SDK | no integrado (no es obligatorio) | logros, overlay y presencia: integrar `steam_api64.dll` detrás de una capa opcional |
| Firma de código | sin firmar | recomendable: Windows SmartScreen y Smart App Control avisan o bloquean los ejecutables sin firmar (ARCHITECTURE, riesgo 2) |
| Página de tienda | — | cápsulas e imágenes en los tamaños de Steam, capturas, tráiler, descripción, requisitos, cuestionario de contenido y clasificación por edades (IARC) |
| Revisión de Valve | — | página "Próximamente" al menos 2 semanas antes del lanzamiento; revisión de la página y de la build (unos días cada una) |
