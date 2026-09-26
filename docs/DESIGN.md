# Diseño provisional del juego

> **No existe `game.md`.** Por decisión del usuario (2026-09-26), el diseño lo decido yo a partir del prompt
> maestro y de las 4 capturas de referencia, y lo documento aquí para revisarlo. Todo lo de este documento es
> **provisional**: cuando exista `game.md`, prevalece. Los valores numéricos están en tablas de contenido
> (`Game/Sandbox/Content.h`) o en parámetros de generación, no dispersos por los sistemas, para que moverlos
> a ficheros de datos (modding) sea un cambio local.

## Estado: slice M2.1 "Sistema estelar jugable"

Se puede: empezar una partida en un sistema generado, pilotar la nave (autopiloto con salto al
hiperespacio, o empuje manual), ver a los cargueros NPC viajar solos entre puertos, jugar en tiempo real
con aceleración ×3 o ×10, guardar y cargar, e inspeccionar cualquier entidad, con **sensores y niebla de guerra** (M2.2). **Todavía no hay:** combate, daño, economía jugable,
facciones políticas ni más de un sistema estelar (slices M2.2 en adelante).

## Escala, unidades y tiempo

| Decisión | Valor | Motivo |
|---|---|---|
| Unidades | metros, segundos, kg | físicas; la presentación convierte a km, km/s y UA |
| Marco de referencia | inercial centrado en la estrella; plano de la eclíptica = mapa | zoom continuo como en las referencias |
| Año de época | 2400 (`kEpochYear`) | futuro lejano; original |
| Calendario | 12 meses gregorianos, años de 365 días | fechas legibles ("2400-03-01"), determinismo trivial |
| Velocidades del tiempo | pausa, **×1 = tiempo real**, ×3, ×10 | directiva del usuario (ADR-021): se juega en tiempo real, como un simulador espacial; los viajes duran minutos |
| Consecuencia | el calendario avanza a ritmo real (×10 como máximo) | la escala de la economía, la política y las generaciones se diseñará para esta velocidad en M3–M5 |

## Sistema estelar (generación procedural)

- **Semilla** → estrella de 0,6 a 1,4 masas solares; luminosidad ∝ masa^3,5.
- **Planetas**: de 4 a 9; el primero entre 0,3 y 0,5 UA·√L y cada siguiente ×1,45–2,0 más lejos
  (parecido a los sistemas reales, sin copiar ninguno).
- **Tipo según la distancia**: rocosos y desérticos dentro; oceánicos en la zona habitable (0,95–1,4 UA·√L);
  gigantes gaseosos y helados más allá de la línea de hielo (2,7 UA·√L). Radios y densidades según el tipo.
- **Lunas**: 1–4 en gigantes, 0–2 en helados, 0–1 en rocosos, siempre dentro del 30 % de la esfera de Hill.
- **Estaciones**: el puerto principal orbita el planeta más habitable y la segunda estación un gigante
  gaseoso (o el último planeta). Así hay dos polos de tráfico.
- **Nombres**: inventados con sílabas propias (estrella), numeración romana (planetas), letras (lunas) y
  títulos en español para las estaciones ("Puerto", "Estación", "Atalaya", "Muelle", "Enclave", "Relé").
  Nunca se usan catálogos reales ni nombres de otros juegos.
- **Órbitas**: raíles keplerianos analíticos. Los cuerpos no se integran y no derivan.

## Modelo de vuelo (ADR-022)

- **Dos motores**, como en las referencias: **sublumínico** para maniobrar y atracar (newtoniano), e
  **hiperespacio** para cruzar el sistema (millones de km/s, trayecto recto).
- **Pozos gravitatorios**: estrella = 20 radios y planetas = 25 radios (círculos discontinuos en el mapa).
  Dentro no se puede saltar, y el salto termina en el borde del pozo del destino con la velocidad igualada
  a la suya.
- **El autopiloto decide solo**: si el tramo en hiperespacio supera 1 millón de km y la nave está fuera de
  un pozo, carga el salto (5–12 s, sin dejar de avanzar), salta, sale junto al destino y termina en
  sublumínico. Cualquier otra orden cancela la carga o provoca una salida de emergencia.
- **Newtoniano con impulsor de ciencia ficción** (sublumínico): la nave tiene inercia y un empuje máximo
  alto; si no hay empuje, sigue derivando. **Sin gravedad sobre las naves** en este slice (sí sobre los cuerpos, vía
  órbitas): con aceleraciones de cientos de g la gravedad planetaria es despreciable en viaje, y aplicarla
  complicaría el pilotaje sin aportar juego todavía. Se reconsiderará con combate orbital.
- **Velocidad de crucero**: límite del autopiloto, no físico. Con empuje manual se puede superar.
- **Autopiloto**: modos Deriva, Detenerse, Ir a punto, Aproximación (a un cuerpo o una nave, manteniendo una
  distancia de seguridad) y Manual. El perfil acelera, navega a crucero y frena al 80 % del empuje máximo, y
  nunca se pasa del objetivo.
- **Distancias de seguridad**: estaciones a 5 km, naves a 2 km, planetas y lunas a 1,25 radios + 200 km, y la
  estrella a 3 radios.
