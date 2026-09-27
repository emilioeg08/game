# Diseño provisional del juego

> **No existe `game.md`.** Por decisión del usuario (2026-09-26), el diseño lo decido yo a partir del prompt
> maestro y de las 4 capturas de referencia, y lo documento aquí para revisarlo. Todo lo de este documento es
> **provisional**: cuando exista `game.md`, prevalece. Los valores numéricos están en tablas de contenido
> (`Game/Sandbox/Content.h`) o en parámetros de generación, no dispersos por los sistemas, para que moverlos
> a ficheros de datos (modding) sea un cambio local.

## Estado: slice M3 "Economía mínima y comercio"

Se puede:

- empezar una partida en un sistema generado y pilotar la nave (autopiloto con salto al hiperespacio, o
  empuje manual);
- jugar en tiempo real con aceleración ×3 o ×10, guardar y cargar, e inspeccionar cualquier entidad;
- detectar con **sensores y niebla de guerra** (M2.2);
- **combatir** (M2.3): armas que disparan a pistas de sensores, daño por módulos, piratas que cazan
  cargueros, reparaciones y reapariciones;
- **comerciar** (M3): cada puerto tiene un mercado con producción, consumo y precios que siguen a las
  existencias; los cargueros compran y venden por beneficio, y el jugador también.

Con esto el primer vertical slice del prompt (§5) está completo.

**Todavía no hay:** salarios, impuestos, crédito ni quiebras; misiles ni defensa puntual; facciones
políticas; más de un sistema estelar.

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
| Corsario | pirata NPC | 40 km/s² / 12.000 km/s | 1.200.000 km/s (4 c) | 6 s | ~125 s |

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
| Corsario | 2e3 | +1,5e6 | 3e3 m² | 1e12 | sí |

## Combate (ADR-024, ADR-025)

**Módulos.** Una nave no es una barra de vida, sino un conjunto de módulos con su propia salud (como en el
editor de naves de las referencias). La estructura va primero: si llega a cero, la nave se destruye.

| Diseño | Estructura | Reactor | Motor | Hipermotor | Sensores | Armas | Otros |
|---|---|---|---|---|---|---|---|
| Correo | 500 | 150 | 150 | 120 | 100 | láser 80, riel 80 | habitáculo 80 |
| Carguero | 600 | 150 | 200 | 150 | 80 | — | bodega 600, habitáculo 150 |
| Corsario | 400 | 120 | 150 | 120 | 80 | 2 láseres 80, riel 80 | habitáculo 80 |

**Daño.**

- Cada impacto hace dos cosas: la estructura absorbe la mitad, y un módulo elegido al azar en proporción
  a su tamaño recibe el impacto completo. Si ese módulo ya está destrozado, su parte pasa a la estructura.
- Por debajo del 10 % un módulo queda fuera de servicio.
- Sin reactor, la nave queda **sin energía**: no tiene motor, ni hipermotor, ni armas, ni radar, y va a la
  deriva.
- Cuando el reactor queda fuera de servicio, hay un 30 % de probabilidad de brecha, que destruye la nave.
- Motor y sensores escalan con su salud. El hipermotor no funciona por debajo del 50 %, y un salto en curso
  se aborta si se pierde.

**Armas.**

| Arma | Tipo | Alcance | Daño | Notas |
|---|---|---|---|---|
| Láser | haz (instantáneo) | 800 km | 4 por segundo en el blanco | acierta si el error de puntería es menor que el casco |
| Cañón de riel | proyectil a 5.000 km/s | 3.000 km | 15 por impacto | recarga de 2 s; el proyectil vuela en línea recta |

**Control de tiro.**

- Se dispara a **pistas de sensores**, no a naves.
- Para disparar hace falta que tu facción tenga el contacto y que tus propios sensores lo fijen (SNR ≥ 1).
  A un fantasma no se le puede disparar.
- El error de puntería suma tres cosas: el error de medida (d/√SNR, pequeño con radar), el error del arma,
  y lo que el blanco acelera durante la latencia del control de tiro (50 ms).
- Un Correo que acelera a fondo esquiva parte de los láseres. Los proyectiles se apuntan suponiendo
  velocidad constante, así que a largo alcance una nave que acelera los esquiva.
- El daño está ajustado para que un duelo dure alrededor de un minuto a ×1. Da tiempo para huir, maniobrar
  o apagar el radar. Medido: un corsario destruye al Correo, que no responde, en 52 s.

**Reparaciones y pérdidas.**