- **Mantener posición** junto a un cuerpo que orbita exige empujar de forma continua (se ve la llama): es
  el precio de no tener gravedad sobre las naves.
- **LOD del vuelo**: pasos de 200 ms (estratégico) mientras nadie pilota a mano, y de 50 ms (táctico)
  mientras el jugador usa el empuje manual. El cambio se hace dentro de la simulación, por comando, y es
  determinista.

### Clases de nave

| Clase | Rol | Sublumínico: empuje / crucero | Hiperespacio | Carga del salto | 1 UA en hiperespacio |
|---|---|---|---|---|---|
| Correo | nave del jugador | 50 km/s² / 15.000 km/s | 1.500.000 km/s (5 c) | 5 s | ~100 s |
| Carguero | transporte NPC | 15 km/s² / 6.000 km/s | 600.000 km/s (2 c) | 12 s | ~250 s |

Las referencias muestran velocidades sublumínicas de ~20.000 km/s y velocidades de hiperespacio de más de
un millón de km/s. Con ×1 = tiempo real, un viaje entre planetas dura de 3 a 6 minutos a ×1 (menos de un
minuto a ×10).

## Sensores (ADR-023)

- **Nadie es omnisciente**: solo ves tu flota y lo que detectan tus sensores. Los cuerpos celestes sí se
  conocen (cartas del sistema).
- **Pasivo** (siempre activo): detecta según la emisión del otro. Una nave quieta es casi invisible (a unos
  22.000 km para los sensores de un carguero), una que acelera fuerte se ve a unos 700.000 km, y cualquier
  salto al hiperespacio se ve desde casi todo el sistema.
- **Radar (R)**: identifica un carguero a unos 3,5 millones de km con buena precisión, pero su emisión te
  hace visible a unos 7 millones de km.
- **Transpondedor (T)**: difunde tu identidad y posición hasta 1 UA. Los cargueros lo llevan encendido. Si
  lo apagas, cuesta mucho más identificarte (más adelante tendrá consecuencias legales).
- **Contactos**: desconocido (?), clasificado ("Carguero?") o identificado (nombre), con círculo de
  incertidumbre. Si dejan de detectarse se atenúan y se pierden a los 20 s. El ruido crea a veces contactos
  fantasma que desaparecen.
- **Ir a un contacto** lleva a su posición estimada, no a la real.

| Clase | Emisión base | Emisión a pleno empuje | Sección radar | Sensibilidad pasiva | Radar |
|---|---|---|---|---|---|
| Correo | 1e3 | +1e6 | 1e3 m² | 1e12 | sí |
| Carguero | 5e3 | +2e6 | 1e4 m² | 5e11 | no |

## Facciones y personajes (mínimo)

- **Jugador** (verde) y **Transportistas independientes** (azul). La política, las Casas y los títulos
  llegarán en la fase 5.
- Nave del jugador: "Errante" (Correo). Cargueros: nombre + número ("Faro-12", "Nómada-47").

## Comportamiento de los NPC (cargueros)

- Regla simple y reproducible, sin LLM (prompt §16): esperar en puerto de 30 s a 3 min, elegir **otro** puerto
  al azar (flujo aleatorio por nave y viaje), volar (con salto) y atracar, y repetir.
- La galaxia actúa sin el jugador: los cargueros viajan y atracan aunque el jugador no haga nada, y cada
  llegada queda en el diario.
- Próximo paso (M3, economía): los destinos dependerán de oferta, demanda y precios (utility AI) y la carga
  será real.

## Controles del cliente

| Acción | Control |
|---|---|
| Zoom (de metros a UA) | rueda del ratón |
| Mover la vista | arrastrar con el botón izquierdo |
| Seleccionar | clic |
| Ir a un objeto o a un punto | clic derecho |
| Empuje manual | W A S D (arriba = +y del mapa) |
| Frenar | X |
| Radar / transpondedor | R / T |
| Pausa / velocidad | Espacio / 1 (×1), 2 (×3), 3 (×10) |
| Seguir tu nave / la selección | H / F |
| Guardar / cargar | F5 / F9 |
| Depuración / ayuda | F3 / F1 |

## Lenguaje visual

- Mapa 2D oscuro con órbitas tenues, colores por tipo de cuerpo, triángulos orientados por la velocidad
  para las naves, llama de empuje, línea de rumbo, barra de escala en km o UA, y etiquetas según el zoom.
- Paneles: tiempo (arriba), nave (izquierda), selección con inspector (derecha), diario (abajo a la
  izquierda) y depuración (F3).
- Referencias: solo conceptuales. La interfaz, los nombres y los assets son originales.

## Pendiente de decidir (para `game.md` o para próximos slices)

1. Consecuencias de apagar el transpondedor (ley, reputación) y guerra electrónica.
2. Armas, defensa puntual, misiles y daño por módulos (M2.3–M2.4, según las referencias del editor de nave).
3. Bienes, producción y precios de los puertos (M3).
4. Hiperespacio o viaje entre sistemas y la estructura de la galaxia.
5. Nombre del juego, tono y estética definitiva.