- Atracado en una **estación**, todo se repara al 2 % por segundo.
- Fuera de puerto, el control de daños lleva el reactor y el motor hasta el 25 %, a 0,25 % por segundo,
  tras 10 s sin recibir impactos. Un reactor destrozado vuelve a dar energía en unos 50 s.
- Si destruyen tu nave, a los 10 s te espera una nueva en la estación principal.
- Los cargueros perdidos se reponen al ritmo de uno por minuto, y los piratas de uno cada 3 minutos (el
  sistema no es una caja cerrada).

## Economía (ADR-027, ADR-028)

**Cadena de suministro.** En cada sistema, los planetas extraen según su tipo; las estaciones fabrican:

| Puerto | Produce | Con | Consume (población) |
|---|---|---|---|
| Planeta helado | agua | mantenimiento: maquinaria | alimentos, combustible |
| Planeta oceánico | alimentos | mantenimiento: maquinaria | combustible |
| Planeta rocoso / desértico | mineral | mantenimiento: maquinaria | alimentos, agua |
| Gigante gaseoso | combustible | mantenimiento: maquinaria | alimentos, agua |
| Estación refinería | metales | 2 t de mineral + 0,5 t de combustible por t | alimentos, agua |
| Estación fábrica | maquinaria | 2 t de metales + 0,5 t de combustible por t | alimentos, agua |

- Sin insumos esenciales no hay producción.
- Sin mantenimiento (maquinaria), la extracción cae al 35 %: la cadena se cierra sobre sí misma.
- Al generar el sistema, cada bien se produce en total un 20 % por encima de lo que se consume. Si un
  bien no lo puede extraer ningún planeta, la primera estación lo fabrica (hidroponía, recicladoras). Así
  la escasez local depende del transporte y de los piratas, no de la suerte del generador.

| Bien | Precio base (cr/t) |
|---|---|
| Agua | 20 |
| Alimentos | 40 |
| Mineral | 30 |
| Combustible | 50 |
| Metales | 120 |
| Maquinaria | 300 |

**Precios.**

- Cada puerto guarda un **objetivo de existencias**: 4 horas de su producción o de su consumo, el mayor
  de los dos. Su capacidad es el triple.
- Precio medio = base · 2^(1,5 · (1 − existencias/objetivo)): la base en el objetivo, ×2,83 con el almacén
  vacío y ×0,21 con 2,5 veces el objetivo.
- El puerto vende un 4 % por encima y compra un 4 % por debajo.
- Las operaciones se cobran tonelada a tonelada, así que un pedido grande mueve el precio en su contra.

**Conocimiento.** Nadie lee un mercado remoto:

- Cada facción tiene un **libro de precios** con la edad de cada observación. Al empezar, todos tienen el
  boletín inicial del sistema.
- Los comerciantes independientes comparten una red: cada carguero que atraca actualiza los precios de
  todos.
- El jugador actualiza los precios del puerto donde atraca, y las **estaciones le dan el boletín** de los
  comerciantes.

**Cargueros comerciantes.** Al salir de puerto, cada carguero:

1. Si lleva carga, va donde mejor se vende.
2. Si no, compra aquí lo que más rinde en otro puerto: beneficio por segundo de viaje, descontando precios
   viejos (a la mitad cada 30 min) y el peligro del destino.
3. Si aquí no hay nada rentable, va vacío a donde sabe que hay una ganga (reposicionamiento).
4. Si tampoco, va a refrescar los precios más viejos.

Al estimar lo que obtendrá, sigue la curva de precios real y cuenta con las entregas que la red ya tiene
en camino a ese puerto. Así varios cargueros no hunden a la vez el mismo mercado.

El **peligro** de un puerto sube cada vez que muere un carguero que iba hacia él y se olvida a la mitad
cada 20 minutos.

**Causalidad medida** (10 h, 12 cargueros, con y sin 3 piratas):

| Semilla | Entregas | Escasez de agua | Escasez de alimentos |
|---|---|---|---|
| 2400 | 16.949 → 11.680 t (−31 %) | 256 → 472 t | 238 → 634 t |
| 7 | 14.977 → 11.476 t (−23 %) | 132 → 712 t | 58 → 167 t |

**Tú.**

- Empiezas con 2.000 cr y 20 t de bodega (los cargueros llevan 100 t).
- Atracado en un puerto, compras y vendes en la ventana **Mercado**.
- Los precios verdes son baratos y los rojos caros, respecto al precio base.
- Seleccionando un puerto ves los precios que conoces y su antigüedad.
- Si te destruyen, pierdes la carga pero no los créditos.

## Facciones y personajes (mínimo)

- **Jugador** (verde), **Transportistas independientes** (azul) y **Piratas** (rojo). La política, las
  Casas y los títulos llegarán en la fase 5.
- Nave del jugador: "Errante" (Correo). Cargueros: nombre + número ("Faro-12", "Nómada-47"). Corsarios:
  "Colmillo-66", "Sombra-86"...

## Comportamiento de los NPC

- Regla simple y reproducible, sin LLM (prompt §16): esperar en puerto de 30 s a 3 min, elegir **otro** puerto
  al azar (flujo aleatorio por nave y viaje), volar (con salto) y atracar, y repetir.
- La galaxia actúa sin el jugador: los cargueros viajan y atracan aunque el jugador no haga nada, y cada
  llegada queda en el diario.
- Un carguero atacado huye a la estación más cercana, donde los piratas no se atreven a entrar.
- Desde M3 los cargueros comercian: eligen carga y destino por beneficio con lo que sabe su red (ver
  Economía).

**Piratas (ADR-026).** Deciden solo con la imagen de sensores de su facción, nunca con la verdad:

- **Acecho:** esperan justo fuera del pozo de un planeta, que es donde los cargueros salen del
  hiperespacio. Van en silencio (sin transpondedor ni radar) y cada 4 minutos cambian a uno de los 3
  planetas más cercanos.
- **Caza:** eligen la pista clasificada más cercana, siempre que no sea un corsario y esté a menos de
  300.000 km. Descartan las pistas en hiperespacio y las que están a menos de 50.000 km de una estación.
  Encienden el radar, persiguen la pista a 200 km y disparan.
- **Abandono de la caza:** abandonan si pierden la pista, si la presa se aleja más de 600.000 km o llega a
  una estación, o a los 3 minutos (y entonces descartan esa pista).
- **Huida:** con la estructura por debajo del 40 % o sin armas, salen del sistema por las afueras y
  desaparecen de la simulación.
- El jugador con el transpondedor encendido es tan presa como un carguero. Apagarlo y no acelerar cerca de
  los pozos es la forma de pasar desapercibido.
- **Medido** (3 h, 12 cargueros, 3 piratas, 4 semillas): de 30 a 61 cacerías y de 3 a 7 cargueros
  perdidos. Es decir, uno o dos por hora.

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
| Atacar el contacto seleccionado (lo persigue a 300 km y dispara) | E |
| Alto el fuego | C |
| Interceptar sin disparar | botón en la selección del contacto |
| Comprar / vender | ventana Mercado, atracado en un puerto (+1, +10, −1, Todo) |
| Pausa / velocidad | Espacio / 1 (×1), 2 (×3), 3 (×10) |
| Seguir tu nave / la selección | H / F |
| Guardar / cargar | F5 / F9 |
| Depuración / ayuda | F3 / F1 |

## Lenguaje visual

- Mapa 2D oscuro con órbitas tenues, colores por tipo de cuerpo, triángulos orientados por la velocidad
  para las naves, llama de empuje, línea de rumbo, barra de escala en km o UA, y etiquetas según el zoom.
- Paneles: tiempo (arriba), nave (izquierda, con barras de salud por módulo y estado de cada arma: fijado,
  sin fijación, fuera de alcance o recargando), selección con inspector (derecha), diario (abajo a la
  izquierda) y depuración (F3).
- Combate: los haces son cian (los tuyos) o naranjas (hostiles), y tenues cuando fallan. Los proyectiles
  son puntos con estela, y las explosiones, anillos que se expanden. El blanco de tus armas lleva una
  retícula roja. Solo ves el fuego a menos de 10.000 km de tu nave y las explosiones a menos de 5 millones
  de km, salvo en la vista de depuración.
- Referencias: solo conceptuales. La interfaz, los nombres y los assets son originales.

## Pendiente de decidir (para `game.md` o para próximos slices)

1. Consecuencias de apagar el transpondedor (ley, reputación) y guerra electrónica.
2. Misiles, defensa puntual, blindaje o escudos, y abordaje con botín de las naves sin energía.
3. Consecuencias de atacar a cargueros o a otras facciones: reputación, policía de las estaciones y
   recompensas por piratas.
4. Economía completa (§18): salarios, impuestos, crédito, deuda, seguros, inversión y quiebra. Ahora el
   dinero de los comerciantes solo crece; faltan sumideros (costes de operación).
5. Población con efectos: la escasez debería afectar a la estabilidad y al crecimiento.
6. Viaje entre sistemas y la estructura de la galaxia.
7. Nombre del juego, tono y estética definitiva.
